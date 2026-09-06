// dlssnr-shmctl — read and write the shared-memory header from shell.
//
// This exists because the helper CLI used to poke the header with
//
//     printf '\x01\x00\x00\x00' | dd of="$shm" bs=1 seek=20 count=4 conv=notrunc
//
// which is a hardcoded byte offset into a C++ struct. It was correct for exactly one layout: the
// field at offset 20 was `quit` in the v1 header and is `height` in v2, so the same line that used
// to stop the helper would instead have silently corrupted the frame size. Nothing about the shell
// could have caught that.
//
// Everything here derives its offsets from shm_protocol.h by including it, so the header and the
// tool cannot disagree. Adding a field is now a recompile rather than an arithmetic exercise.
#include "../common/shm_protocol.h"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

// Every setting the header carries, by name, so the shell can drive the pass before there is an
// interface for it. Table-driven on purpose: a field added to the header and not to this table is a
// setting nobody can reach, and the table is short enough that the omission is obvious.
struct Setting {
    const char* name;
    std::atomic<uint32_t> ShmHeader::*field;
    bool isFloat;
    const char* help;
};

const Setting kSettings[] = {
    { "enabled", &ShmHeader::enabled, false, "0/1 run the model at all" },
    { "passes", &ShmHeader::passes, false, "how many times the model runs over one frame" },
    { "unlockpasses", &ShmHeader::unlockPasses, false, "0/1 lift the pass ceiling" },
    { "preset", &ShmHeader::preset, false, "model preset" },
    { "style", &ShmHeader::style, false, "0 default, 1 natural, 2 cinematic" },
    { "automask", &ShmHeader::autoMask, false, "0/1 automatic skin mask" },
    { "intensity", &ShmHeader::intensityBits, true, "model intensity" },
    { "localtone", &ShmHeader::localToneBits, true, "local tone strength" },
    { "localstructure", &ShmHeader::localStructureBits, true, "local structure strength" },
    { "skinstructure", &ShmHeader::skinStructureBits, true, "-1 follows local structure" },
    { "sharpness", &ShmHeader::sharpnessBits, true, "sharpness" },
    { "detail", &ShmHeader::transferStrengthBits, true, "how much of the edit lands, 0-4" },
    { "colour", &ShmHeader::colourStrengthBits, true, "how much of its colour comes with it, 0-4" },
    { "guard", &ShmHeader::maxRatioBits, true, "highlight guard, the most a pixel may move" },
    { "transfer", &ShmHeader::transfer, false, "0 classic, 1 matched residual, 2 native + edit" },
    { "mvec", &ShmHeader::mvecEnabled, false, "estimate motion vectors from the frames, 0 or 1" },
    { "mvecquality", &ShmHeader::mvecQuality, false, "0 fast, 1 balanced, 2 quality" },
    { "mvecunits", &ShmHeader::mvecScaleMode, false, "0 normalised, 1 pixels, 2 uv 0..1" },
    { "debugview", &ShmHeader::debugView, false, "0 off, 1 proxy, 2 model, 3 amplified edit" },
    { "debugscale", &ShmHeader::debugScaleBits, true, "what the debug views are multiplied by" },
    { "whitepoint", &ShmHeader::whitePointBits, true, "paper white" },
    { "whitepointscale", &ShmHeader::whitePointScaleBits, true, "multiplier on the white point" },
    { "whitepointsource", &ShmHeader::whitePointSource, false, "0 the slider, 1 measured off the frame" },
    { "whitepointtrim", &ShmHeader::whitePointTrimBits, true, "multiplier on a measured white point" },
    { "workingscale", &ShmHeader::workingScaleBits, true, "the fraction of the frame the model works at" },
    { "downscaler", &ShmHeader::scalingDownscaler, false, "1 bicubic, 2 catmull, 3 lanczos2, 4 lanczos3, 5 kaiser2, 6 kaiser3, 7 magic" },
    { "compare", &ShmHeader::compareMode, false, "0 off, 1 side by side, 2 wipe" },
    { "comparesplit", &ShmHeader::compareSplitBits, true, "where the split sits, 0-1" },
    { "comparezoom", &ShmHeader::compareZoomBits, true, "side by side only, 1-2" },
    { "compareswap", &ShmHeader::compareSwap, false, "0/1 which side the edited frame is on" },
    { "colourmode", &ShmHeader::colourMode, false, "0 auto, 1 display-referred, 2 linear HDR" },
    { "reversible", &ShmHeader::reversibleMode, false, "0 knee, 1 neutwo, 2 replace, 3 hybrid, 4 hybrid+replace" },
    { "applymodel", &ShmHeader::applyModel, false, "0 show the clean frame, 1 apply the edit" },
    { "hold", &ShmHeader::holdFrame, false, "0/1 freeze the frame the pass works on" },
    { "togglekey", &ShmHeader::toggleKey, false, "Linux KEY_ code the layer watches, 0 for none" },
};

