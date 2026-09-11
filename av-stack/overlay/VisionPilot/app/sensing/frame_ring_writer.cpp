#include "../common/frame_ring.hpp"

#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <logging/logger.hpp>

namespace visionpilot::ap {

namespace {

RingHeader* header(void* base) { return static_cast<RingHeader*>(base); }

std::uint8_t* slot_at(void* base, std::size_t image_bytes, std::uint32_t slot)
{
    return static_cast<std::uint8_t*>(base) + sizeof(RingHeader) +
           static_cast<std::size_t>(slot) * slot_stride(image_bytes);
}

}  // namespace

FrameRingWriter::~FrameRingWriter()
{
    if (base_ != nullptr) { ::munmap(base_, bytes_); }
    if (fd_ >= 0)         { ::close(fd_); }
    // The segment is deliberately NOT unlinked. EM may restart this process,
    // and shm_unlink here would race a reader still holding a mapping. A new
    // instanceId is what tells consumers the contents changed.
}

bool FrameRingWriter::create(int width, int height)
{
    image_bytes_ = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3U;
    bytes_       = segment_bytes(image_bytes_);

    fd_ = ::shm_open(kRingName, O_CREAT | O_RDWR, 0660);
    if (fd_ < 0)
    {
        VP_ERROR("frame ring: shm_open(%s) failed: %s", kRingName, std::strerror(errno));
        return false;
    }

    if (::ftruncate(fd_, static_cast<off_t>(bytes_)) != 0)
    {
        // On WSL2 the usual cause is /dev/shm being too small — check with
        // `df -h /dev/shm`; three slots of two 1024x512x3 images need ~9.5 MB.
        VP_ERROR("frame ring: ftruncate(%zu) failed: %s", bytes_, std::strerror(errno));
        ::close(fd_); fd_ = -1;
        return false;
    }

    base_ = ::mmap(nullptr, bytes_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (base_ == MAP_FAILED)
    {
        VP_ERROR("frame ring: mmap failed: %s", std::strerror(errno));
        base_ = nullptr;
        ::close(fd_); fd_ = -1;
        return false;
    }

    // A fresh instance id on every create(). A consumer holding the previous
    // value re-maps rather than reading a segment whose geometry may differ.
    // Monotonic-clock nanoseconds truncated to 32 bits: never zero, never
    // repeats across a restart.
    struct timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    instance_id_ = static_cast<std::uint32_t>(ts.tv_nsec) ^
                   static_cast<std::uint32_t>(ts.tv_sec << 8);
    if (instance_id_ == 0U) instance_id_ = 1U;

    RingHeader* h = header(base_);
    h->slots      = kSlots;
    h->width      = static_cast<std::uint32_t>(width);
    h->height     = static_cast<std::uint32_t>(height);
    h->imageBytes = static_cast<std::uint32_t>(image_bytes_);

    for (std::uint32_t s = 0; s < kSlots; ++s)
    {
        auto* sh = reinterpret_cast<SlotHeader*>(slot_at(base_, image_bytes_, s));
        sh->generation.store(0U, std::memory_order_relaxed);
        sh->seq         = 0U;
        sh->timestampNs = 0U;
    }

    // Published last: a reader that sees the id knows the geometry above is
    // already in place.
    h->instanceId.store(instance_id_, std::memory_order_release);

    VP_INFO("frame ring: created %s  %dx%d  %u slots  %.1f MB  instance=%u",
            kRingName, width, height, kSlots,
            static_cast<double>(bytes_) / (1024.0 * 1024.0), instance_id_);
    return true;
}

std::uint32_t FrameRingWriter::write(const cv::Mat& warped, const cv::Mat& resized,
                                     std::uint32_t seq, std::uint64_t timestamp_ns,
                                     std::uint32_t& out_generation)
{
    const std::uint32_t slot = seq % kSlots;
    auto* raw = slot_at(base_, image_bytes_, slot);
    auto* sh  = reinterpret_cast<SlotHeader*>(raw);
    auto* px  = raw + sizeof(SlotHeader);

    // Odd generation marks the slot as being written. A reader that catches it
    // here discards rather than returning half a frame.
    sh->generation.store(generation_ | kWritingMark, std::memory_order_release);

    std::memcpy(px, warped.data, image_bytes_);
    std::memcpy(px + image_bytes_, resized.data, image_bytes_);
    sh->seq         = seq;
    sh->timestampNs = timestamp_ns;

    generation_ += 2U;                       // stays even
    sh->generation.store(generation_, std::memory_order_release);

    out_generation = generation_;
    return slot;
}

}  // namespace visionpilot::ap
