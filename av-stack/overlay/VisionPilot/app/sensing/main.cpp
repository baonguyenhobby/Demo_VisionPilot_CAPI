// ---------------------------------------------------------------------------
// sensingd — the Sensing Adaptive Application.
//
// The sensor abstraction layer. Today it offers one camera; radar and lidar
// arrive as additional provided ports on this component, with no change to
// Perception's process boundary and none at all to Planning or Control. That
// substitutability is why Sensing is its own AA rather than the front of the
// perception loop.
//
// Scope follows the reference decomposition: driver plus sensor-specific
// preprocessing. Adapted from the head of upstream app/vision_pilot.cpp:
//
//     auto [ok, frame] = camera_interface->get_latest_frame();
//     preprocessor.preprocess(frame, warped, resized, net_size);
//
// Moving preprocess() here is what turns the measured `wall = pre + parallel`
// into a two-stage pipeline: steady-state throughput becomes max(pre, parallel)
// rather than their sum, which on the recorded numbers is 67-84 ms down to
// 55-65 ms. Per-frame latency is unchanged; throughput is what improves.
//
// What it owns beyond ara::com: the camera device (or the clip) and the
// shared-memory frame ring. Neither appears in the model — see
// app/av-stack/common/frame_ring.hpp for what that costs.
// ---------------------------------------------------------------------------
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>

#include <opencv2/core.hpp>

#include <config/vision_pilot_config.hpp>
#include <image_preprocessing/image_preprocessor.hpp>
#include <logging/logger.hpp>

#include "camera_interface/file_interface.hpp"
#include "camera_interface/v4l2_camera_interface.hpp"

#include <ap_runtime/ap_runtime.hpp>
#include "../common/phase.hpp"
#include "../common/frame_ring.hpp"

#include <ara/core/instance_specifier.h>
#include <av/vp/cm/cameraframeservice_skeleton.h>

namespace ap = visionpilot::ap;

