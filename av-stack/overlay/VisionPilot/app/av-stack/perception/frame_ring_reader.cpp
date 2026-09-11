#include "../common/frame_ring.hpp"

#include <cstring>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <logging/logger.hpp>

namespace visionpilot::ap {

namespace {

const RingHeader* header(const void* base) { return static_cast<const RingHeader*>(base); }

const std::uint8_t* slot_at(const void* base, std::size_t image_bytes, std::uint32_t slot)
{
    return static_cast<const std::uint8_t*>(base) + sizeof(RingHeader) +
           static_cast<std::size_t>(slot) * slot_stride(image_bytes);
}

}  // namespace

FrameRingReader::~FrameRingReader() { unmap(); }

void FrameRingReader::unmap()
{
    if (base_ != nullptr) { ::munmap(const_cast<void*>(base_), bytes_); base_ = nullptr; }
    if (fd_ >= 0)         { ::close(fd_); fd_ = -1; }
    instance_id_ = 0U;
}

bool FrameRingReader::ensure_mapped(std::uint32_t instance_id)
{
    if (base_ != nullptr && instance_id_ == instance_id) return true;   // common case

    // Either first call, or Sensing restarted and handed us a new id. Drop the
    // old mapping before taking a new one: continuing to read the previous
    // segment would look like perfectly valid but frozen frames.
    unmap();

    fd_ = ::shm_open(kRingName, O_RDONLY, 0);
    if (fd_ < 0) return false;                 // sensingd has not created it yet

    // Read the header first to learn the geometry, then map the whole segment.
    RingHeader probe{};
    if (::read(fd_, &probe, sizeof(probe)) != static_cast<ssize_t>(sizeof(probe)))
    {
        ::close(fd_); fd_ = -1;
        return false;
    }

    image_bytes_ = probe.imageBytes;
    width_       = static_cast<int>(probe.width);
    height_      = static_cast<int>(probe.height);
    bytes_       = segment_bytes(image_bytes_);

    void* p = ::mmap(nullptr, bytes_, PROT_READ, MAP_SHARED, fd_, 0);
    if (p == MAP_FAILED)
    {
        VP_ERROR("frame ring: mmap failed: %s", std::strerror(errno));
        ::close(fd_); fd_ = -1;
        return false;
    }
    base_ = p;

    instance_id_ = header(base_)->instanceId.load(std::memory_order_acquire);
    if (instance_id_ != instance_id)
    {
        // Raced a create() in progress. Next descriptor will bring us back.
        unmap();
        return false;
    }

    VP_INFO("frame ring: mapped %s  %dx%d  instance=%u", kRingName, width_, height_, instance_id_);
    return true;
}

bool FrameRingReader::read(std::uint32_t slot, std::uint32_t expected_generation,
                           cv::Mat& warped, cv::Mat& resized)
{
    if (base_ == nullptr || slot >= kSlots) return false;

    const auto* raw = slot_at(base_, image_bytes_, slot);
    const auto* sh  = reinterpret_cast<const SlotHeader*>(raw);
    const auto* px  = raw + sizeof(SlotHeader);

    // Before: the writer may already have moved on, or be mid-write (odd).
    if (sh->generation.load(std::memory_order_acquire) != expected_generation) return false;

    if (warped.rows != height_ || warped.cols != width_ || warped.type() != CV_8UC3)
        warped.create(height_, width_, CV_8UC3);
    if (resized.rows != height_ || resized.cols != width_ || resized.type() != CV_8UC3)
        resized.create(height_, width_, CV_8UC3);

    std::memcpy(warped.data,  px,                std::min<std::size_t>(image_bytes_, warped.total() * warped.elemSize()));
    std::memcpy(resized.data, px + image_bytes_, std::min<std::size_t>(image_bytes_, resized.total() * resized.elemSize()));

    // After: if the generation moved while we copied, the writer lapped us and
    // what we hold is a mix of two frames. Discard it. A ring that never
    // reports a discard under a deliberately slowed reader is not detecting
    // this race — it is losing it silently.
    return sh->generation.load(std::memory_order_acquire) == expected_generation;
}

}  // namespace visionpilot::ap
