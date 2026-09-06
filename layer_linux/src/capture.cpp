#include "capture.h"
#include "log.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO_STDLIB_DEFINED
#include "../../core/stb_image_write.h"

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <vector>

namespace dlssnr {
namespace {

void MakeDirs(const std::string& path) {
    size_t pos = 1;
    while ((pos = path.find('/', pos)) != std::string::npos) {
        mkdir(path.substr(0, pos).c_str(), 0700);
        ++pos;
    }
    mkdir(path.c_str(), 0700);
}

// Empties the directory without removing it. Only the files this writer makes are touched, so a
// mistyped path cannot take anything else with it.
void EmptyCaptureDir(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        const bool ours = name.rfind("before_", 0) == 0 || name.rfind("after_", 0) == 0 ||
                          name == "manifest.txt";
        if (ours) unlink((dir + "/" + name).c_str());
    }
    closedir(d);
}

// True when the format is four 8-bit channels, which is the only shape PNG can hold here.
bool IsEightBitRgba(uint32_t f) {
    switch (f) {
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
            return true;
        default:
            return false;
    }
}

// PNG wants RGBA byte order; a B8G8R8A8 surface has red and blue the other way round.
bool NeedsChannelSwap(uint32_t f) { return f == VK_FORMAT_B8G8R8A8_UNORM; }

size_t BytesPerPixel(uint32_t f) {
    return f == VK_FORMAT_R16G16B16A16_SFLOAT ? 8u : 4u;
}

void WritePng(const std::string& path, const void* pixels, uint32_t w, uint32_t h, bool swap) {
    const size_t px = size_t(w) * h;
    std::vector<uint8_t> rgba(px * 4);
    std::memcpy(rgba.data(), pixels, px * 4);
    if (swap) {
        for (size_t i = 0; i < px; ++i) std::swap(rgba[i * 4 + 0], rgba[i * 4 + 2]);
    }
    if (!stbi_write_png(path.c_str(), int(w), int(h), 4, rgba.data(), int(w) * 4))
        Log("[capture] could not write %s", path.c_str());
}

void WriteRaw(const std::string& path, const void* pixels, size_t bytes) {
    if (FILE* f = fopen(path.c_str(), "wb")) {
        fwrite(pixels, 1, bytes, f);
        fclose(f);
    } else {
        Log("[capture] could not write %s", path.c_str());
    }
}

}  // namespace

std::string CaptureWriter::Directory() {
    if (const char* state = getenv("XDG_STATE_HOME"); state && *state)
        return std::string(state) + "/dlssnr/captures";
    if (const char* home = getenv("HOME"); home && *home)
        return std::string(home) + "/.local/state/dlssnr/captures";
    return "/tmp/dlssnr-captures";
}

void CaptureWriter::Begin(uint32_t frames) {
    if (frames == 0) return;
    const std::string dir = Directory();
    MakeDirs(dir);
    if (!_cleared) {
        EmptyCaptureDir(dir);
        _cleared = true;
    }
    _remaining = frames;
    _index = 0;
    Log("[capture] capturing %u frames to %s", frames, dir.c_str());
}

void CaptureWriter::WriteFrame(const void* before, const void* after, uint32_t width, uint32_t height,
                               uint32_t vkFormat) {
    if (_remaining == 0) return;

    const std::string dir = Directory();
    const bool png = IsEightBitRgba(vkFormat);
    const bool swap = NeedsChannelSwap(vkFormat);
    const size_t bytes = size_t(width) * height * BytesPerPixel(vkFormat);

    char name[64];
    if (png) {
        std::snprintf(name, sizeof(name), "/before_%02u.png", _index);
        WritePng(dir + name, before, width, height, swap);
        std::snprintf(name, sizeof(name), "/after_%02u.png", _index);
        WritePng(dir + name, after, width, height, swap);
    } else {
        std::snprintf(name, sizeof(name), "/before_%02u.raw", _index);
        WriteRaw(dir + name, before, bytes);
        std::snprintf(name, sizeof(name), "/after_%02u.raw", _index);
        WriteRaw(dir + name, after, bytes);
    }

    ++_index;
    --_remaining;

    if (_remaining == 0) {
        WriteManifest(width, height, vkFormat, png);
        Log("[capture] wrote %u pairs to %s", _index, dir.c_str());
    }
}

void CaptureWriter::WriteManifest(uint32_t width, uint32_t height, uint32_t vkFormat, bool png) const {
    const std::string path = Directory() + "/manifest.txt";
    FILE* f = fopen(path.c_str(), "wt");
    if (!f) return;
    std::fprintf(f, "frames %u\n", _index);
    std::fprintf(f, "width %u\nheight %u\n", width, height);
    std::fprintf(f, "vk_format %u\n", vkFormat);
    std::fprintf(f, "encoding %s\n", png ? "png" : "raw");
    std::fprintf(f, "bytes_per_pixel %zu\n", BytesPerPixel(vkFormat));
    std::fprintf(f, "row_pitch %zu\n", BytesPerPixel(vkFormat) * width);
    std::fprintf(f,
                 "\n"
                 "before_NN is the frame as the game presented it; after_NN is the same frame with\n"
                 "the model's edit composed onto it. Same frame, same run, one variable.\n");
    fclose(f);
}

}  // namespace dlssnr