namespace {

constexpr char kCameraFramePort[] = "sensingd/sn_root/PPort_CameraFrame";

// Network input geometry. Perception cross-checks these against
// AutoDrive::NET_W / NET_H on the first descriptor and refuses to run on a
// mismatch, so this is a checked assumption rather than a silent one.
constexpr int kNetW = 1024;
constexpr int kNetH = 512;

// Publish rate cap. Perception needs 55-65 ms per frame; with
// video_realtime = false the file source runs as fast as it can read, which
// laps a 3-slot ring continuously and makes an A/B comparison against the
// monolith meaningless because the two runs never contain the same frames.
//
// Set this BELOW the rate perception sustains for the equivalence run, and to 0
// to run flat out and exercise the drop path deliberately.
constexpr int kMaxFps = 10;

std::uint64_t now_ns() noexcept
{
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

}  // namespace

int main(int argc, char** argv)
{
    const vpap::Phase phase = vpap::parse_phase(argc, argv);
    VP_INFO("sensingd: starting, phase=%s", vpap::to_string(phase));

    Config cfg;
    try { cfg = load_vision_pilot_config(); }
    catch (const std::exception& e) { VP_ERROR("Config: %s", e.what()); return 1; }

    ap::ApRuntime runtime("");
    if (!runtime.ok()) { VP_ERROR("sensingd: ara::core::Initialize() failed"); return 1; }

    // Init phase: prove the source exists before anything downstream waits on
    // frames that will never arrive. Nothing is opened for keeps -- this process
    // exits, and the run phase opens the device itself.
    if (phase == vpap::Phase::Init)
    {
        runtime.report_running();
        bool ok = (cfg.source.mode == SourceMode::Video)
                ? vpap::require_file(cfg.source.input_video,  "input video",   "sensingd")
                : vpap::require_file(cfg.source.v4l2_device,  "camera device", "sensingd");
        VP_INFO("sensingd: init %s", ok ? "ok" : "FAILED");
        return ok ? 0 : 1;
    }

    // ── Frame source ─────────────────────────────────────────────────────────
    // Chosen by source.mode exactly as upstream: the same binary runs open-loop
    // against a recorded clip on a dev host and against the camera on target.
    // Only this process ever knows which.
    std::shared_ptr<CameraInterface> camera;
    if (cfg.source.mode == SourceMode::Video)
        camera = std::make_shared<camera_interface::FileInterface>(
            cfg.source.input_video, cfg.source.video_loop, cfg.source.video_realtime);
    else
        camera = std::make_shared<camera_interface::V4L2CameraInterface>(
            cfg.source.v4l2_device, static_cast<std::uint32_t>(cfg.source.v4l2_fps));

    if (!camera || !camera->is_device_open())
    {
        VP_ERROR("sensingd: cannot open frame source");
        return 1;
    }

    ImagePreprocessor preprocessor;

    // ── Ring ─────────────────────────────────────────────────────────────────
    // Created BEFORE OfferService: a consumer that finds the service must be
    // able to map the segment immediately, and the descriptor it receives names
    // a slot that has to exist.
    ap::FrameRingWriter ring;
    if (!ring.create(kNetW, kNetH))
    {
        VP_ERROR("sensingd: cannot create frame ring - check df -h /dev/shm");
        return 1;
    }

    // ── Provided port ────────────────────────────────────────────────────────
    auto spec = ara::core::InstanceSpecifier::Create(kCameraFramePort);
    if (!spec) { VP_ERROR("sensingd: bad InstanceSpecifier '%s'", kCameraFramePort); return 1; }

    av::vp::cm::skeleton::CameraFrameServiceSkeleton frames(std::move(spec).Value());
    if (!frames.OfferService()) { VP_ERROR("sensingd: OfferService failed"); return 1; }

    runtime.report_running();
    VP_INFO("sensingd: running  (%s, %dx%d, cap %d fps)",
            cfg.source.mode == SourceMode::Video ? "clip" : "camera", kNetW, kNetH, kMaxFps);

    const cv::Size net_size(kNetW, kNetH);
    cv::Mat frame, warped, resized;
    std::uint32_t seq = 0U;

    const auto min_period = (kMaxFps > 0)
        ? std::chrono::nanoseconds(1000000000LL / kMaxFps)
        : std::chrono::nanoseconds(0);
    auto next_due = std::chrono::steady_clock::now();

    while (ap::ApRuntime::running())
    {
        runtime.checkpoint(ap::Checkpoint::kCycleStart);

        auto [ok, captured] = camera->get_latest_frame();
        if (!ok || captured.empty())
        {
            // End of clip with looping off: upstream broke out of the loop here.
            // A platform process does not exit on its own — EM decides when it
            // stops — so idle instead and let the function group end the run.
            if (cfg.source.mode == SourceMode::Video && !cfg.source.video_loop)
            {
                VP_INFO("sensingd: end of clip - idling until EM stops this process");
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        runtime.checkpoint(ap::Checkpoint::kFrameReceived);
        const std::uint64_t t_capture = now_ns();

        // The whole of upstream's `pre` stage: homography warp to BEV plus the
        // resize to network input. Mounting-specific, therefore Sensing's.
        preprocessor.preprocess(captured, warped, resized, net_size);

        std::uint32_t generation = 0U;
        const std::uint32_t slot = ring.write(warped, resized, seq, t_capture, generation);

        av::vp::cm::FrameDescriptor d{};
        d.timestampNs       = t_capture;
        d.seqCounter        = ++seq;
        d.segmentInstanceId = ring.instance_id();
        d.warpedSlot        = static_cast<std::uint16_t>(slot);
        d.resizedSlot       = static_cast<std::uint16_t>(slot);  // one slot holds both
        d.generation        = generation;
        d.width             = static_cast<std::uint16_t>(kNetW);
        d.height            = static_cast<std::uint16_t>(kNetH);
        d.rawWidth          = static_cast<std::uint16_t>(captured.cols);
        d.rawHeight         = static_cast<std::uint16_t>(captured.rows);
        d.stride            = static_cast<std::uint32_t>(warped.step);
        d.pixelFormat       = 0U;   // BGR8, OpenCV native
        d.valid             = 1U;

        if (!frames.FrameEvent.Send(d))
            VP_WARN("sensingd: FrameEvent Send failed (seq=%u)", d.seqCounter);

        runtime.checkpoint(ap::Checkpoint::kPublished);

        if (min_period.count() > 0)
        {
            next_due += min_period;
            std::this_thread::sleep_until(next_due);
        }
    }

    VP_INFO("sensingd: SIGTERM from EM - shutting down");
    return 0;
}
