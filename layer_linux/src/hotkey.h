#pragma once
// An in-game key, in every environment this layer runs in.
//
// This is harder than it looks, because the layer has no window. It is a shared object inside someone
// else's process, and the three environments it has to work in do not agree on what "the keyboard" is:
//
//   X11 / XWayland   an X server exists, but not every way of asking it still works
//   winewayland      the game is a Wayland client; the layer is not a client at all
//   gamescope        a nested compositor with its own Xwayland inside it
//
// Two backends, measured rather than assumed on an XWayland session with a real key press:
//
//   evdev    reads the kernel's own input devices. Works everywhere -- a Proton game is still a Linux
//            process and gamescope does not sit between a process and /dev/input -- and has the lowest
//            latency. It needs read access to /dev/input/event*, which is root:input. Keyboards
//            deliberately get no uaccess ACL, unlike joysticks, because that would let any program
//            keylog, so this means the 'input' group and most people are not in it.
//
//   XInput2  raw key events selected on the root window. Needs no permission, and raw events arrive
//            regardless of which client has focus -- including when the focused client is a Wayland
//            one and no X client is focused at all. Verified with a real key from a virtual keyboard,
//            not with XTEST, which injects at the server and would have made this pass falsely.
//
// XQueryKeymap, which is what vkBasalt uses, was tried first and does not work here: on an XWayland
// session it reports nothing for a real key press. It is fine on a true Xorg session and that is
// increasingly not what people run.
#include <chrono>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace dlssnr {

// A Linux KEY_* code, or 0 for unbound. Names are the KEY_ names without the prefix, case-insensitive:
// "F10", "Home", "N". Returns 0 for anything unrecognised.
uint32_t KeyCodeFromName(const std::string& name);
const char* KeyNameFromCode(uint32_t code);

class Hotkeys {
  public:
    ~Hotkeys();

    // Opens whichever backend is available. Safe to call repeatedly; only the first does anything.
    void Open();

    // True once, on the frame the key goes down. Cheap enough for the present hook: the evdev backend
    // drains a non-blocking read, and the X11 one is a single round trip against a cached connection.
    bool Pressed(uint32_t keyCode);

    const char* Backend() const { return _backend; }

  private:
    bool OpenEvdev();
    void RescanEvdev();
    bool OpenX11();
    bool PressedEvdev(uint32_t keyCode);
    bool PressedX11(uint32_t keyCode);

    bool _opened = false;
    const char* _backend = "none";

    std::vector<int> _fds;              // evdev keyboards
    std::vector<uint32_t> _pending;     // codes seen down since the last ask

    // Which device files are already open, so a rescan is cheap and does not double-open.
    std::set<std::string> _known;
    std::vector<std::string> _knownOrder;

    // A keyboard plugged in mid-game has to be picked up, but a readdir every present would be silly.
    // Once a second is far below what anyone would notice and does not vary with frame rate.
    std::chrono::steady_clock::time_point _lastScan{};
    bool _announced = false;

    // Resolved by name so the layer does not hard-require X at load time.
    void* _x11 = nullptr;
    void* _xi = nullptr;
    void* _display = nullptr;
    int _xiOpcode = 0;

    void* (*_xOpenDisplay)(const char*) = nullptr;
    int (*_xQueryExtension)(void*, const char*, int*, int*, int*) = nullptr;
    int (*_xPending)(void*) = nullptr;
    int (*_xNextEvent)(void*, void*) = nullptr;
    int (*_xGetEventData)(void*, void*) = nullptr;
    void (*_xFreeEventData)(void*, void*) = nullptr;
    int (*_xFlush)(void*) = nullptr;
    int (*_xiQueryVersion)(void*, int*, int*) = nullptr;
    int (*_xiSelectEvents)(void*, unsigned long, void*, int) = nullptr;
};

}  // namespace dlssnr