void Usage() {
    std::fprintf(stderr,
                 "usage: dlssnr-shmctl <shm-path> <command> [args]\n"
                 "\n"
                 "commands:\n"
                 "  status          print the header, one 'key=value' per line\n"
                 "  quit            ask the helper and the layer to stand down\n"
                 "  resume          clear the quit flag and nudge the readers\n"
                 "  reset           re-initialise the whole header to defaults\n"
                 "  capture <n>     write n matched before/after frames\n"
                 "  toggle <key>    flip a setting between 0 and 1\n"
                 "  set <key> <v>   change one setting\n"
                 "  settings        list the settings and their current values\n");
    std::fprintf(stderr, "\nsettings:\n");
    for (const auto& s : kSettings) std::fprintf(stderr, "  %-16s %s\n", s.name, s.help);
}

// Maps the header only. The two pixel regions are megabytes and nothing here reads them.
ShmHeader* MapHeader(const char* path, bool create, void** base, int* fdOut) {
    const int flags = create ? (O_RDWR | O_CREAT) : O_RDWR;
    const int fd = open(path, flags, 0600);
    if (fd < 0) return nullptr;

    struct stat st {};
    if (fstat(fd, &st) != 0) {
        close(fd);
        return nullptr;
    }

    if ((size_t) st.st_size < ShmTotalBytes()) {
        if (!create) {  // nothing has ever attached; there is nothing to talk to
            close(fd);
            return nullptr;
        }
        if (ftruncate(fd, (off_t) ShmTotalBytes()) != 0) {
            close(fd);
            return nullptr;
        }
    }

    void* m = mmap(nullptr, kHeaderBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) {
        close(fd);
        return nullptr;
    }

    *base = m;
    *fdOut = fd;
    return (ShmHeader*) m;
}

bool Initialised(const ShmHeader* h) {
    return h->magic.load() == kShmMagic && h->version.load() == kShmVersion;
}

void PrintSettings(ShmHeader* h) {
    for (const auto& s : kSettings) {
        const uint32_t raw = (h->*s.field).load();
        if (s.isFloat) std::printf("%s=%g\n", s.name, double(BitsToFloat(raw)));
        else std::printf("%s=%u\n", s.name, raw);
    }
}

// Returns false when the name is not a setting, so the caller can say so rather than silently
// succeeding at nothing.
bool ApplySetting(ShmHeader* h, const char* name, const char* value) {
    for (const auto& s : kSettings) {
        if (std::strcmp(s.name, name) != 0) continue;
        const double v = std::atof(value);
        (h->*s.field).store(s.isFloat ? FloatToBits(float(v)) : uint32_t(v < 0 ? 0 : v));
        h->controlSeq.fetch_add(1);
        // Anything the model latches when its feature is built also bumps the tuning sequence, which
        // is what tells the helper to rebuild rather than to keep using a feature built with the old
        // values. Sharpness is absent: it is read at evaluate, so a running feature follows it.
        static const char* kCreateTime[] = { "preset", "style", "automask", "intensity",
                                             "localtone", "localstructure", "skinstructure", "passes" };
        for (const char* k : kCreateTime) {
            if (std::strcmp(k, name) == 0) { h->tuningSeq.fetch_add(1); break; }
        }
        return true;
    }
    return false;
}

