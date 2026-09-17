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
    { "hdrmode", &ShmHeader::hdrMode, false, "0 auto, 1 off, 2 force float16 proxy" },
    { "sdr16multipass", &ShmHeader::sdr16Multipass, false,
      "0/1 use 16-bit images between SDR model passes" },
    { "passes", &ShmHeader::passes, false, "how many times the model runs over one frame" },
    { "preset", &ShmHeader::preset, false,
      "model preset 0-15; no value was measured to change the picture on this model" },
    { "style", &ShmHeader::style, false,
      "0 default, 1 natural, 2 cinematic; anything above 2 behaves as 2" },
    { "automask", &ShmHeader::autoMask, false, "0/1 automatic skin mask" },
    { "scenecut", &ShmHeader::sceneCutThreshold, false,
      "scene-cut threshold 0-255 (mean luma difference); 0 turns the detector off" },
    { "uicorrection", &ShmHeader::uiCorrection, false,
      "0/1 tell the model the frame already has the game's UI drawn on it" },
    { "intensity", &ShmHeader::intensityBits, true, "model intensity" },
    { "localtone", &ShmHeader::localToneBits, true, "local tone strength" },
    { "localstructure", &ShmHeader::localStructureBits, true, "local structure strength" },
    { "skinstructure", &ShmHeader::skinStructureBits, true,
      "-1 follows local structure; inert unless automask is on" },
    { "sharpness", &ShmHeader::sharpnessBits, true, "sharpness" },
    { "detail", &ShmHeader::transferStrengthBits, true, "how much of the edit lands, 0-4" },
    { "colour", &ShmHeader::colourStrengthBits, true, "how much of its colour comes with it, 0-4" },
    { "guard", &ShmHeader::maxRatioBits, true,
      "highlight guard, the most a pixel may move; the GUI starts at 1.1 and 1.0 pins it to identity" },
    { "transfer", &ShmHeader::transfer, false, "0 classic, 1 matched residual, 2 native + edit" },
    { "bypass", &ShmHeader::compositionBypass, false,
      "present the model's raw answer instead of composing its edit, 0 or 1" },
    { "rebuildms", &ShmHeader::rebuildSettleMs, false,
      "ms to settle before rebuilding a pass after a model setting changes" },
    { "ratiosmooth", &ShmHeader::ratioSmoothPercent, false,
      "how much of the relighting ratio comes from the neighbourhood, 0-100 (100 default, 0 = per-pixel)" },
    { "colourtrust", &ShmHeader::colourTrustPercent, false,
      "how far the model may move a pixel's colour from the frame's, in hundredths (200 default, 0 = frame's hue)" },
    { "mvec", &ShmHeader::mvecEnabled, false, "estimate motion vectors from the frames, 0 or 1" },
    { "mvecquality", &ShmHeader::mvecQuality, false, "0 fast, 1 balanced, 2 quality" },
    { "mvecunits", &ShmHeader::mvecScaleMode, false,
      "what the field holds: 0 normalised, 1 pixels, 2 uv 0..1. Ours always holds pixels" },
    { "mvecpixels", &ShmHeader::mvecPixelSize, false, "0 1px, 1 2px, 2 4px, 3 8px optical-flow grid" },
    { "debugview", &ShmHeader::debugView, false, "0 off, 1 proxy, 2 model, 3 amplified edit" },
    { "debugscale", &ShmHeader::debugScaleBits, true, "what the debug views are multiplied by" },
    { "whitepoint", &ShmHeader::whitePointBits, true, "paper white" },
    { "whitepointscale", &ShmHeader::whitePointScaleBits, true, "multiplier on the white point" },
    { "whitepointsource", &ShmHeader::whitePointSource, false, "0 the slider, 1 measured off the frame" },
    { "whitepointtrim", &ShmHeader::whitePointTrimBits, true, "multiplier on a measured white point" },
    { "workingscale", &ShmHeader::workingScaleBits, true,
      "the fraction of the frame the model works at; above 1 there are no extra samples to find" },
    { "downscaler", &ShmHeader::scalingDownscaler, false, "1 bicubic, 2 catmull, 3 lanczos2, 4 lanczos3, 5 kaiser2, 6 kaiser3, 7 magic" },
    { "compare", &ShmHeader::compareMode, false, "0 off, 1 side by side, 2 wipe" },
    { "comparesplit", &ShmHeader::compareSplitBits, true, "where the split sits, 0-1" },
    { "comparezoom", &ShmHeader::compareZoomBits, true, "side by side only, 1-2" },
    { "compareswap", &ShmHeader::compareSwap, false, "0/1 which side the edited frame is on" },
    { "colourmode", &ShmHeader::colourMode, false, "0 auto, 1 display-referred, 2 linear HDR" },
    { "reversible", &ShmHeader::reversibleMode, false, "0 knee, 1 neutwo, 2 replace, 3 hybrid, 4 hybrid+replace" },
    { "applymodel", &ShmHeader::applyModel, false, "0 show the clean frame, 1 apply the edit" },
    { "hold", &ShmHeader::holdFrame, false,
      "0 running, 1 hold, 2 hold the next frame that completes a round trip" },
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

