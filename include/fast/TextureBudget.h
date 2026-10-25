#pragma once

#include <algorithm>
#include <cstdint>

namespace Fast {
// GPU texture storage only; decoded resources and framebuffers have separate lifetimes.
constexpr uint64_t TextureBudgetBytes(uint64_t physicalBytes, int overrideMiB = 0) {
    constexpr uint64_t MiB = 1024 * 1024;
    if (overrideMiB == -1) {
        return UINT64_MAX; // Comparison run without byte-budget eviction.
    }
    if (overrideMiB > 0) {
        return std::clamp(overrideMiB, 64, 512) * MiB;
    }
    return physicalBytes == 0 ? 128 * MiB : std::clamp(physicalBytes / 32, 128 * MiB, 512 * MiB);
}

constexpr uint64_t TextureStorageBytes(uint32_t width, uint32_t height) {
    return uint64_t(width) * height * 4;
}

constexpr bool TextureCanBeEvicted(uint64_t lastUsedFrame, uint64_t currentFrame, bool bound) {
    // Keep recently used textures through short camera changes instead of reuploading them.
    return !bound && currentFrame - lastUsedFrame >= 60;
}
} // namespace Fast
