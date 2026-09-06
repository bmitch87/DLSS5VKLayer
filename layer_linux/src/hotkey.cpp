#include "hotkey.h"
#include "log.h"

#include <linux/input.h>

#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace dlssnr {
namespace {

struct NamedKey {
    const char* name;
    uint32_t code;
};

// The keys anyone would plausibly bind. Not the whole table: a name that is not here simply does not
// resolve, which is better than accepting something nobody can type.
const NamedKey kKeys[] = {
    { "F1", KEY_F1 },   { "F2", KEY_F2 },   { "F3", KEY_F3 },   { "F4", KEY_F4 },
    { "F5", KEY_F5 },   { "F6", KEY_F6 },   { "F7", KEY_F7 },   { "F8", KEY_F8 },
    { "F9", KEY_F9 },   { "F10", KEY_F10 }, { "F11", KEY_F11 }, { "F12", KEY_F12 },
    { "HOME", KEY_HOME }, { "END", KEY_END }, { "INSERT", KEY_INSERT }, { "DELETE", KEY_DELETE },
    { "PAGEUP", KEY_PAGEUP }, { "PAGEDOWN", KEY_PAGEDOWN }, { "PAUSE", KEY_PAUSE },
    { "SCROLLLOCK", KEY_SCROLLLOCK }, { "SYSRQ", KEY_SYSRQ }, { "GRAVE", KEY_GRAVE },
    { "A", KEY_A }, { "B", KEY_B }, { "C", KEY_C }, { "D", KEY_D }, { "E", KEY_E }, { "F", KEY_F },
    { "G", KEY_G }, { "H", KEY_H }, { "I", KEY_I }, { "J", KEY_J }, { "K", KEY_K }, { "L", KEY_L },
    { "M", KEY_M }, { "N", KEY_N }, { "O", KEY_O }, { "P", KEY_P }, { "Q", KEY_Q }, { "R", KEY_R },
    { "S", KEY_S }, { "T", KEY_T }, { "U", KEY_U }, { "V", KEY_V }, { "W", KEY_W }, { "X", KEY_X },
    { "Y", KEY_Y }, { "Z", KEY_Z },
    { "0", KEY_0 }, { "1", KEY_1 }, { "2", KEY_2 }, { "3", KEY_3 }, { "4", KEY_4 },
    { "5", KEY_5 }, { "6", KEY_6 }, { "7", KEY_7 }, { "8", KEY_8 }, { "9", KEY_9 },
};

bool TestBit(const unsigned long* bits, int bit) {
    return (bits[bit / (8 * sizeof(long))] >> (bit % (8 * sizeof(long)))) & 1UL;
}

// A keyboard, as opposed to a mouse, a lid switch, a power button or a gamepad. Asking for letters is
// enough: nothing else on a normal system claims to produce KEY_A through KEY_Z.
bool LooksLikeAKeyboard(int fd) {
    unsigned long evbits[(EV_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {};
    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0) return false;
    if (!TestBit(evbits, EV_KEY)) return false;

    unsigned long keybits[(KEY_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0) return false;

    for (int k = KEY_A; k <= KEY_Z; ++k) {
        if (!TestBit(keybits, k)) return false;
    }
    return true;
}

}  // namespace

uint32_t KeyCodeFromName(const std::string& name) {
    if (name.empty()) return 0;

    // A bare number is a Linux key code, so a key with no name here is still reachable.
    if (std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isdigit(c); }))
        return uint32_t(std::stoul(name));

    std::string upper;
    upper.reserve(name.size());
    for (char c : name) upper.push_back(char(std::toupper((unsigned char) c)));
    if (upper.rfind("KEY_", 0) == 0) upper.erase(0, 4);

    for (const auto& k : kKeys) {
        if (upper == k.name) return k.code;
    }
    return 0;
}

const char* KeyNameFromCode(uint32_t code) {
    for (const auto& k : kKeys) {
        if (k.code == code) return k.name;
    }
    return "?";
}

Hotkeys::~Hotkeys() {
    for (int fd : _fds) close(fd);
    if (_xi) dlclose(_xi);
    if (_x11) dlclose(_x11);
}

