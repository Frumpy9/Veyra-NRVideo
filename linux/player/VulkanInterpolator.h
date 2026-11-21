#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace veyra::linuxgpu {

// Diagnostic ingress/egress is packed, full-range, SDR RGBA8 (R in low byte).
// Frames A and B must belong to the same continuous, decoded source interval.
// The owner resets/recreates this object for a size or source-format change.
class VulkanInterpolator {
public:
    VulkanInterpolator(uint32_t width, uint32_t height,
                       const std::filesystem::path& shaderDirectory);
    ~VulkanInterpolator();
    VulkanInterpolator(const VulkanInterpolator&) = delete;
    VulkanInterpolator& operator=(const VulkanInterpolator&) = delete;

    // B must already exist: this operation cannot eliminate lookahead latency.
    // No timestamps are invented here. The caller owns the source timeline.
    std::vector<uint32_t> interpolate(std::span<const uint32_t> a,
                                      std::span<const uint32_t> b,
                                      float phase = 0.5f, uint32_t searchRadius = 8);
    const std::string& deviceName() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
