#include "../common/shm_protocol.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void FillFrame(uint8_t* px, uint32_t w, uint32_t h, int frame) {
    for (uint32_t y = 0; y < h; ++y) {
        uint32_t* row = (uint32_t*)px + (size_t)y * w;
        for (uint32_t x = 0; x < w; ++x) {
            uint32_t r = (x + frame * 3) & 0xFF;
            uint32_t g = (y + frame * 2) & 0xFF;
            uint32_t b = 128;
            row[x] = (b << 16) | (g << 8) | r;
        }
    }
    uint32_t bx = (uint32_t)(frame * 24) % (w - 128);
    uint32_t by = h / 2 - 64;
    for (uint32_t y = by; y < by + 128; ++y) {
        uint32_t* row = (uint32_t*)px + (size_t)y * w;
        for (uint32_t x = bx; x < bx + 128; ++x) row[x] = 0x00FFFFFFu;
    }
}

int main(int argc, char** argv) {
    const uint32_t w = argc > 1 ? (uint32_t)atoi(argv[1]) : 640;
    const uint32_t h = argc > 2 ? (uint32_t)atoi(argv[2]) : 360;
    const uint32_t frames = argc > 3 ? (uint32_t)atoi(argv[3]) : 420;
    const uint32_t settle = argc > 4 ? (uint32_t)atoi(argv[4]) : 250;
    std::string path = ShmDefaultPath();
    if (const char* e = getenv("DLSSNR_SHM"); e && *e) path = e;
    int fd = open(path.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd < 0) { printf("[soak] open %s failed\n", path.c_str()); return 1; }
    size_t total = 4096 + kMaxFrame * 2;
    struct stat st{};
    if (fstat(fd, &st) != 0 || (size_t)st.st_size < total) ftruncate(fd, (off_t)total);
    void* m = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { printf("[soak] mmap failed\n", 0); return 1; }
    ShmHeader* hdr = (ShmHeader*)m;
    uint8_t* inPx = (uint8_t*)m + 4096;
    if (hdr->magic.load() != kShmMagic || hdr->version.load() != kShmVersion ||
        hdr->passes.load() == 0) ShmInitDefaults(hdr);
    hdr->quit.store(0);
    hdr->width.store(w);
    hdr->height.store(h);
    hdr->format.store(0);
    hdr->enabled.store(1);
    hdr->rebuildSettleMs.store(settle);
    hdr->passes.store(4);
    hdr->controlSeq.fetch_add(1);
    printf("[soak] %ux%u frames=%u settle=%u passes=4 requested\n", w, h, frames, settle);

    const double t0 = Now();
    double lastFeatLog = -1;
    uint32_t lastFeat = 0;
    double worst = 0;
    int fails = 0;
    for (uint32_t i = 0; i < frames; ++i) {
        if (i == 120) {
            hdr->pass[2].overrideMask.fetch_or(kOverridePreset);
            hdr->pass[2].preset.store(3);
            hdr->tuningSeq.fetch_add(1);
            printf("[soak] t=%.2f frame %u: pass 2 preset -> 3\n", Now() - t0, i);
        }
        if (i == 200) {
            hdr->passes.store(2);
            hdr->controlSeq.fetch_add(1);
            printf("[soak] t=%.2f frame %u: passes -> 2\n", Now() - t0, i);
        }
        if (i == 260) {
            hdr->passes.store(4);
            hdr->controlSeq.fetch_add(1);
            printf("[soak] t=%.2f frame %u: passes -> 4\n", Now() - t0, i);
        }
        if (i == 320) {
            hdr->rebuildSettleMs.store(0);
            hdr->pass[0].overrideMask.fetch_or(kOverridePreset);
            hdr->pass[0].preset.store(5);
            hdr->tuningSeq.fetch_add(1);
            printf("[soak] t=%.2f frame %u: settle -> 0, pass 0 preset -> 5\n", Now() - t0, i);
        }
        if (i == 360) {
            hdr->rebuildSettleMs.store(settle);
            hdr->pass[1].overrideMask.fetch_or(kOverrideIntensity);
            hdr->pass[1].intensityBits.store(FloatToBits(2.0f));
            hdr->tuningSeq.fetch_add(1);
            printf("[soak] t=%.2f frame %u: settle -> %u, pass 1 intensity -> 2.0\n",
                   Now() - t0, i, settle);
        }

        FillFrame(inPx, w, h, (int)i);
        const uint32_t req = hdr->seq_req.load() + 1;
        hdr->seq_req.store(req);
        const double f0 = Now();
        bool ok = false;
        while (Now() - f0 < 60.0) {
            if (hdr->seq_resp.load() >= req) { ok = hdr->seq_ok.load() >= req; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const double lat = Now() - f0;
        if (lat > worst) worst = lat;
        if (!ok) { printf("[soak] frame %u NOT ok (lat %.2fs)\n", i, lat); ++fails; if (fails > 5) return 2; }
        const uint32_t feat = hdr->helperFeatures.load();
        if (feat != lastFeat) {
            printf("[soak] t=%.2f frame %u: helperFeatures %u -> %u\n", Now() - t0, i, lastFeat, feat);
            lastFeat = feat; lastFeatLog = Now() - t0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    printf("[soak] done: %u frames, worst latency %.2fs, fails %d, features %u, seq_ok=%u\n",
           frames, worst, fails, hdr->helperFeatures.load(), hdr->seq_ok.load());
    printf("%s\n", fails ? "SOAK FAIL" : "SOAK PASS");
    return fails ? 2 : 0;
}