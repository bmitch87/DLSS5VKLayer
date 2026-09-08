#include "runner_discovery.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace dlssnr {

static std::string envOr(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
}

static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

static bool containsCI(const std::string& hay, const std::string& needle) {
    return lower(hay).find(lower(needle)) != std::string::npos;
}

static bool isCustomMarker(const std::string& name) {
    return containsCI(name, "GE") ||
           containsCI(name, "Cachy") ||
           containsCI(name, "Sugar") ||
           containsCI(name, "Tkg") ||
           containsCI(name, "Wine-GE");
}

static bool isValveOfficialName(const std::string& name) {
    if (isCustomMarker(name)) return false;
    if (name == "Proton - Experimental") return true;
    if (name == "Proton Hotfix") return true;
    if (name.rfind("Proton ", 0) == 0 && name.size() > 7 && std::isdigit(static_cast<unsigned char>(name[7]))) return true;
    return false;
}

static int parseVersionScore(const std::string& name) {
    size_t i = 0;
    while (i < name.size() && !std::isdigit(static_cast<unsigned char>(name[i]))) ++i;
    if (i >= name.size()) return 0;

    int major = 0;
    while (i < name.size() && std::isdigit(static_cast<unsigned char>(name[i]))) {
        major = major * 10 + (name[i] - '0');
        ++i;
    }

    int minor = 0;
    if (i < name.size() && name[i] == '.') {
        ++i;
        while (i < name.size() && std::isdigit(static_cast<unsigned char>(name[i]))) {
            minor = minor * 10 + (name[i] - '0');
            ++i;
        }
    }

    return major * 1000 + minor;
}

static int runnerScore(const std::string& name) {
    int base = 7000000;
    if (containsCI(name, "Cachy")) base = 10000000;
    else if (containsCI(name, "Wine-GE")) base = 8500000;
    else if (containsCI(name, "GE")) base = 9000000;
    return base + parseVersionScore(name);
}

static bool isExecutableFile(const fs::path& p) {
    std::error_code ec;
    fs::file_status st = fs::status(p, ec);
    if (ec || !fs::is_regular_file(st)) return false;
    auto perms = st.permissions();
    return (perms & fs::perms::owner_exec) != fs::perms::none ||
           (perms & fs::perms::group_exec) != fs::perms::none ||
           (perms & fs::perms::others_exec) != fs::perms::none;
}

static void scanCompatibilityTools(const fs::path& root, std::vector<RunnerInfo>& out) {
    std::error_code ec;
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) return;

    fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    if (ec) return;

    for (const auto& entry : it) {
        std::error_code e;
        if (!entry.is_directory(e) || e) continue;

        std::string name = entry.path().filename().string();
        if (isValveOfficialName(name)) continue;

        fs::path proton = entry.path() / "proton";
        if (!fs::exists(proton, e) || e) continue;
        if (!isExecutableFile(proton)) continue;

        RunnerInfo r;
        r.id = name;
        r.name = name;
        r.path = proton.string();
        r.source = root.string();
        r.score = runnerScore(name);
        out.push_back(r);
    }
}

std::vector<RunnerInfo> discoverCustomRunners() {
    std::vector<RunnerInfo> runners;

    const std::string home = envOr("HOME", "/");
    const std::string xdgData = envOr("XDG_DATA_HOME", home + "/.local/share");

    scanCompatibilityTools(fs::path(xdgData) / "Steam" / "compatibilitytools.d", runners);
    scanCompatibilityTools(fs::path(home) / ".var/app/com.valvesoftware.Steam/data/Steam/compatibilitytools.d", runners);
    scanCompatibilityTools(fs::path(home) / "snap/steam/common/.local/share/Steam/compatibilitytools.d", runners);

    // System-wide data dirs, scanned after the user dirs so user installs win score ties.
    // The XDG defaults are always included, not just used as a fallback: Steam Runtime scrubs
    // XDG_DATA_DIRS and points it at container paths, so trusting the variable alone would
    // miss /usr/share/steam/compatibilitytools.d (CachyOS) when launched from %command%.
    std::vector<std::string> dataDirs{"/usr/local/share", "/usr/share"};
    if (const char* env = std::getenv("XDG_DATA_DIRS")) {
        std::stringstream ss(env);
        std::string dir;
        while (std::getline(ss, dir, ':')) {
            if (dir.empty() || dir[0] != '/') continue;
            dataDirs.push_back(dir);
        }
    }
    for (const auto& dir : dataDirs) {
        scanCompatibilityTools(fs::path(dir) / "steam" / "compatibilitytools.d", runners);
    }

    std::stable_sort(runners.begin(), runners.end(), [](const RunnerInfo& a, const RunnerInfo& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.name < b.name;
    });

    std::vector<RunnerInfo> unique;
    for (const auto& r : runners) {
        if (unique.empty() || unique.back().path != r.path) unique.push_back(r);
    }
    return unique;
}

std::string runnerDisplayName(const RunnerInfo& runner) {
    return runner.name;
}

}