// ---------------------------------------------------------------------------
// VisionPilot on AUTOSAR Adaptive (CAPI) — AP process runtime
// ---------------------------------------------------------------------------
#include <ap_runtime/ap_runtime.hpp>

#include <ara/core/initialization.h>
#include <ara/core/instance_specifier.h>
#include <ara/core/string_view.h>
#include <ara/exec/execution_client.h>
#include <ara/log/logger.h>
#include <ara/phm/supervised_entity.h>

#include <atomic>
#include <csignal>
#include <cstring>

namespace visionpilot::ap {
namespace {

// EM stops a process by sending SIGTERM and waiting for it to exit within the
// configured timeout. Handling it (rather than dying on the default action) is
// what lets the process flush its .rrd, stop offering its service and exit 0.
std::atomic_bool g_continue_execution{true};

void SigTermHandler(int signal)
{
    if (signal == SIGTERM) {
        g_continue_execution = false;
    }
}

bool RegisterSigTermHandler()
{
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SigTermHandler;
    sa.sa_flags   = 0;
    sigemptyset(&sa.sa_mask);
    return sigaction(SIGTERM, &sa, nullptr) != -1;
}

}  // namespace

struct ApRuntime::Impl
{
    bool initialized{false};
    ara::log::Logger& logger;
    std::unique_ptr< ara::phm::SupervisedEntity< Checkpoint > > se;

    Impl() : logger(ara::log::CreateLogger("#VPA", "visionpilot-ap", ara::log::LogLevel::kInfo)) {}
};

ApRuntime::ApRuntime(std::string se_instance_specifier)
{
    // ara::core::Initialize() must be the first ara:: call in the process.
    if (!ara::core::Initialize()) {
        return;
    }

    impl_              = std::make_unique< Impl >();
    impl_->initialized = true;

    if (!RegisterSigTermHandler()) {
        impl_->logger.LogError() << "ApRuntime: cannot register SIGTERM handler — EM will have to "
                                    "kill this process instead of stopping it cleanly";
    }

    if (!se_instance_specifier.empty()) {
        auto spec_res = ara::core::InstanceSpecifier::Create(ara::core::StringView{se_instance_specifier.c_str()});
        if (spec_res.HasValue()) {
            impl_->se = std::make_unique< ara::phm::SupervisedEntity< Checkpoint > >(spec_res.Value());
            impl_->logger.LogInfo() << "ApRuntime: supervised entity " << se_instance_specifier.c_str();
        } else {
            impl_->logger.LogError() << "ApRuntime: malformed SupervisedEntity InstanceSpecifier "
                                     << se_instance_specifier.c_str() << " — running unsupervised";
        }
    } else {
        impl_->logger.LogWarn() << "ApRuntime: no supervised entity configured — checkpoints are dropped";
    }
}

ApRuntime::~ApRuntime()
{
    if (impl_ && impl_->initialized) {
        impl_->se.reset();
        static_cast< void >(ara::core::Deinitialize());
    }
}

bool ApRuntime::ok() const noexcept
{
    return impl_ != nullptr && impl_->initialized;
}

void ApRuntime::report_running() noexcept
{
    if (!ok()) {
        return;
    }
    ara::exec::ExecutionClient{}.ReportExecutionState(ara::exec::ExecutionState::kRunning);
    impl_->logger.LogInfo() << "ApRuntime: ReportExecutionState(kRunning)";
}

bool ApRuntime::running() noexcept
{
    return g_continue_execution.load();
}

void ApRuntime::checkpoint(Checkpoint cp) noexcept
{
    if (!ok() || !impl_->se) {
        return;
    }
    // Deliberately not logged: this is on the per-frame hot path.
    static_cast< void >(impl_->se->ReportCheckpoint(cp));
}

bool ApRuntime::supervision_healthy() noexcept
{
    if (!ok() || !impl_->se) {
        return true;  // no supervision configured — do not report a false alarm
    }
    const auto status = impl_->se->GetGlobalSupervisionStatus();
    if (!status.HasValue()) {
        return false;
    }
    return status.Value() == ara::phm::GlobalSupervisionStatus::kOK;
}

}  // namespace visionpilot::ap
