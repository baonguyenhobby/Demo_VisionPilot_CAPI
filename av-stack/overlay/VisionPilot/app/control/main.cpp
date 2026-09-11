// ---------------------------------------------------------------------------
// controld — the Control Adaptive Application.
//
// The last thing before the actuator, and the only process that touches the
// chassis. Adapted from one line at the tail of upstream app/vision_pilot.cpp:
//
//     vehicle_interface->write(plan.steering.empty() ? 0.0 : plan.steering[1],
//                              plan.acceleration);
//
// That line becomes this process. What upstream did implicitly — apply index 1,
// trust the value, write immediately — becomes explicit here, because once
// Planning is a separate process that EM can stop, "no trajectory this cycle"
// is a state the vehicle can be in and must survive.
//
// Also senses ego speed and publishes it, because this process owns the chassis
// link. Control being both the vehicle interface and the last safety gate is
// deliberate: the chassis is a single bidirectional link only one process can
// own, and a supervisor that is not the last thing before the actuator is not
// supervising anything.
//
// Deliberately links no engine / models / fusion.
// ---------------------------------------------------------------------------
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <thread>

#include <config/vision_pilot_config.hpp>
#include <logging/logger.hpp>
#include <vehicle_interface/vehicle_interface.hpp>
#include <vehicle_interface/can_interface.hpp>
#include <vehicle_interface/file_interface.hpp>

#include <ap_runtime/ap_runtime.hpp>

#include <ara/core/instance_specifier.h>
#include <ara/core/promise.h>
#include <ara/com/types.h>

#include <av/vp/cm/trajectoryservice_proxy.h>
#include <av/vp/cm/vehiclestatusservice_skeleton.h>

namespace ap = visionpilot::ap;

