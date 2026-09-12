// ---------------------------------------------------------------------------
// perceptiond — the Perception Adaptive Application.
//
// InferencePipeline (AutoDrive / AutoSteer / AutoSpeed) plus LongitudinalFusion
// and LateralFusion. Adapted from the middle of upstream app/vision_pilot.cpp:
//
//     if (const auto r = pipeline.process(warped, resized)) { ... }
//
// Consumes frames by reference, publishes the fused lane-relative state. It
// never sees a sensor device and never computes a command.
//
// DO NOT split this executable further. AD, AS and ASp each cost 55-62 ms yet
// the measured `parallel` is 61.9 ms, because they run concurrently on one
// shared preprocessed frame. One AA per network would serialise them across
// process boundaries and turn that maximum back into a sum.
//
// The TrajectoryService required port is DEBUG ONLY: visualization draws the
// plan on the frame, the frame is here, and the frame must not go on the wire.
// The overlay is one cycle behind, which is correct for a debug view and
// irrelevant to control. Nothing in inference, fusion or the published
// DrivingState reads it — delete the port and the only casualty is the overlay.
// ---------------------------------------------------------------------------
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include <opencv2/core.hpp>

#include <common/types.hpp>
// Order matters. Upstream declares find_config() twice: common/utils.hpp
// declares one with external linkage, config/vision_pilot_config.hpp
// DEFINES a static one in the header. The two bodies are identical, but
// extern-then-static is an error while static-then-extern is legal (the
// later declaration inherits internal linkage). No upstream TU includes
// both, so this only shows up here.
#include <config/vision_pilot_config.hpp>
#include <common/utils.hpp>
#include <engine/onnx_engine.hpp>
#include <logging/logger.hpp>
#include <models/inference.hpp>
#include <visualization/visualization.hpp>

#include <ap_runtime/ap_runtime.hpp>
#include "../common/phase.hpp"
#include "../common/frame_ring.hpp"

#include <ara/core/instance_specifier.h>
#include <ara/core/promise.h>
#include <ara/com/types.h>

#include <av/vp/cm/cameraframeservice_proxy.h>
#include <av/vp/cm/drivingstateservice_skeleton.h>
#include <av/vp/cm/trajectoryservice_proxy.h>

namespace ap = visionpilot::ap;
namespace ve = visionpilot::engine;
namespace vm = visionpilot::models;

namespace {

constexpr char kDrivingStatePort[] = "perceptiond/pc_root/PPort_DrivingState";

// Tracker-based CIPO gate, lifted verbatim from upstream main(): true only when
// the filter tracks a target closer than D_MAX. cipo_raw_found alone must not
// gate the planner. This is a perception judgement about its own tracker, which
// is why it lives here and not in planningd.
constexpr double kCipoMaxDistanceM = 150.0;

}  // namespace