// Weaker than Initialised on purpose, for the two commands that must keep working across a
// version mismatch: quit and resume.
//
// The strict check turns a cosmetic mismatch into "you cannot ask the helper to stop". That
// is not hypothetical -- it was hit here the moment the protocol went to 21 with a version
// 20 tool still installed: dlssnr-helper stop set no flag, said nothing, and escalated
// straight to SIGTERM, so the helper never ran its own teardown and there was no way to
// tell from the outside.
//
// Safe because of what the magic means. kShmMagic was deliberately bumped when the v1
// layout was abandoned, precisely so a stale mapping would be re-created rather than
// half-read -- so a magic match implies the v2-and-later prefix, in which magic, version,
// the sequence numbers, width, height and quit have not moved. Those are the only fields
// these two commands touch.
bool MagicOk(const ShmHeader* h) { return h->magic.load() == kShmMagic; }

void WarnVersion(const ShmHeader* h, const char* what) {
    const uint32_t v = h->version.load();
    if (v == kShmVersion) return;
    std::fprintf(stderr,
                 "warning: header is version %u and this tool is version %u; doing '%s' anyway "
                 "because the fields it touches have not moved. Reinstall the layer, helper, "
                 "GUI and tools together.\n",
                 v, kShmVersion, what);
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
        // A guard of exactly 1 pins the composition's luminance path to identity: the
        // clamp becomes [1,1] and the model's luminance verdict is divided straight back
        // out, so half the pass stops. That is a legitimate diagnostic and stays reachable
        // from here -- the GUI's slider starts at 1.1 -- but it should never be reached by
        // accident and thought to be a setting.
        if (std::strcmp(name, "guard") == 0 && v <= 1.0) {
            std::fprintf(stderr,
                         "note: guard=%g pins the luminance path to identity; the model's "
                         "luminance change is fully undone. Colour still moves.\n", v);
        }
        (h->*s.field).store(s.isFloat ? FloatToBits(float(v)) : uint32_t(v < 0 ? 0 : v));
        h->controlSeq.fetch_add(1);
        // What actually costs a rebuild, which turned out to be far less than this list used to say.
        //
        // Measured: with the rebuild debounce pushed out so that one feature served every value,
        // style, intensity, local tone, local structure, skin structure and the auto mask all moved
        // the picture on their own, reproducibly, with zero rebuilds -- see
        // NgxTuning::SameCreateParams for the numbers. Only the preset is read at create, and it
        // was separately measured to change nothing at all. `passes` stays because adding or
        // dropping a pass is a build, not a tuning.
        static const char* kCreateTime[] = { "preset", "passes" };
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
    std::printf("helper_features=%u\nhelper_pass_ceiling=%u\nhelper_vram_mb=%u\n",
                h->helperFeatures.load(), h->helperPassCeiling.load(), h->helperVramMB.load());
    std::printf("helper_eval_ms=%.2f\nhelper_upload_ms=%.2f\nhelper_readback_ms=%.2f\n",
                double(BitsToFloat(h->helperEvalMsBits.load())),
                double(BitsToFloat(h->helperUploadMsBits.load())),
                double(BitsToFloat(h->helperReadbackMsBits.load())));
    const uint64_t roundTrips = ShmLoad64(h->layerFramesLo, h->layerFramesHi);
    const uint64_t presents = ShmLoad64(h->layerPresentsLo, h->layerPresentsHi);
    std::printf("layer_pid=%u\nlayer_composition_up=%u\nlayer_frames=%llu\nlayer_ms=%.2f\n",
                h->layerPid.load(),
                h->layerCompositionUp.load(),
                (unsigned long long) roundTrips,
                double(BitsToFloat(h->layerMsBits.load())));
    // The denominator, printed next to what it divides. layer_frames counts round trips;
    // layer_presents counts what the game handed the display. Every millisecond figure above is
    // per round trip, so the two being equal is the only case in which they are also per present.
    std::printf("layer_presents=%llu\n", (unsigned long long) presents);
    if (presents > 0)
        std::printf("layer_presents_per_round_trip=%.2f\n",
                    roundTrips ? double(presents) / double(roundTrips) : 0.0);
    std::printf("measured_white_point=%g\n", double(BitsToFloat(h->layerMeasuredWhiteBits.load())));
    std::printf("hdr_mode=%u\nhdr_detected=%u\nhdr_active=%u\nproxy_format=%u\nhdr_encode=%u\n",
                h->hdrMode.load(), h->hdrDetected.load(), h->hdrActive.load(),
                h->proxyFormat.load(), h->hdrEncode.load());
    std::printf("frame_repeat=%u\n", h->frameRepeat.load());
    // The threshold next to what it is compared against: a scene-cut threshold reported on its own
    // cannot be judged, which is why it was unusable as an environment variable.
    std::printf("scene_cut_threshold=%u\nscene_cut_score=%u\nscene_cut_count=%u\n",
                h->sceneCutThreshold.load(), h->sceneCutScore.load(), h->sceneCutCount.load());
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
        if (MagicOk(h)) {
            WarnVersion(h, "quit");
            h->quit.store(1);
            h->controlSeq.fetch_add(1);
        }
    } else if (std::strcmp(cmd, "resume") == 0) {
        // Same reasoning as quit: clearing the flag must keep working across a version
        // mismatch. Re-initialise only when the MAGIC is wrong -- a version mismatch alone
        // is not a reason to overwrite a header a newer process is using.
        if (!MagicOk(h)) ShmInitDefaults(h);
        else WarnVersion(h, "resume");
        h->quit.store(0);
        h->controlSeq.fetch_add(1);
    } else if (std::strcmp(cmd, "reset") == 0) {
        // Settings only, leaving the transport and both sides' status alone. Clearing the whole
        // header on a live session takes the layer's published presence with it, and the layer
        // republishes only when it next composes a frame.
        ShmResetSettings(h);
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