namespace {

constexpr char kVehicleStatusPort[] = "controld/ct_root/PPort_VehicleStatus";

// How long a trajectory stays actionable before Control starts degrading.
// Placeholder: tune on the Orin under combined GPU and sensor load, never on a
// dev host, and never against the wall clock (see now_ms below).
constexpr std::uint64_t kTrajectoryStaleMs = 200U;

constexpr double kComfortDecelMps2   = 1.5;
constexpr double kEmergencyDecelMps2 = 4.0;
constexpr double kMaxSteerRateRadS   = 0.6;   // limit how fast the tyre angle may move
constexpr double kControlPeriodS     = 0.05;

std::uint64_t now_ms() noexcept
{
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// Bicycle pair -> chassis velocity pair.
//
//   v_target = v + a*dt          Twist carries no acceleration field
//   omega    = v * tan(delta)/L  bicycle model
//
// Two limits this cannot fix, recorded so nobody re-derives them:
//   - The rover is SKID-STEER, so tan(delta) is an approximation whose error
//     grows with steering angle.
//   - ugv_driver.py forces |omega| >= 0.2 whenever linear.x == 0, which fights
//     a lane-keeper emitting small corrections at low speed.
struct ChassisCommand { double linear_mps; double angular_rps; };

ChassisCommand to_chassis(double steering_rad, double accel_mps2, double ego_v_mps,
                          double wheelbase_m, double speed_limit_mps)
{
    double v = std::clamp(ego_v_mps + accel_mps2 * kControlPeriodS, 0.0, speed_limit_mps);
    const double omega = (wheelbase_m > 0.0) ? v * std::tan(steering_rad) / wheelbase_m : 0.0;
    return {v, omega};
}

}  // namespace

int main(int argc, char** argv)
{
    (void)argc; (void)argv;

    Config cfg;
    try { cfg = load_vision_pilot_config(); }
    catch (const std::exception& e) { VP_ERROR("Config: %s", e.what()); return 1; }

    ap::ApRuntime runtime("");
    if (!runtime.ok()) { VP_ERROR("controld: ara::core::Initialize() failed"); return 1; }

    // ── Chassis ──────────────────────────────────────────────────────────────
    // On the bench this replays frame_speed.txt; on the rover it is the real
    // link. Same VehicleInterface either way, which is why the abstraction is
    // kept rather than a driver being inlined here.
    std::shared_ptr<VehicleInterface> chassis;
    if (cfg.source.mode == SourceMode::Video)
        chassis = std::make_shared<FileInterface>(cfg.source.input_vehicle_speed, cfg.source.video_loop);
    else
        chassis = std::make_shared<CanInterface>();

    // ── Provided port ────────────────────────────────────────────────────────
    auto speed_spec = ara::core::InstanceSpecifier::Create(kVehicleStatusPort);
    if (!speed_spec)
    {
        VP_ERROR("controld: bad InstanceSpecifier '%s'", kVehicleStatusPort);
        return 1;
    }
    av::vp::cm::skeleton::VehicleStatusServiceSkeleton status(std::move(speed_spec).Value());
    if (!status.OfferService()) { VP_ERROR("controld: OfferService failed"); return 1; }

    // ── Required port ────────────────────────────────────────────────────────
    // NOTE the asymmetry with planningd: Control does NOT block waiting for a
    // trajectory before reporting kRunning. It must be able to hold the vehicle
    // still with no planner at all — that is the whole point of AvPilotFG.Safe,
    // where controld is the only process running.
    using TrajProxy = av::vp::cm::proxy::TrajectoryServiceProxy;
    std::unique_ptr<TrajProxy> traj;

    TrajProxy::StartFindService(
        [&traj](auto handles, auto handler) {
            if (handles.empty() || traj) return;
            traj = std::make_unique<TrajProxy>(std::move(TrajProxy::Create(handles[0])).Value());
            traj->TrajectoryEvent.Subscribe(1U);
            VP_INFO("controld: found TrajectoryService");
            TrajProxy::StopFindService(handler);
        },
        ara::com::InstanceIdentifier::MakeAny());

    runtime.report_running();
    VP_INFO("controld: running");

    // ── State carried between cycles ─────────────────────────────────────────
    av::vp::cm::Trajectory last{};
    bool          have_traj  = false;
    std::uint64_t traj_rx_ms = 0;
    std::size_t   horizon_ix = 1;          // upstream applies steering[1]
    double        applied_steer = 0.0;
    std::uint32_t speed_seq  = 0;

    while (ap::ApRuntime::running())
    {
        runtime.checkpoint(ap::Checkpoint::kCycleStart);

        // 1. Sense and publish ego speed FIRST, unconditionally. planningd
        //    starves without it, and a Control that stopped publishing because
        //    it had no trajectory would deadlock the pair.
        const double ego_v = chassis->read();

        av::vp::cm::VehicleSpeed speed{};
        speed.timestampNs = static_cast<std::uint64_t>(now_ms()) * 1000000ULL;
        speed.seqCounter  = ++speed_seq;
        speed.speedMps    = static_cast<float>(ego_v);
        speed.valid       = 1U;
        if (!status.SpeedEvent.Send(speed))
            VP_WARN("controld: SpeedEvent Send failed");

        // 2. Take the newest trajectory, if any arrived.
        if (traj)
        {
            traj->TrajectoryEvent.GetNewSamples(
                [&](auto sample) {
                    last       = *sample;
                    have_traj  = true;
                    traj_rx_ms = now_ms();
                    horizon_ix = 1;
                },
                1U);
        }

        // 3. Decide what to actuate.
        const std::uint64_t age = have_traj ? (now_ms() - traj_rx_ms) : kTrajectoryStaleMs + 1;
        double target_steer = 0.0;
        double accel        = 0.0;

        if (!have_traj)
        {
            // Never had a plan — hold still rather than roll on an assumption.
            accel = -kComfortDecelMps2;
        }
        else if (age <= kTrajectoryStaleMs)
        {
            target_steer = last.steeringHorizonRad[1];
            accel        = last.accelerationMps2;
            horizon_ix   = 2;
        }
        else if (horizon_ix < last.steeringHorizonRad.size())
        {
            // Planning has stopped — AvPilotFG.Degraded, or it crashed. Follow
            // the REMAINDER of the horizon it already gave us, decaying toward
            // straight, and slow down. This is the entire reason Trajectory
            // carries 20 steps instead of one value: without it, Degraded would
            // be an instant cutoff rather than a controlled hand-back.
            const double decay = 1.0 - static_cast<double>(horizon_ix) /
                                        static_cast<double>(last.steeringHorizonRad.size());
            target_steer = last.steeringHorizonRad[horizon_ix] * decay;
            accel        = -kComfortDecelMps2;
            ++horizon_ix;
            VP_WARN("controld: trajectory stale (%lu ms) - horizon step %zu of %zu",
                    static_cast<unsigned long>(age), horizon_ix, last.steeringHorizonRad.size());
        }
        else
        {
            accel = -kEmergencyDecelMps2;
            VP_ERROR("controld: horizon exhausted - safe stop");
        }

        // 4. Rate-limit, convert, write. A planner is allowed to ask for things
        //    a chassis should not be asked to do; this is where that is refused.
        const double max_delta = kMaxSteerRateRadS * kControlPeriodS;
        applied_steer += std::clamp(target_steer - applied_steer, -max_delta, max_delta);

        const ChassisCommand cmd =
            to_chassis(applied_steer, accel, ego_v, cfg.L, cfg.speed_limit);

        chassis->write(applied_steer, accel);

        VP_INFO("ctrl: tyre=%.4f rad  accel=%+.3f m/s2  ->  v=%.2f m/s  omega=%+.3f rad/s"
                "  |  ego=%.2f m/s  age=%lu ms",
                applied_steer, accel, cmd.linear_mps, cmd.angular_rps, ego_v,
                static_cast<unsigned long>(age));

        runtime.checkpoint(ap::Checkpoint::kPublished);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(static_cast<int>(kControlPeriodS * 1000)));
    }

    VP_INFO("controld: SIGTERM from EM - shutting down");
    return 0;
}
