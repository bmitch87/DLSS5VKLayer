// Exact sampling coordinates shared by the host detector and GPU proxy readback.
#pragma once
#include <cstdint>
namespace dlssnr {
inline uint32_t SceneGridWidth(uint32_t width) { return width < 64 ? width : 64; }
inline uint32_t SceneGridHeight(uint32_t height) { return height < 36 ? height : 36; }
inline uint32_t SceneSampleCoordinate(uint32_t sample, uint32_t extent, uint32_t count) {
    return count ? uint32_t((uint64_t(sample) * extent + count / 2) / count) : 0;
}
} // namespace dlssnr
