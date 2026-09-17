// capture_metrics -- read the matched before/after pairs CaptureWriter produces and
// put numbers on them.
//
// The layer already writes the pairs (layer_linux/src/capture.cpp) and a manifest, and
// until now there was nothing to read them with, so every question the settings raise
// ("does matched residual keep detail a bilinear stretch loses?", "what does the guard
// cost at 4?") had no answer but an opinion.
//
// Two sharpness metrics, reported together and never one alone, because they fail
// differently:
//   * Laplacian variance -- the classic focus measure. Rises with fine detail AND with
//     noise, so a pass that adds grain scores well on it.
//   * Sobel gradient energy -- steadier, far less sensitive to single-pixel noise.
// A pass that raises the first and not the second added noise, not detail. That pairing,
// and the border discard below, come from dlss5-for-all's tests/nitidez_png.cpp (MIT);
// the metrics themselves are standard image processing.
//
// A 16-pixel border is discarded: a window frame, a letterbox edge or the compare
// overlay's divider is all high-frequency content that is not the thing being measured.
//
// Deliberately NOT ported: their comparar_png.cpp searches for the integer shift that
// minimises the error before reporting it, because their two images come from separate
// runs. Ours are the same frame from one run with one variable changed -- there is no
// misalignment to search for. Do not add it.
//
// usage: capture_metrics [directory]
//        capture_metrics <reference-dir> <candidate-dir>   compare two runs' answers
//   directory defaults to the same place CaptureWriter::Directory() resolves:
//   $XDG_STATE_HOME/dlssnr/captures, else ~/.local/state/dlssnr/captures, else /tmp.

#define STB_IMAGE_IMPLEMENTATION
#include "../standalone_runner/third_party/stb_image.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Manifest {
    uint32_t frames = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t vkFormat = 0;
    std::string encoding;  // "png" or "raw"
    uint32_t bytesPerPixel = 4;
};

// Mirrors CaptureWriter::Directory() so the tool defaults to where the layer wrote.
std::string DefaultDirectory() {
    const char* state = getenv("XDG_STATE_HOME");
    if (state && *state) return std::string(state) + "/dlssnr/captures";
    const char* home = getenv("HOME");
    if (home && *home) return std::string(home) + "/.local/state/dlssnr/captures";
    return "/tmp/dlssnr/captures";
}

bool ReadManifest(const std::string& dir, Manifest& m) {
    const std::string path = dir + "/manifest.txt";
    FILE* f = fopen(path.c_str(), "rt");
    if (!f) {
        std::fprintf(stderr, "capture_metrics: cannot open %s\n", path.c_str());
        return false;
    }
    char key[64];
    char value[256];
    while (std::fscanf(f, "%63s %255s", key, value) == 2) {
        if (!std::strcmp(key, "frames")) m.frames = uint32_t(atoi(value));
        else if (!std::strcmp(key, "width")) m.width = uint32_t(atoi(value));
        else if (!std::strcmp(key, "height")) m.height = uint32_t(atoi(value));
        else if (!std::strcmp(key, "vk_format")) m.vkFormat = uint32_t(atoi(value));
        else if (!std::strcmp(key, "encoding")) m.encoding = value;
        else if (!std::strcmp(key, "bytes_per_pixel")) m.bytesPerPixel = uint32_t(atoi(value));
    }
    fclose(f);
    if (!m.frames || !m.width || !m.height) {
        std::fprintf(stderr, "capture_metrics: %s is incomplete\n", path.c_str());
        return false;
    }
    return true;
}