int main(int argc, char** argv)
{
    const vpap::Phase phase = vpap::parse_phase(argc, argv);
    VP_INFO("perceptiond: starting, phase=%s", vpap::to_string(phase));

    Config cfg;
    try { cfg = load_vision_pilot_config(); }
    catch (const std::exception& e) { VP_ERROR("Config: %s", e.what()); return 1; }

    ap::ApRuntime runtime("");
    if (!runtime.ok()) { VP_ERROR("perceptiond: ara::core::Initialize() failed"); return 1; }

    // Init phase: the engine and the inference pipeline are already constructed
    // above, so reaching this line IS the check -- a missing or malformed .onnx
    // throws there, before any service is offered. The session does not survive
    // into the run phase; this is fail-fast, not warm-up.
    if (phase == vpap::Phase::Init)
    {
        runtime.report_running();
        VP_INFO("perceptiond: init ok (engine and pipeline constructed)");
        return 0;
    }

    // ── Inference ────────────────────────────────────────────────────────────
    ve::OnnxEngine engine(cfg.engine);
    vm::InferencePipeline pipeline(engine, cfg.inference);

    // H projects AutoSteer / AutoSpeed outputs back to world. It belongs to
    // perception, not sensing: sensing owns the C matrix that warps to BEV.
    cv::Mat H = load_matrix("H.yaml", "H");
    bool h_resized_set = false;

    visualization::Visualization viz({cfg.webrtc_on, cfg.webrtc_port, /*show_window=*/false});
    if (cfg.visualization_on) visualization::init_production_assets();

    // ── Provided port ────────────────────────────────────────────────────────
    auto ds_spec = ara::core::InstanceSpecifier::Create(kDrivingStatePort);
    if (!ds_spec) { VP_ERROR("perceptiond: bad InstanceSpecifier '%s'", kDrivingStatePort); return 1; }

    av::vp::cm::skeleton::DrivingStateServiceSkeleton state(std::move(ds_spec).Value());
    if (!state.OfferService()) { VP_ERROR("perceptiond: OfferService failed"); return 1; }

    // ── Required: frames (blocking — nothing to do without them) ─────────────
    using FrameProxy = av::vp::cm::proxy::CameraFrameServiceProxy;
    ara::core::Promise<FrameProxy> promise;
    auto future = promise.get_future();
    FrameProxy::StartFindService(
        [&promise](auto handles, auto handler) {
            if (handles.empty()) return;
            static bool satisfied{false};
            if (satisfied) return;
            satisfied = true;
            VP_INFO("perceptiond: found CameraFrameService");
            promise.set_value(std::move(FrameProxy::Create(handles[0])).Value());
            FrameProxy::StopFindService(handler);
        },
        ara::com::InstanceIdentifier::MakeAny());
    auto frames = future.get();

    // maxSampleCount 1: only the newest frame matters. A deeper queue would
    // have perception working through stale captures while the vehicle moves.
    if (!frames.FrameEvent.Subscribe(1U)) { VP_ERROR("perceptiond: Subscribe failed"); return 1; }

    // ── Required: trajectory (non-blocking — debug overlay only) ─────────────
    using TrajProxy = av::vp::cm::proxy::TrajectoryServiceProxy;
    std::unique_ptr<TrajProxy> traj;
    TrajProxy::StartFindService(
        [&traj](auto handles, auto handler) {
            if (handles.empty() || traj) return;
            traj = std::make_unique<TrajProxy>(std::move(TrajProxy::Create(handles[0])).Value());
            traj->TrajectoryEvent.Subscribe(1U);
            TrajProxy::StopFindService(handler);
        },
        ara::com::InstanceIdentifier::MakeAny());

    ap::FrameRingReader ring;
    cv::Mat warped, resized;
    Plan last_plan{};                    // for the overlay only
    std::uint32_t out_seq  = 0U;
    std::uint32_t lapped   = 0U;         // frames discarded because the writer lapped us

    runtime.report_running();
    VP_INFO("perceptiond: running");

    while (ap::ApRuntime::running())
    {
        runtime.checkpoint(ap::Checkpoint::kCycleStart);

        // Debug overlay input. Never gates anything below.
        if (traj)
        {
            traj->TrajectoryEvent.GetNewSamples(
                [&](auto t) {
                    last_plan.acceleration = t->accelerationMps2;
                    last_plan.steering.assign(t->steeringHorizonRad.begin(),
                                              t->steeringHorizonRad.end());
                    last_plan.warnings.assign(1, static_cast<Warning>(t->warning));
                },
                1U);
        }

        frames.FrameEvent.GetNewSamples(
            [&](auto d) {
                if (!d->valid) return;
                runtime.checkpoint(ap::Checkpoint::kFrameReceived);

                // Geometry contract with sensingd, checked rather than assumed.
                if (d->width != vm::AutoDrive::NET_W || d->height != vm::AutoDrive::NET_H)
                {
                    VP_ERROR("perceptiond: frame %ux%u does not match network input %dx%d",
                             d->width, d->height, vm::AutoDrive::NET_W, vm::AutoDrive::NET_H);
                    return;
                }

                if (!ring.ensure_mapped(d->segmentInstanceId)) return;

                if (!ring.read(d->warpedSlot, d->generation, warped, resized))
                {
                    // The writer lapped us mid-read. Discarding is correct: the
                    // buffer holds a mix of two frames. If this counter climbs,
                    // sensing is outrunning inference — cap its publish rate.
                    if (++lapped % 30U == 1U)
                        VP_WARN("perceptiond: dropped %u frames (writer lapped reader)", lapped);
                    return;
                }

                if (!h_resized_set)
                {
                    pipeline.set_H_resized(H, cv::Size(d->width, d->height));
                    h_resized_set = true;
                }

                const auto r = pipeline.process(warped, resized);
                if (!r) return;    // empty on the first frame: AutoDrive needs t-1 and t
                runtime.checkpoint(ap::Checkpoint::kInferenceDone);

                av::vp::cm::DrivingState out{};
                out.timestampNs      = d->timestampNs;   // from CAPTURE, so age is measured end to end
                out.seqCounter       = ++out_seq;
                out.cteM             = r->lateral.cte_m;
                out.epsiRad          = r->lateral.yaw_rad;
                out.kappa            = r->lateral.curvature;
                out.cteRateMps       = r->lateral.cte_rate_mps;
                out.yawRateRps       = r->lateral.yaw_rate_rps;
                out.rawCteM          = r->lateral.path_valid ? r->lateral.raw_cte_m
                                                             : r->lateral.cte_m;
                out.pathValid        = r->lateral.path_valid ? 1U : 0U;
                out.hasCipo          = (r->cipo.valid && r->cipo.distance_m < kCipoMaxDistanceM) ? 1U : 0U;
                out.cipoDistanceM    = r->cipo.distance_m;
                out.cipoVelocityMps  = r->cipo.velocity_ms;
                out.valid            = r->lateral.valid ? 1U : 0U;

                if (!state.DrivingStateEvent.Send(out))
                    VP_WARN("perceptiond: Send failed (seq=%u)", out.seqCounter);

                runtime.checkpoint(ap::Checkpoint::kPublished);

                // Same telemetry line as upstream, minus the plan half — that
                // now belongs to planningd, which prints its own.
                VP_INFO("state: cte=%.2fm(raw=%.2fm) cte_dot=%+.2fm/s epsi=%.3f epsi_dot=%+.3frad/s "
                        "kappa=%.4f | cipo=%s dist=%.1f m vel=%+.2f m/s | seq=%u",
                        out.cteM, out.rawCteM, out.cteRateMps, out.epsiRad, out.yawRateRps,
                        out.kappa, out.hasCipo ? "true" : "false",
                        out.cipoDistanceM, out.cipoVelocityMps, out.seqCounter);

                if (cfg.visualization_on)
                {
                    // ego_v is 0 here: perception has no VehicleStatus port, and
                    // adding one for a HUD digit would be the wrong reason to
                    // widen an interface. If the readout matters, the honest fix
                    // is for Trajectory to carry the ego speed its plan was
                    // computed against — useful for correlation anyway.
                    cv::Mat display = viz.build_frame(resized, *r, last_plan, 0.0,
                                                      pipeline.H_resized(), cfg.speed_limit);
                    viz.render_frame(display);
                }
            },
            1U);

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    VP_INFO("perceptiond: SIGTERM from EM - shutting down (%u frames dropped to lapping)", lapped);
    viz.stop();
    return 0;
}