void Hotkeys::Open() {
    if (_opened) return;
    _opened = true;

    // Forced backends exist for testing: the fallback is otherwise unreachable on a machine where
    // evdev works, which is the machine it is hardest to get wrong on.
    const char* forced = getenv("DLSSNR_HOTKEY_BACKEND");
    const bool wantEvdev = !forced || std::strcmp(forced, "evdev") == 0;
    const bool wantX11 = !forced || std::strcmp(forced, "x11") == 0;

    if (wantEvdev && OpenEvdev()) {
        _backend = "evdev";
        Log("[hotkey] watching %zu keyboard(s) through evdev", _fds.size());
        return;
    }
    if (wantX11 && OpenX11()) {
        _backend = "x11";
        Log("[hotkey] watching XInput2 raw keys on %s", getenv("DISPLAY"));
        return;
    }

    Log("[hotkey] no way to read the keyboard here. /dev/input needs the 'input' group or a uaccess "
        "ACL; without it only an X11 session can be read, and never one inside gamescope.");
}

bool Hotkeys::OpenEvdev() {
    RescanEvdev();
    return !_fds.empty();
}

// Opens any keyboard not already open, and drops any that has gone away. Cheap: a readdir of
// /dev/input and an ioctl per new device, a few times a second at most.
void Hotkeys::RescanEvdev() {
    // Devices that have gone away, asked with an ioctl rather than a read.
    //
    // A read looked like the obvious test -- a removed device answers ENODEV -- but it answers with a
    // queued event first if there is one, so a dead device with unread input looked alive and its path
    // stayed in the known set. The next device to take that path was then never opened, and the key
    // silently stopped working. EVIOCGVERSION asks the same question without consuming anything.
    for (size_t i = 0; i < _fds.size();) {
        int version = 0;
        if (ioctl(_fds[i], EVIOCGVERSION, &version) < 0) {
            close(_fds[i]);
            _fds.erase(_fds.begin() + long(i));
            _known.erase(_knownOrder[i]);
            _knownOrder.erase(_knownOrder.begin() + long(i));
            continue;
        }
        ++i;
    }

    DIR* dir = opendir("/dev/input");
    if (!dir) return;

    while (dirent* e = readdir(dir)) {
        if (std::strncmp(e->d_name, "event", 5) != 0) continue;
        const std::string name = e->d_name;
        if (_known.count(name)) continue;

        const std::string path = "/dev/input/" + name;
        const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        if (!LooksLikeAKeyboard(fd)) {
            close(fd);
            continue;
        }
        _fds.push_back(fd);
        _known.insert(name);
        _knownOrder.push_back(name);
        if (_announced) Log("[hotkey] picked up a keyboard that appeared later: %s", path.c_str());
    }
    closedir(dir);
    _announced = true;
}

bool Hotkeys::OpenX11() {
    if (!getenv("DISPLAY")) return false;

    // Loaded rather than linked, so a layer on a machine with no X at all still starts.
    _x11 = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
    _xi = dlopen("libXi.so.6", RTLD_LAZY | RTLD_LOCAL);
    if (!_x11 || !_xi) return false;

    _xOpenDisplay = (void* (*) (const char*)) dlsym(_x11, "XOpenDisplay");
    _xQueryExtension = (int (*) (void*, const char*, int*, int*, int*)) dlsym(_x11, "XQueryExtension");
    _xPending = (int (*) (void*)) dlsym(_x11, "XPending");
    _xNextEvent = (int (*) (void*, void*)) dlsym(_x11, "XNextEvent");
    _xGetEventData = (int (*) (void*, void*)) dlsym(_x11, "XGetEventData");
    _xFreeEventData = (void (*) (void*, void*)) dlsym(_x11, "XFreeEventData");
    _xFlush = (int (*) (void*)) dlsym(_x11, "XFlush");
    _xiQueryVersion = (int (*) (void*, int*, int*)) dlsym(_xi, "XIQueryVersion");
    _xiSelectEvents = (int (*) (void*, unsigned long, void*, int)) dlsym(_xi, "XISelectEvents");

    if (!_xOpenDisplay || !_xQueryExtension || !_xPending || !_xNextEvent || !_xGetEventData ||
        !_xFreeEventData || !_xFlush || !_xiQueryVersion || !_xiSelectEvents)
        return false;

    _display = _xOpenDisplay(nullptr);
    if (!_display) return false;

    int event = 0, error = 0;
    if (!_xQueryExtension(_display, "XInputExtension", &_xiOpcode, &event, &error)) return false;

    int major = 2, minor = 2;
    if (_xiQueryVersion(_display, &major, &minor) != 0 /* Success */) return false;

    // Raw key presses on the root window. Raw events are delivered whatever has focus, which is the
    // whole point: the layer has no window and the game may not even be an X client.
    unsigned char mask[4] = {};
    mask[XI_RawKeyPress / 8] |= (1u << (XI_RawKeyPress % 8));
    XIEventMask em{};
    em.deviceid = 1;  // XIAllMasterDevices
    em.mask_len = sizeof(mask);
    em.mask = mask;

    Display* dpy = (Display*) _display;
    _xiSelectEvents(_display, (unsigned long) DefaultRootWindow(dpy), &em, 1);
    _xFlush(_display);
    return true;
}