// Half-float decode by hand, subnormals included: the same reason helper/main.cpp has
// its own -- there is no portable _cvtsh_ss here either.
float HalfToFloat(uint16_t h) {
    const uint32_t sign = uint32_t(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {
            exp = 127 - 15 + 1;
            while ((mant & 0x400u) == 0) { mant <<= 1; --exp; }
            mant &= 0x3FFu;
            bits = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

constexpr float kLumaR = 0.2126f, kLumaG = 0.7152f, kLumaB = 0.0722f;

// One luminance plane per image, in whatever units the capture carries: 0..1 for the
// 8-bit paths, scene-referred light for the float16 one. Every metric below is
// scale-relative except PSNR, which is reported against the measured peak rather than
// an assumed 1.0 for exactly that reason.
bool LoadLuma(const std::string& path, const Manifest& m, std::vector<float>& out) {
    const size_t pixels = size_t(m.width) * m.height;
    out.assign(pixels, 0.0f);

    if (m.encoding == "png") {
        int w = 0, h = 0, comp = 0;
        unsigned char* data = stbi_load(path.c_str(), &w, &h, &comp, 4);
        if (!data) {
            std::fprintf(stderr, "capture_metrics: cannot read %s (%s)\n", path.c_str(),
                         stbi_failure_reason() ? stbi_failure_reason() : "unknown");
            return false;
        }
        if (uint32_t(w) != m.width || uint32_t(h) != m.height) {
            std::fprintf(stderr, "capture_metrics: %s is %dx%d, manifest says %ux%u\n", path.c_str(),
                         w, h, m.width, m.height);
            stbi_image_free(data);
            return false;
        }
        // WritePng already put the channels in RGBA order, swapping where the swapchain
        // was B8G8R8A8, so no second swap belongs here.
        for (size_t i = 0; i < pixels; ++i) {
            out[i] = (kLumaR * float(data[i * 4 + 0]) + kLumaG * float(data[i * 4 + 1]) +
                      kLumaB * float(data[i * 4 + 2])) / 255.0f;
        }
        stbi_image_free(data);
        return true;
    }

    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        std::fprintf(stderr, "capture_metrics: cannot open %s\n", path.c_str());
        return false;
    }
    const size_t bytes = pixels * m.bytesPerPixel;
    std::vector<uint8_t> raw(bytes);
    const size_t got = fread(raw.data(), 1, bytes, f);
    fclose(f);
    if (got != bytes) {
        std::fprintf(stderr, "capture_metrics: %s is %zu bytes, expected %zu\n", path.c_str(), got,
                     bytes);
        return false;
    }

    if (m.bytesPerPixel == 8) {  // R16G16B16A16_SFLOAT
        for (size_t i = 0; i < pixels; ++i) {
            uint16_t c[3];
            std::memcpy(c, raw.data() + i * 8, sizeof(c));
            out[i] = kLumaR * HalfToFloat(c[0]) + kLumaG * HalfToFloat(c[1]) +
                     kLumaB * HalfToFloat(c[2]);
        }
    } else {
        // The raw path preserves the swapchain's own byte order, unlike the PNG path.
        // 44 is VK_FORMAT_B8G8R8A8_UNORM; anything else 8-bit here is R-first.
        const bool bgr = m.vkFormat == 44;
        for (size_t i = 0; i < pixels; ++i) {
            const uint8_t* p = raw.data() + i * 4;
            const float r = float(bgr ? p[2] : p[0]);
            const float g = float(p[1]);
            const float b = float(bgr ? p[0] : p[2]);
            out[i] = (kLumaR * r + kLumaG * g + kLumaB * b) / 255.0f;
        }
    }
    return true;
}

constexpr int kBorder = 16;

struct Sharpness {
    double laplacianVariance = 0.0;
    double sobelEnergy = 0.0;
};

Sharpness Measure(const std::vector<float>& luma, uint32_t w, uint32_t h) {
    Sharpness s;
    if (int(w) <= 2 * kBorder + 2 || int(h) <= 2 * kBorder + 2) return s;

    const auto at = [&](int x, int y) { return double(luma[size_t(y) * w + x]); };

    double sum = 0.0, sumSq = 0.0, grad = 0.0;
    size_t n = 0;
    for (int y = kBorder; y < int(h) - kBorder; ++y) {
        for (int x = kBorder; x < int(w) - kBorder; ++x) {
            const double lap = at(x - 1, y) + at(x + 1, y) + at(x, y - 1) + at(x, y + 1) -
                               4.0 * at(x, y);
            sum += lap;
            sumSq += lap * lap;

            const double gx = (at(x + 1, y - 1) + 2.0 * at(x + 1, y) + at(x + 1, y + 1)) -
                              (at(x - 1, y - 1) + 2.0 * at(x - 1, y) + at(x - 1, y + 1));
            const double gy = (at(x - 1, y + 1) + 2.0 * at(x, y + 1) + at(x + 1, y + 1)) -
                              (at(x - 1, y - 1) + 2.0 * at(x, y - 1) + at(x + 1, y - 1));
            grad += gx * gx + gy * gy;
            ++n;
        }
    }
    if (!n) return s;
    const double mean = sum / double(n);
    s.laplacianVariance = sumSq / double(n) - mean * mean;
    s.sobelEnergy = grad / double(n);
    return s;
}

// PSNR against the measured peak rather than an assumed 1.0: the float16 capture path
// carries scene light, where a peak of 200 is ordinary and a fixed peak would report a
// meaningless number.
double Psnr(const std::vector<float>& a, const std::vector<float>& b, uint32_t w, uint32_t h) {
    if (int(w) <= 2 * kBorder || int(h) <= 2 * kBorder) return 0.0;
    double mse = 0.0, peak = 0.0;
    size_t n = 0;
    for (int y = kBorder; y < int(h) - kBorder; ++y) {
        for (int x = kBorder; x < int(w) - kBorder; ++x) {
            const size_t i = size_t(y) * w + x;
            const double d = double(a[i]) - double(b[i]);
            mse += d * d;
            if (double(a[i]) > peak) peak = double(a[i]);
            ++n;
        }
    }
    if (!n) return 0.0;
    mse /= double(n);
    if (mse <= 0.0) return INFINITY;
    if (peak <= 0.0) peak = 1.0;
    return 10.0 * std::log10((peak * peak) / mse);
}

bool FileExists(const std::string& p) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

}  // namespace

// Two directories: compare each round's ANSWER against the other's, rather than each round's answer
// against its own input.
//
// The one-directory mode answers "how far did the pass move this picture". It cannot answer "which
// of these two settings is closer to the reference", because each round's PSNR is measured against
// its own before_ image and the two rounds never meet. That is the question the reconstruction
// filter needs: capture at 100%, capture again at 50% with each filter, and compare each 50% answer
// against the 100% one.
//
// Both directories must hold the same frames of the same picture, which is what holdFrame is for.
// The raster and encoding are required to match and the run is refused otherwise -- comparing two
// different pictures and reporting a number is the failure this whole tool exists to avoid.
static int CompareDirs(const std::string& a, const std::string& b) {
    Manifest ma, mb;
    if (!ReadManifest(a, ma) || !ReadManifest(b, mb)) return 1;
    if (ma.width != mb.width || ma.height != mb.height || ma.encoding != mb.encoding ||
        ma.bytesPerPixel != mb.bytesPerPixel) {
        std::fprintf(stderr,
                     "capture_metrics: %s is %ux%u %s and %s is %ux%u %s -- these are not two "
                     "versions of the same picture, so there is nothing to compare.\n",
                     a.c_str(), ma.width, ma.height, ma.encoding.c_str(), b.c_str(), mb.width,
                     mb.height, mb.encoding.c_str());
        return 1;
    }

    std::printf("reference       %s\n", a.c_str());
    std::printf("candidate       %s\n", b.c_str());
    std::printf("raster          %ux%u\n", ma.width, ma.height);
    std::printf("border discard  %d px\n\n", kBorder);
    std::printf("Each row is the candidate's ANSWER against the reference's answer for the same\n");
    std::printf("frame. Higher PSNR is closer to the reference; the sharpness columns say which\n");
    std::printf("way it differs, since two settings can be equally far apart and one of them\n");
    std::printf("softer.\n\n");
    std::printf("%-6s  %14s %14s  %14s %14s  %10s\n", "frame", "lap(ref)", "lap(cand)",
                "sobel(ref)", "sobel(cand)", "PSNR dB");

    const char* ext = ma.encoding == "png" ? "png" : "raw";
    const uint32_t frames = ma.frames < mb.frames ? ma.frames : mb.frames;
    std::vector<float> ra, ca;
    double lapR = 0, lapC = 0, sobR = 0, sobC = 0, psnr = 0;
    uint32_t counted = 0;
    for (uint32_t i = 0; i < frames; ++i) {
        char na[512], nb[512];
        std::snprintf(na, sizeof(na), "%s/after_%02u.%s", a.c_str(), i, ext);
        std::snprintf(nb, sizeof(nb), "%s/after_%02u.%s", b.c_str(), i, ext);
        if (!FileExists(na) || !FileExists(nb)) continue;
        if (!LoadLuma(na, ma, ra) || !LoadLuma(nb, mb, ca)) return 1;

        const Sharpness sr = Measure(ra, ma.width, ma.height);
        const Sharpness sc = Measure(ca, ma.width, ma.height);
        const double p = Psnr(ra, ca, ma.width, ma.height);
        std::printf("%-6u  %14.6g %14.6g  %14.6g %14.6g  %10.2f\n", i, sr.laplacianVariance,
                    sc.laplacianVariance, sr.sobelEnergy, sc.sobelEnergy, p);
        lapR += sr.laplacianVariance;
        lapC += sc.laplacianVariance;
        sobR += sr.sobelEnergy;
        sobC += sc.sobelEnergy;
        psnr += std::isinf(p) ? 0.0 : p;
        ++counted;
    }
    if (!counted) {
        std::fprintf(stderr, "capture_metrics: no matching after_NN frames in %s and %s\n",
                     a.c_str(), b.c_str());
        return 1;
    }
    std::printf("\n%-6s  %14.6g %14.6g  %14.6g %14.6g  %10.2f\n", "mean", lapR / counted,
                lapC / counted, sobR / counted, sobC / counted, psnr / counted);
    std::printf("\nlaplacian candidate/reference  %.4f\n", lapR > 0 ? lapC / lapR : 0.0);
    std::printf("sobel     candidate/reference  %.4f\n", sobR > 0 ? sobC / sobR : 0.0);
    std::printf("\nA candidate at the same PSNR but a HIGHER laplacian ratio is not closer to the\n");
    std::printf("reference, it is differently wrong and sharper. Read the two together.\n");
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 2) return CompareDirs(argv[1], argv[2]);
    const std::string dir = argc > 1 ? argv[1] : DefaultDirectory();

    Manifest m;
    if (!ReadManifest(dir, m)) return 1;

    std::printf("directory       %s\n", dir.c_str());
    std::printf("frames          %u\n", m.frames);
    std::printf("raster          %ux%u\n", m.width, m.height);
    std::printf("vk_format       %u\n", m.vkFormat);
    std::printf("encoding        %s (%u bytes/pixel)\n", m.encoding.c_str(), m.bytesPerPixel);
    std::printf("border discard  %d px\n\n", kBorder);
    std::printf("Laplacian variance rises with detail AND with noise; Sobel energy is steadier.\n");
    std::printf("A change that raises one and not the other added noise, not detail.\n\n");
    std::printf("%-6s  %14s %14s  %14s %14s  %10s\n", "frame", "lap(before)", "lap(after)",
                "sobel(before)", "sobel(after)", "PSNR dB");

    const char* ext = m.encoding == "png" ? "png" : "raw";
    std::vector<float> before, after;
    double lapB = 0, lapA = 0, sobB = 0, sobA = 0, psnr = 0;
    uint32_t counted = 0;

    for (uint32_t i = 0; i < m.frames; ++i) {
        char nb[64], na[64];
        std::snprintf(nb, sizeof(nb), "%s/before_%02u.%s", dir.c_str(), i, ext);
        std::snprintf(na, sizeof(na), "%s/after_%02u.%s", dir.c_str(), i, ext);
        if (!FileExists(nb) || !FileExists(na)) continue;
        if (!LoadLuma(nb, m, before) || !LoadLuma(na, m, after)) return 1;

        const Sharpness b = Measure(before, m.width, m.height);
        const Sharpness a = Measure(after, m.width, m.height);
        const double p = Psnr(before, after, m.width, m.height);

        std::printf("%-6u  %14.6g %14.6g  %14.6g %14.6g  %10.2f\n", i, b.laplacianVariance,
                    a.laplacianVariance, b.sobelEnergy, a.sobelEnergy, p);

        lapB += b.laplacianVariance;
        lapA += a.laplacianVariance;
        sobB += b.sobelEnergy;
        sobA += a.sobelEnergy;
        psnr += std::isinf(p) ? 0.0 : p;
        ++counted;
    }

    if (!counted) {
        std::fprintf(stderr, "capture_metrics: no before/after pairs found in %s\n", dir.c_str());
        return 1;
    }

    std::printf("\n%-6s  %14.6g %14.6g  %14.6g %14.6g  %10.2f\n", "mean", lapB / counted,
                lapA / counted, sobB / counted, sobA / counted, psnr / counted);
    std::printf("\nlaplacian after/before  %.4f\n", lapB > 0 ? lapA / lapB : 0.0);
    std::printf("sobel     after/before  %.4f\n", sobB > 0 ? sobA / sobB : 0.0);
    return 0;
}
