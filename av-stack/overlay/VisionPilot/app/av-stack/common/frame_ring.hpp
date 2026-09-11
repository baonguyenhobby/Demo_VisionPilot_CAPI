// ---------------------------------------------------------------------------
// The shared-memory frame ring: Sensing writes, Perception reads.
//
// This is the one part of the design the ARXML cannot describe, and therefore
// the one part no model check will ever catch. It exists because Sensing is a
// separate Adaptive Application — the sensor abstraction layer, where radar and
// lidar will arrive as additional ports — while a frame is 3 MB and 30 fps of
// that is 90 MB/s. Sending it through ara::com would need SOME/IP-TP
// segmentation, roughly 2,250 datagrams per frame, and two copies.
//
// So the pixels do not travel. Sensing writes each capture into a slot here and
// publishes only FrameDescriptor (34 B) over CameraFrameService. Perception maps
// the same segment read-only and reads the slot the descriptor names.
//
// WHAT THIS COSTS, since the model cannot say it:
//   - E2E protection covers the descriptor, never the pixels. The integrity
//     check for the payload is this ring's own generation counter.
//   - IAM does not gate access to the segment; filesystem permissions on the
//     /dev/shm object do. The user and OS resource group EM starts each process
//     under are therefore load-bearing — see integration/deployment.arxml.
//
// PROTOCOL (seqlock, writer never blocks)
//
//   Writer, per frame:
//     1. slot = seq % kSlots
//     2. store slot.generation = kWriting          (odd, "do not read me")
//     3. memcpy warped and resized into the slot
//     4. store slot.generation = g                 (even, monotonically rising)
//     5. publish FrameDescriptor{..., generation = g, warpedSlot = slot, ...}
//
//   Reader, per descriptor:
//     1. if descriptor.segmentInstanceId != mapped instance -> re-map
//     2. read slot.generation; if != descriptor.generation, discard
//     3. consume the pixels
//     4. re-read slot.generation; if it changed, the writer lapped us mid-read
//        -> DISCARD the frame. Do not use it.
//
// kSlots = 3 is the minimum that keeps the writer off the slot a reader holds
// while it works. With 2 the reader is overwritten as soon as it falls one frame
// behind, which at 55-65 ms of inference against a faster source is immediately.
//
// The writer never waits for the reader. A perception pipeline that cannot keep
// up should drop frames, not apply backpressure to the camera.
// ---------------------------------------------------------------------------
#ifndef VISIONPILOT_AP_FRAME_RING_HPP
#define VISIONPILOT_AP_FRAME_RING_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include <opencv2/core.hpp>

namespace visionpilot::ap {

// Name of the POSIX shared-memory object. Leading slash is required by
// shm_open; it appears as /dev/shm/visionpilot_frames.
inline constexpr char kRingName[] = "/visionpilot_frames";

inline constexpr std::uint32_t kSlots       = 3U;
inline constexpr std::uint32_t kWritingMark = 1U;   // odd generation = mid-write

// Per-slot header, immediately followed by the warped image then the resized
// image. Both are CV_8UC3 of the same dimensions (1024x512 for AutoDrive).
struct SlotHeader
{
    std::atomic<std::uint32_t> generation;   // odd = being written, even = stable
    std::uint32_t              seq;
    std::uint64_t              timestampNs;
};

// Segment header. instanceId changes every time Sensing creates the segment —
// without it a restarted sensingd hands Perception a recreated segment that
// looks exactly like the old one, which is the failure mode that costs days.
struct RingHeader
{
    std::atomic<std::uint32_t> instanceId;
    std::uint32_t              slots;
    std::uint32_t              width;
    std::uint32_t              height;
    std::uint32_t              imageBytes;    // ONE image; a slot holds two
};

inline std::size_t slot_stride(std::size_t image_bytes)
{
    return sizeof(SlotHeader) + 2U * image_bytes;
}

inline std::size_t segment_bytes(std::size_t image_bytes)
{
    return sizeof(RingHeader) + kSlots * slot_stride(image_bytes);
}

// ---------------------------------------------------------------------------
// Writer — owned by sensingd.
// ---------------------------------------------------------------------------
class FrameRingWriter
{
public:
    FrameRingWriter() = default;
    ~FrameRingWriter();

    FrameRingWriter(const FrameRingWriter&)            = delete;
    FrameRingWriter& operator=(const FrameRingWriter&) = delete;

    // Creates (or recreates) the segment sized for width x height CV_8UC3.
    // Always allocates a NEW instanceId, so a consumer holding the old one
    // re-maps rather than reading a stale layout.
    bool create(int width, int height);

    // Copies both images into the next slot and fills in the fields of the
    // descriptor the caller then publishes. Returns the slot index used.
    // Never blocks.
    std::uint32_t write(const cv::Mat& warped, const cv::Mat& resized,
                        std::uint32_t seq, std::uint64_t timestamp_ns,
                        std::uint32_t& out_generation);

    std::uint32_t instance_id() const noexcept { return instance_id_; }
    bool ok() const noexcept { return base_ != nullptr; }

private:
    void*         base_        = nullptr;
    std::size_t   bytes_       = 0;
    std::size_t   image_bytes_ = 0;
    std::uint32_t instance_id_ = 0U;
    std::uint32_t generation_  = 0U;
    int           fd_          = -1;
};

// ---------------------------------------------------------------------------
// Reader — owned by perceptiond. Maps read-only.
// ---------------------------------------------------------------------------
class FrameRingReader
{
public:
    FrameRingReader() = default;
    ~FrameRingReader();

    FrameRingReader(const FrameRingReader&)            = delete;
    FrameRingReader& operator=(const FrameRingReader&) = delete;

    // Maps the segment if not already mapped for this instance_id. Cheap to
    // call every frame; only does work when the id changes.
    bool ensure_mapped(std::uint32_t instance_id);

    // Copies the two images out of the slot, re-checking the generation
    // afterwards. Returns false if the writer lapped us mid-read — in that case
    // the frame MUST be discarded, not used.
    bool read(std::uint32_t slot, std::uint32_t expected_generation,
              cv::Mat& warped, cv::Mat& resized);

    std::uint32_t mapped_instance() const noexcept { return instance_id_; }

private:
    void          unmap();

    const void*   base_        = nullptr;
    std::size_t   bytes_       = 0;
    std::size_t   image_bytes_ = 0;
    int           width_       = 0;
    int           height_      = 0;
    std::uint32_t instance_id_ = 0U;
    int           fd_          = -1;
};

}  // namespace visionpilot::ap

#endif  // VISIONPILOT_AP_FRAME_RING_HPP
