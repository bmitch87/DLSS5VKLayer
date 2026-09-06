#pragma once
// Matched before/after frames, written so questions about this pass get settled by measurement.
//
// Upstream's reasoning, which is worth keeping: every comparison of a detail pass tends to be two
// separate video captures -- different camera path, different exposure, and a codec in between
// throwing away exactly the high-frequency detail the argument is about. What is wanted instead is
// the same frames twice. The pass already holds the frame as the game presented it and the frame
// after the model's edit, so both are written for one run of consecutive frames: a control with
// nothing varying but the thing under test.
//
// Upstream writes raw because a codec is the confound. PNG is lossless, so it is not a confound, and
// a file you can open is worth a great deal more than one you cannot -- so an 8-bit frame is written
// as PNG. A 10-bit or float frame has no PNG that can hold it and is written raw, as upstream does,
// with a manifest saying how to read it. The manifest is written either way.
#include <cstdint>
#include <string>

namespace dlssnr {

class CaptureWriter {
  public:
    // Where captures go: $XDG_STATE_HOME/dlssnr/captures, or ~/.local/state/dlssnr/captures.
    // Emptied when a session's first capture starts, so it holds one session and never grows.
    static std::string Directory();

    void Begin(uint32_t frames);
    bool Active() const { return _remaining > 0; }
    uint32_t Remaining() const { return _remaining; }

    // One frame's pair. `format` is the Vulkan format both surfaces are in; the writer decides
    // between PNG and raw from it, and reports what it chose in the manifest.
    void WriteFrame(const void* before, const void* after, uint32_t width, uint32_t height,
                    uint32_t vkFormat);

  private:
    void WriteManifest(uint32_t width, uint32_t height, uint32_t vkFormat, bool png) const;

    uint32_t _remaining = 0;
    uint32_t _index = 0;
    bool _cleared = false;
};

}  // namespace dlssnr
