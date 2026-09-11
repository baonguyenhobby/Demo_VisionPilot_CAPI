// ---------------------------------------------------------------------------
// VisionPilot on AUTOSAR Adaptive (CAPI) — AP process runtime
//
// Owns everything about being an Adaptive Application that is NOT
// communication: ara::core initialisation, the SIGTERM contract with
// Execution Management, execution-state reporting, and the PHM
// SupervisedEntity used to report per-cycle checkpoints.
//
// Nothing in VisionPilot's inference / fusion / planning layers knows this
// class exists. It is instantiated in main() only when the binary is built
// with -DENABLE_AP_INTERFACE=ON, exactly as rclcpp::init() is today.
// ---------------------------------------------------------------------------
#ifndef VISIONPILOT_AP_RUNTIME_HPP
#define VISIONPILOT_AP_RUNTIME_HPP

#include <cstdint>
#include <memory>
#include <string>

namespace visionpilot::ap {

// Checkpoints reported to ara::phm.
//
// The numeric values MUST match the CHECKPOINT-ID values of the
// PHM-SUPERVISED-ENTITY-INTERFACE in the model — ara::phm transports the
// enum as a plain uint32_t (see ara/phm/supervised_entity.h), so a
// mismatch is silent, not a compile error.
//
//   kCycleStart    -> alive supervision  (one indication per reference cycle)
//   kFrameReceived -> start of the deadline-supervised transition
//   kInferenceDone -> observability only; not currently supervised
//   kPublished     -> end of the deadline-supervised transition
//
// The supervised transition kFrameReceived -> kPublished is what catches the
// alive-but-throttled case that alive supervision alone cannot see.
enum class Checkpoint : std::uint32_t
{
    kCycleStart    = 0U,
    kFrameReceived = 1U,
    kInferenceDone = 2U,
    kPublished     = 3U,
};

class ApRuntime
{
public:
    // se_instance_specifier: InstanceSpecifier of the SupervisedEntity port,
    //   e.g. "VisionPilotApp/VisionPilotRoot/SE_Pipeline".
    // Pass an empty string to run without PHM (checkpoint() then does nothing) —
    // useful when bringing the application up before the supervision model exists.
    explicit ApRuntime(std::string se_instance_specifier);
    ~ApRuntime();

    ApRuntime(const ApRuntime&)            = delete;
    ApRuntime& operator=(const ApRuntime&) = delete;

    // False when ara::core::Initialize() failed. main() must abort in that case:
    // no ara:: API may be called before a successful Initialize().
    bool ok() const noexcept;

    // ExecutionState::kRunning. Call once, AFTER every skeleton has offered its
    // service and every proxy has been found — EM treats kRunning as
    // "this process is ready to be depended upon".
    void report_running() noexcept;

    // The main-loop exit condition. Turns false when EM sends SIGTERM, which is
    // what happens when the function group leaves a state this process is
    // listed in — e.g. AvPilotFG.Driving -> AvPilotFG.Degraded.
    static bool running() noexcept;

    void checkpoint(Checkpoint cp) noexcept;

    // GlobalSupervisionStatus == kOK. Cheap enough to poll once per cycle;
    // exposed so the application can log its own supervision view rather than
    // relying on the PHM log alone.
    bool supervision_healthy() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace visionpilot::ap

#endif  // VISIONPILOT_AP_RUNTIME_HPP
