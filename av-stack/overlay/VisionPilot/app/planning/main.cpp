// ---------------------------------------------------------------------------
// planningd — the Planning Adaptive Application.
//
// Planner::compute_plan and nothing else. Adapted from upstream
// app/vision_pilot.cpp, where the same seven arguments were assembled from
// local variables:
//
//     const double ego_v  = vehicle_interface->read();
//     const double cte    = r->lateral.cte_m;
//     const double epsi   = r->lateral.yaw_rad;
//     const double kappa  = r->lateral.curvature;
//     const bool   has_cipo = r->cipo.valid && r->cipo.distance_m < D_MAX;
//     const double cipo_v   = has_cipo ? r->cipo.velocity_ms : cfg.speed_limit;
//     const Plan plan = planner.compute_plan(cte, epsi, kappa, ego_v,
//                                            has_cipo, cipo_v, cipo_dist);
//
// Six of those now arrive on DrivingStateEvent and the seventh on SpeedEvent.
// The D_MAX gate moved upstream into perceptiond (it is a perception
// judgement about its own tracker); the speed_limit substitution stayed here,
// because cfg.speed_limit is a planning parameter.
//
// This process owns no device and speaks to no chassis. It is a separate AA
// because it has a distinct failure mode: stopping it while Control keeps
// running is a state the vehicle can be in and survive.
// ---------------------------------------------------------------------------
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <common/types.hpp>
#include <config/vision_pilot_config.hpp>
#include <logging/logger.hpp>
#include <planning/planning.hpp>

#include <ap_runtime/ap_runtime.hpp>
#include "../common/phase.hpp"

#include <ara/core/instance_specifier.h>
#include <ara/core/promise.h>
#include <ara/com/types.h>

#include <av/vp/cm/drivingstateservice_proxy.h>
#include <av/vp/cm/vehiclestatusservice_proxy.h>
#include <av/vp/cm/trajectoryservice_skeleton.h>

namespace ap = visionpilot::ap;