void PrintStatus(const ShmHeader* h) {
    std::printf("initialised=%d\n", Initialised(h) ? 1 : 0);
    std::printf("magic=%#x\nversion=%u\n", h->magic.load(), h->version.load());
    if (!Initialised(h)) return;
    std::printf("seq_req=%u\nseq_resp=%u\n", h->seq_req.load(), h->seq_resp.load());
    std::printf("width=%u\nheight=%u\n", h->width.load(), h->height.load());
    std::printf("quit=%u\nheartbeat=%u\ncontrol_seq=%u\n", h->quit.load(), h->heartbeat.load(),
                h->controlSeq.load());
    std::printf("helper_state=%u\nmodel_up=%u\nhelper_frames=%llu\n", h->helperState.load(),
                h->modelUp.load(),
                (unsigned long long) ShmLoad64(h->helperFramesLo, h->helperFramesHi));
    std::printf("layer_composition_up=%u\nlayer_frames=%llu\nlayer_ms=%.2f\n",
                h->layerCompositionUp.load(),
                (unsigned long long) ShmLoad64(h->layerFramesLo, h->layerFramesHi),
                double(BitsToFloat(h->layerMsBits.load())));
    std::printf("measured_white_point=%g\n", double(BitsToFloat(h->layerMeasuredWhiteBits.load())));
    const std::string reason = ShmLoadString(h->helperReasonSeq, h->helperReason, kReasonBytes);
    if (!reason.empty()) std::printf("helper_reason=%s\n", reason.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        Usage();
        return 2;
    }
    const char* path = argv[1];
    const char* cmd = argv[2];

    const bool create = std::strcmp(cmd, "resume") == 0 || std::strcmp(cmd, "reset") == 0 ||
                        std::strcmp(cmd, "set") == 0 || std::strcmp(cmd, "capture") == 0 ||
                        std::strcmp(cmd, "toggle") == 0;

    void* base = nullptr;
    int fd = -1;
    ShmHeader* h = MapHeader(path, create, &base, &fd);
    if (!h) {
        // No mapping and none wanted: for 'quit' that means nothing is running, which is success.
        return std::strcmp(cmd, "quit") == 0 ? 0 : 1;
    }

    int rc = 0;
    if (std::strcmp(cmd, "status") == 0) {
        PrintStatus(h);
    } else if (std::strcmp(cmd, "quit") == 0) {
        if (Initialised(h)) {
            h->quit.store(1);
            h->controlSeq.fetch_add(1);
        }
    } else if (std::strcmp(cmd, "resume") == 0) {
        if (!Initialised(h)) ShmInitDefaults(h);
        h->quit.store(0);
        h->controlSeq.fetch_add(1);
    } else if (std::strcmp(cmd, "reset") == 0) {
        ShmInitDefaults(h);
    } else if (std::strcmp(cmd, "settings") == 0) {
        if (!Initialised(h)) ShmInitDefaults(h);
        PrintSettings(h);
    } else if (std::strcmp(cmd, "capture") == 0) {
        if (argc != 4) { Usage(); rc = 2; }
        else {
            if (!Initialised(h)) ShmInitDefaults(h);
            h->captureRequest.store(uint32_t(std::atoi(argv[3])));
            h->controlSeq.fetch_add(1);
        }
    } else if (std::strcmp(cmd, "toggle") == 0) {
        // The one command worth binding to a key.
        //
        // On Wayland a game is a client and its keys never reach this process, and /dev/input is not
        // readable without the 'input' group -- keyboards get no uaccess ACL, deliberately, because
        // that would let any program keylog. So on a Wayland game the layer cannot read a key at all,
        // and the way to get an in-game toggle is to bind this command to a shortcut in the desktop's
        // own settings, where the compositor already has the key and will deliver it over a fullscreen
        // window.
        if (argc != 4) { Usage(); rc = 2; }
        else {
            if (!Initialised(h)) ShmInitDefaults(h);
            bool found = false;
            for (const auto& st : kSettings) {
                if (std::strcmp(st.name, argv[3]) != 0) continue;
                found = true;
                const uint32_t now = (h->*st.field).load();
                const uint32_t next = now ? 0u : 1u;
                (h->*st.field).store(st.isFloat ? FloatToBits(float(next)) : next);
                h->controlSeq.fetch_add(1);
                std::printf("%s=%u\n", st.name, next);
                break;
            }
            if (!found) {
                std::fprintf(stderr, "unknown setting: %s\n", argv[3]);
                rc = 2;
            }
        }
    } else if (std::strcmp(cmd, "set") == 0) {
        if (argc != 5) { Usage(); rc = 2; }
        else {
            if (!Initialised(h)) ShmInitDefaults(h);
            if (!ApplySetting(h, argv[3], argv[4])) {
                std::fprintf(stderr, "unknown setting: %s\n", argv[3]);
                rc = 2;
            }
        }
    } else {
        Usage();
        rc = 2;
    }

    msync(base, kHeaderBytes, MS_SYNC);
    munmap(base, kHeaderBytes);
    close(fd);
    return rc;
}