// Drains everything the keyboards have to say and remembers which keys went down. Draining rather
// than looking for one code matters: the queue is per device and a read that leaves events behind
// makes the next frame's answer stale, and eventually the buffer overruns.
bool Hotkeys::PressedEvdev(uint32_t keyCode) {
    // Keyboards appear after the game starts: someone plugs one in, a wireless one wakes, or a
    // controller's keyboard half enumerates late. Scanning once at startup meant those were invisible
    // for the life of the process, which is a very confusing way for a key to not work.
    // Timed rather than counted in frames: a frame count means a different rescan interval at 30 fps
    // than at 300, and the thing being waited for is a person plugging something in.
    const auto now = std::chrono::steady_clock::now();
    if (now - _lastScan >= std::chrono::seconds(1)) {
        _lastScan = now;
        RescanEvdev();
    }

    input_event events[64];
    for (int fd : _fds) {
        for (;;) {
            const ssize_t n = read(fd, events, sizeof(events));
            if (n <= 0) break;
            const size_t count = size_t(n) / sizeof(input_event);
            for (size_t i = 0; i < count; ++i) {
                if (events[i].type == EV_KEY && events[i].value == 1)
                    _pending.push_back(events[i].code);
            }
            if (n < ssize_t(sizeof(events))) break;
        }
    }

    const auto it = std::find(_pending.begin(), _pending.end(), keyCode);
    if (it == _pending.end()) {
        // Nothing this caller wanted; keep the list short rather than growing it forever.
        if (_pending.size() > 256) _pending.clear();
        return false;
    }
    _pending.erase(it);
    return true;
}

// Raw key presses are already edges, so there is no held state to track. The X keycode for an evdev
// code is that code plus eight on every X server driven by evdev, which is all of them on Linux.
bool Hotkeys::PressedX11(uint32_t keyCode) {
    if (!_display) return false;

    while (_xPending(_display) > 0) {
        XEvent event;
        _xNextEvent(_display, &event);
        XGenericEventCookie* cookie = &event.xcookie;
        if (cookie->type != GenericEvent || cookie->extension != _xiOpcode) continue;
        if (!_xGetEventData(_display, cookie)) continue;
        if (cookie->evtype == XI_RawKeyPress) {
            const XIRawEvent* raw = (const XIRawEvent*) cookie->data;
            if (raw->detail >= 8) _pending.push_back(uint32_t(raw->detail) - 8);
        }
        _xFreeEventData(_display, cookie);
    }

    const auto it = std::find(_pending.begin(), _pending.end(), keyCode);
    if (it == _pending.end()) {
        if (_pending.size() > 256) _pending.clear();
        return false;
    }
    _pending.erase(it);
    return true;
}

bool Hotkeys::Pressed(uint32_t keyCode) {
    if (keyCode == 0) return false;
    Open();
    if (!_fds.empty()) return PressedEvdev(keyCode);
    if (_display) return PressedX11(keyCode);
    return false;
}

}  // namespace dlssnr