namespace {

// InstanceSpecifiers mirror the model as <executable>/<root>/<port>. They are
// constants rather than config keys for now: modules/config has no ap_* fields
// yet (the overlay patch that adds them has not been applied), and hard-coding
// four strings is less machinery than extending the parser before anything
// compiles. Move them to vision_pilot_ap.conf once the process runs.
//
// Renaming any of these in the ARXML breaks the binding at RUN time, not build
// time — the symptom is discovery that never completes.
constexpr char kTrajectoryPort[] = "planningd/pl_root/PPort_Trajectory";

// Plan::warnings -> Trajectory.warning.
//
// The wire codes are DEFINED to equal the Warning enum values (None 0, FCW 1,
// AEB 2, LLDW 3, RLDW 4), so the mapping is an identity cast. What is not
// identity is the reduction: Plan carries a vector, the wire carries one code,
// so the most severe wins. Severity is NOT numeric order — a brake demand must
// never be masked by the warning that preceded it.
std::uint8_t worst_warning(const std::vector<Warning>& warnings)
{
    //                                 None FCW AEB LLDW RLDW
    static constexpr int kSeverity[] = {0,   2,  3,  1,   1};

    std::uint8_t worst = static_cast<std::uint8_t>(Warning::None);
    for (const auto w : warnings)
    {
        const auto code = static_cast<std::uint8_t>(w);
        if (code < (sizeof(kSeverity) / sizeof(kSeverity[0])) &&
            kSeverity[code] > kSeverity[worst])
        {
            worst = code;
        }
    }
    return worst;
}

// Monotonic milliseconds. NOT the wall clock: WSL2 and any NTP-stepped RTC can
// jump, which would make the staleness gate below fire at random or never.
// timestampNs in the events is for correlation and logging; the age that gates
// planning is measured here.
std::uint64_t now_ms() noexcept
{
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// Blocks until the service is offered, then returns the proxy. Same idiom as
// CAPI's own samples/helloworld-cm client: a Promise satisfied from the
// StartFindService callback, then StopFindService so the callback is not
// re-entered.
//
// MakeAny() is correct here even with several services on the machine: the
// proxy TYPE already pins the service interface, and the model declares exactly
// one instance of each.
template <typename Proxy>
Proxy await_service(const char* label)
{
    ara::core::Promise<Proxy> promise;
    auto future = promise.get_future();

    Proxy::StartFindService(
        [&promise, label](auto handles, auto handler) {
            if (handles.empty()) return;
            static bool satisfied{false};
            if (satisfied) return;
            satisfied = true;
            VP_INFO("planningd: found %s", label);
            promise.set_value(std::move(Proxy::Create(handles[0])).Value());
            Proxy::StopFindService(handler);
        },
        ara::com::InstanceIdentifier::MakeAny());

    return future.get();
}

}  // namespace

int main(int argc, char** argv)
{
    const vpap::Phase phase = vpap::parse_phase(argc, argv);
    VP_INFO("planningd: starting, phase=%s", vpap::to_string(phase));

    Config cfg;
    try { cfg = load_vision_pilot_config(); }
    catch (const std::exception& e) { VP_ERROR("Config: %s", e.what()); return 1; }

    // ara::core::Initialize() must precede every other ara:: call. ApRuntime
    // also installs the SIGTERM handler EM uses to stop this process when
    // AvPilotFG leaves Driving.
    ap::ApRuntime runtime("");   // empty == run without PHM until the supervision model exists
    if (!runtime.ok())
    {
        VP_ERROR("planningd: ara::core::Initialize() failed");
        return 1;
    }

    // Init phase: the two numbers the Planner is constructed from. Both are
    // car-sized defaults that are wrong on the rover, so a zero or a negative
    // here is a config error worth catching before Driving.
    if (phase == vpap::Phase::Init)
    {
        runtime.report_running();
        const bool ok = vpap::require_positive(cfg.L,           "wheelbase L", "planningd")
                      & vpap::require_positive(cfg.speed_limit, "speed_limit", "planningd");
        VP_INFO("planningd: init %s", ok ? "ok" : "FAILED");
        return ok ? 0 : 1;
    }

    Planner planner(cfg.speed_limit, cfg.L);

    // ── Provided port ────────────────────────────────────────────────────────
    auto traj_spec = ara::core::InstanceSpecifier::Create(kTrajectoryPort);
    if (!traj_spec)
    {
        VP_ERROR("planningd: bad InstanceSpecifier '%s'", kTrajectoryPort);
        return 1;
    }
    av::vp::cm::skeleton::TrajectoryServiceSkeleton trajectory(std::move(traj_spec).Value());

    if (!trajectory.OfferService())
    {
        VP_ERROR("planningd: OfferService failed");
        return 1;
    }

    // ── Required ports ───────────────────────────────────────────────────────
    // Blocking waits, before report_running(): EM treats kRunning as "safe to
    // depend on", so a process that reports ready before its inputs exist is
    // lying to the platform.
    auto state_proxy = await_service<av::vp::cm::proxy::DrivingStateServiceProxy>("DrivingStateService");
    auto speed_proxy = await_service<av::vp::cm::proxy::VehicleStatusServiceProxy>("VehicleStatusService");

    // maxSampleCount 1: only the newest sample matters. A deeper queue would
    // let this process work through stale perception while the vehicle moves.
    if (!state_proxy.DrivingStateEvent.Subscribe(1) ||
        !speed_proxy.SpeedEvent.Subscribe(1))
    {
        VP_ERROR("planningd: Subscribe failed");
        return 1;
    }

    runtime.report_running();
    VP_INFO("planningd: running");

    // ── State carried between cycles ─────────────────────────────────────────
    double        ego_v      = 0.0;
    bool          ego_valid  = false;
    std::uint64_t ego_rx_ms  = 0;
    std::uint32_t out_seq    = 0;

    const std::uint64_t speed_stale_ms = 200;   // placeholder; tune on target, never on a dev host

    while (ap::ApRuntime::running())
    {
        runtime.checkpoint(ap::Checkpoint::kCycleStart);

        // 1. Refresh ego speed. Kept separate from the driving-state handler
        //    because the two streams are independent and either can stall.
        speed_proxy.SpeedEvent.GetNewSamples(
            [&](auto sample) {
                if (sample->valid)
                {
                    ego_v     = static_cast<double>(sample->speedMps);
                    ego_valid = true;
                    ego_rx_ms = now_ms();
                }
                else
                {
                    // valid == 0 means the producer does not stand behind the
                    // value. Do NOT silently treat it as 0 m/s.
                    ego_valid = false;
                }
            },
            1U);

        // 2. Plan on each new perception sample.
        state_proxy.DrivingStateEvent.GetNewSamples(
            [&](auto st) {
                runtime.checkpoint(ap::Checkpoint::kFrameReceived);

                if (!st->valid) return;

                const std::uint64_t ego_age = now_ms() - ego_rx_ms;
                if (!ego_valid || ego_age > speed_stale_ms)
                {
                    // A plan computed against an assumed speed is worse than no
                    // plan: Control's own staleness gate will safe-stop, which
                    // is the behaviour we want.
                    VP_WARN("planningd: ego speed stale (%lu ms) - not planning",
                            static_cast<unsigned long>(ego_age));
                    return;
                }

                const double cipo_v = st->hasCipo ? static_cast<double>(st->cipoVelocityMps)
                                                  : cfg.speed_limit;

                const Plan plan = planner.compute_plan(
                    static_cast<double>(st->cteM),
                    static_cast<double>(st->epsiRad),
                    static_cast<double>(st->kappa),
                    ego_v,
                    st->hasCipo != 0U,
                    cipo_v,
                    static_cast<double>(st->cipoDistanceM));

                av::vp::cm::Trajectory out{};
                out.timestampNs      = st->timestampNs;   // propagate from capture, not from now
                out.seqCounter       = ++out_seq;
                out.accelerationMps2 = static_cast<float>(plan.acceleration);
                out.warning          = worst_warning(plan.warnings);

                // Copy what the planner produced; pad the remainder with the
                // last real value so Control decays from a sane angle rather
                // than snapping to zero if the planner returned a short vector.
                const std::size_t n = std::min(plan.steering.size(),
                                               out.steeringHorizonRad.size());
                for (std::size_t i = 0; i < n; ++i)
                    out.steeringHorizonRad[i] = static_cast<float>(plan.steering[i]);
                const float tail = (n > 0) ? out.steeringHorizonRad[n - 1] : 0.0F;
                for (std::size_t i = n; i < out.steeringHorizonRad.size(); ++i)
                    out.steeringHorizonRad[i] = tail;

                if (!trajectory.TrajectoryEvent.Send(out))
                    VP_WARN("planningd: Send failed");

                runtime.checkpoint(ap::Checkpoint::kPublished);

                VP_INFO("plan: tyre=%.4f rad  accel=%.3f m/s2  |  cte=%.2fm  epsi=%.3f  kappa=%.4f"
                        "  |  cipo=%s  dist=%.1f m  vel=%+.2f m/s  |  seq=%u",
                        out.steeringHorizonRad[1], plan.acceleration,
                        st->cteM, st->epsiRad, st->kappa,
                        st->hasCipo ? "true" : "false",
                        st->cipoDistanceM, st->cipoVelocityMps, out.seqCounter);
            },
            1U);

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    VP_INFO("planningd: SIGTERM from EM - shutting down");
    return 0;
}
