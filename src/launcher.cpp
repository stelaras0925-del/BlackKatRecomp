// SPDX-License-Identifier: GPL-3.0-only
//
// Built-in launcher (see launcher.h). Shown by `blackkat` with no arguments or with --launcher.

#include "launcher.h"

#include "raylib.h"
#include "rlgl.h"
#include "imgui.h"
#include "rlImGui.h"
#include "imgui_impl_raylib.h"
#include "imgui_internal.h" // input queue (mouse position rescale)

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <sstream>
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// Monitor selection, implemented in the runtime (ps2xRuntime/src/lib/ps2_monitor.inl).
int ps2xMonitorResolve(const char *spec, const char *tag);
void ps2xMonitorPlace(int mon, bool fullscreen, bool testHidden, bool allowResize, const char *tag);
std::string ps2xMonitorListText();

struct GLFWwindow;
extern "C" void glfwGetFramebufferSize(GLFWwindow *window, int *width, int *height);

namespace bklaunch
{
namespace
{
constexpr uint64_t kElfSize = 6439444u;
constexpr const char *kElfMd5 = "e6ea18b09f214dd276078872926ca10d";

// ---- MD5 (RFC 1321) ---------------------------------------------------------------
struct Md5
{
    uint32_t a = 0x67452301, b = 0xefcdab89, c = 0x98badcfe, d = 0x10325476;
    uint64_t len = 0;
    uint8_t buf[64];
    size_t n = 0;
    static uint32_t rol(uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }
    void block(const uint8_t *p)
    {
        static const uint32_t K[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
            0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
            0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
            0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
            0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
            0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
            0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
            0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
        static const int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                  5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                                  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                  6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
        uint32_t M[16];
        for (int i = 0; i < 16; ++i)
            M[i] = uint32_t(p[i * 4]) | (uint32_t(p[i * 4 + 1]) << 8) | (uint32_t(p[i * 4 + 2]) << 16) | (uint32_t(p[i * 4 + 3]) << 24);
        uint32_t A = a, B = b, C = c, D = d;
        for (int i = 0; i < 64; ++i)
        {
            uint32_t F;
            int g;
            if (i < 16) { F = (B & C) | (~B & D); g = i; }
            else if (i < 32) { F = (D & B) | (~D & C); g = (5 * i + 1) & 15; }
            else if (i < 48) { F = B ^ C ^ D; g = (3 * i + 5) & 15; }
            else { F = C ^ (B | ~D); g = (7 * i) & 15; }
            const uint32_t t = D;
            D = C;
            C = B;
            B = B + rol(A + F + K[i] + M[g], S[i]);
            A = t;
        }
        a += A; b += B; c += C; d += D;
    }
    void update(const uint8_t *p, size_t len_)
    {
        len += len_;
        while (len_)
        {
            const size_t k = std::min(len_, 64 - n);
            std::memcpy(buf + n, p, k);
            n += k; p += k; len_ -= k;
            if (n == 64) { block(buf); n = 0; }
        }
    }
    std::string hex()
    {
        const uint64_t bits = len * 8;
        const uint8_t pad = 0x80;
        update(&pad, 1);
        const uint8_t z = 0;
        while (n != 56) update(&z, 1);
        uint8_t l[8];
        for (int i = 0; i < 8; ++i) l[i] = uint8_t(bits >> (8 * i));
        update(l, 8);
        char out[33];
        const uint32_t v[4] = {a, b, c, d};
        for (int i = 0; i < 16; ++i)
            std::snprintf(out + i * 2, 3, "%02x", (v[i / 4] >> (8 * (i % 4))) & 0xFF);
        return std::string(out, 32);
    }
};

// ---- ISO 9660: find SLUS_203.65 in the root directory ----------------------------
bool findRootFile(std::ifstream &f, const char *want, uint32_t &lba, uint32_t &size, bool &isIso)
{
    isIso = false;
    uint8_t pvd[2048];
    f.seekg(16 * 2048);
    if (!f.read(reinterpret_cast<char *>(pvd), sizeof pvd))
        return false;
    if (pvd[0] != 1 || std::memcmp(pvd + 1, "CD001", 5) != 0)
        return false;
    isIso = true;
    const uint8_t *root = pvd + 156;
    const uint32_t rootLba = uint32_t(root[2]) | (uint32_t(root[3]) << 8) | (uint32_t(root[4]) << 16) | (uint32_t(root[5]) << 24);
    const uint32_t rootLen = uint32_t(root[10]) | (uint32_t(root[11]) << 8) | (uint32_t(root[12]) << 16) | (uint32_t(root[13]) << 24);
    std::vector<uint8_t> dir(std::min<uint32_t>(rootLen, 1u << 20));
    f.seekg(uint64_t(rootLba) * 2048u);
    if (!f.read(reinterpret_cast<char *>(dir.data()), dir.size()))
        return false;
    const size_t wl = std::strlen(want);
    for (size_t off = 0; off < dir.size();)
    {
        const uint8_t len = dir[off];
        if (len == 0) { off = (off / 2048 + 1) * 2048; continue; }
        if (off + len > dir.size() || len < 34) break;
        const uint8_t nl = dir[off + 32];
        const char *name = reinterpret_cast<const char *>(&dir[off + 33]);
        if (nl >= wl && strncasecmp(name, want, wl) == 0 && (nl == wl || name[wl] == ';'))
        {
            lba = uint32_t(dir[off + 2]) | (uint32_t(dir[off + 3]) << 8) | (uint32_t(dir[off + 4]) << 16) | (uint32_t(dir[off + 5]) << 24);
            size = uint32_t(dir[off + 10]) | (uint32_t(dir[off + 11]) << 8) | (uint32_t(dir[off + 12]) << 16) | (uint32_t(dir[off + 13]) << 24);
            return true;
        }
        off += len;
    }
    return false;
}

std::string trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool onPath(const char *exe)
{
    const char *p = std::getenv("PATH");
    if (!p) return false;
    std::stringstream ss(p);
    std::string d;
    while (std::getline(ss, d, ':'))
        if (!d.empty() && ::access((fs::path(d) / exe).c_str(), X_OK) == 0)
            return true;
    return false;
}

std::string shellQuote(const std::string &s)
{
    std::string r = "'";
    for (char c : s) { if (c == '\'') r += "'\\''"; else r += c; }
    return r + "'";
}

// Native file dialog through kdialog (KDE) or zenity; empty string when cancelled/unavailable.
std::string fileDialog(const std::string &start, bool isoFilter, const std::string &title)
{
    const char *desk = std::getenv("XDG_CURRENT_DESKTOP");
    const bool kde = desk && std::strstr(desk, "KDE");
    std::string cmd;
    const std::string dir = start.empty() ? std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") : start;
    if ((kde || !onPath("zenity")) && onPath("kdialog"))
        cmd = "kdialog --title " + shellQuote(title) + " --getopenfilename " + shellQuote(dir) + " " +
              shellQuote(isoFilter ? "*.iso *.ISO *.bin *.BIN|Disc images" : "SLUS_203.65|Game ELF");
    else if (onPath("zenity"))
        cmd = "zenity --file-selection --title=" + shellQuote(title) + " --filename=" + shellQuote(dir + "/") +
              (isoFilter ? " --file-filter='Disc images | *.iso *.ISO *.bin *.BIN' --file-filter='All files | *'" : "");
    else
        return {};
    cmd += " 2>/dev/null";
    std::string out;
    if (FILE *p = ::popen(cmd.c_str(), "r"))
    {
        char b[4096];
        while (std::fgets(b, sizeof b, p)) out += b;
        ::pclose(p);
    }
    return trim(out);
}

fs::path exeDir(const char *argv0)
{
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    if (ec && argv0) p = fs::absolute(argv0, ec);
    return p.parent_path();
}

fs::path repoRootGuess(const char *argv0)
{
    // build/linux-release/bin/blackkat -> repository root
    fs::path d = exeDir(argv0);
    for (int i = 0; i < 4 && !d.empty(); ++i, d = d.parent_path())
        if (fs::exists(d / "tools" / "extract_iso.sh"))
            return d;
    return {};
}

void copyStr(char *dst, size_t n, const std::string &s)
{
    std::snprintf(dst, n, "%s", s.c_str());
}
} // namespace

std::string configPath()
{
    fs::path base;
    if (const char *x = std::getenv("XDG_CONFIG_HOME"); x && x[0] == '/')
        base = x;
    else if (const char *h = std::getenv("HOME"); h && h[0])
        base = fs::path(h) / ".config";
    else
        base = ".";
    return (base / "blackkat" / "config.ini").string();
}

static void parseSettings(std::istream &f, Settings &s);
// vk20 remaster options (kept here, not in Settings, to leave launcher.h untouched)
static int g_textFilter = 2; // 0 off (pixel), 1 smooth, 2 sharp -> PS2X_VK_TEXT_FILTER (Vulkan, scale > 1)
static int g_ripple = 0;     // 0 original, 1 soft, 2 calm -> PS2X_VK_RIPPLE 1 / 0.4 / 0
static int g_menuFont = 1;   // vk21: 0 original bitmap font, 1 IM FELL vector font -> PS2X_VK_FONT off / imfell

bool loadSettings(Settings &s)
{
    std::ifstream f(configPath());
    if (!f)
        return false;
    parseSettings(f, s);
    return true;
}

static void parseSettings(std::istream &f, Settings &s)
{
    std::string line;
    while (std::getline(f, line))
    {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[')
            continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        auto b = [&]() { return v == "1" || v == "true" || v == "yes" || v == "on"; };
        if (k == "iso") s.iso = v;
        else if (k == "elf") s.elf = v;
        else if (k == "renderer") { s.gpu = (v != "cpu"); s.vulkan = (v == "vulkan"); }
        else if (k == "scale") s.scale = std::clamp(std::atoi(v.c_str()), 1, 4);
        else if (k == "antialiasing") s.aa = (v == "fxaa");
        else if (k == "text_filter") g_textFilter = (v == "off") ? 0 : (v == "smooth") ? 1 : 2;
        else if (k == "water_ripple") g_ripple = (v == "calm") ? 2 : (v == "soft") ? 1 : 0;
        else if (k == "menu_font") g_menuFont = (v == "original") ? 0 : 1;   // imfell (also old original_hd / vector)
        else if (k == "audio") s.audio = b();
        else if (k == "movies") s.movies = b();
        else if (k == "fullscreen") s.fullscreen = b();
        else if (k == "monitor") s.monitor = v;
        else if (k == "aspect") s.widescreen = (v == "16:9");
        else if (k == "window") { int w = 0, h = 0; if (std::sscanf(v.c_str(), "%dx%d", &w, &h) == 2 && w >= 320 && h >= 200) { s.winW = w; s.winH = h; } }
        else if (k == "button_prompts") s.buttons = (v == "xbox") ? 1 : (v == "auto") ? 2 : 0;
        else if (k == "rumble") s.rumble = b();
        else if (k == "deadzone") s.deadzone = std::clamp(static_cast<float>(std::atof(v.c_str())), 0.0f, 0.9f);
        else if (k == "iso_md5_key") s.md5Key = v;
        else if (k == "iso_md5_ok") s.md5Ok = std::atoi(v.c_str());
    }
}

static std::string serializeSettings(const Settings &s)
{
    std::ostringstream f;
    f << "iso=" << s.iso << "\n"
      << "elf=" << s.elf << "\n"
      << "renderer=" << (!s.gpu ? "cpu" : s.vulkan ? "vulkan" : "gpu") << "\n"
      << "scale=" << s.scale << "\n"
      << "antialiasing=" << (s.aa ? "fxaa" : "off") << "\n"
      << "text_filter=" << (g_textFilter == 0 ? "off" : g_textFilter == 1 ? "smooth" : "sharp") << "\n"
      << "water_ripple=" << (g_ripple == 2 ? "calm" : g_ripple == 1 ? "soft" : "original") << "\n"
      << "menu_font=" << (g_menuFont ? "imfell" : "original") << "\n"
      << "audio=" << (s.audio ? 1 : 0) << "\n"
      << "movies=" << (s.movies ? 1 : 0) << "\n"
      << "fullscreen=" << (s.fullscreen ? 1 : 0) << "\n"
      << "window=" << s.winW << "x" << s.winH << "\n"
      << "monitor=" << s.monitor << "\n"
      << "aspect=" << (s.widescreen ? "16:9" : "4:3") << "\n"
      << "button_prompts=" << (s.buttons == 1 ? "xbox" : s.buttons == 2 ? "auto" : "ps2") << "\n"
      << "rumble=" << (s.rumble ? 1 : 0) << "\n"
      << "deadzone=" << s.deadzone << "\n"
      << "iso_md5_key=" << s.md5Key << "\n"
      << "iso_md5_ok=" << s.md5Ok << "\n";
    return f.str();
}

bool saveSettings(const Settings &s)
{
    const fs::path p = configPath();
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream f(tmp);
        if (!f)
            return false;
        f << "# BlackKatRecomp launcher settings. Environment variables and command-line options override these.\n"
          << "[launcher]\n"
          << serializeSettings(s);
        if (!f)
            return false;
    }
    fs::rename(tmp, p, ec);
    return !ec;
}

IsoCheck checkIso(const std::string &path, Settings *cache)
{
    IsoCheck r;
    std::error_code ec;
    if (path.empty()) { r.message = "Choose your Pirates: The Legend of Black Kat (NTSC-U) disc image."; return r; }
    if (!fs::is_regular_file(path, ec)) { r.message = "File not found."; return r; }
    r.exists = true;
    std::ifstream f(path, std::ios::binary);
    uint32_t lba = 0, size = 0;
    bool iso = false;
    r.hasElf = findRootFile(f, "SLUS_203.65", lba, size, iso);
    r.iso9660 = iso;
    if (!iso) { r.message = "Not an ISO 9660 disc image."; return r; }
    if (!r.hasElf) { r.message = "SLUS_203.65 not found: not the NTSC-U disc of Black Kat."; return r; }
    r.sizeOk = size == kElfSize;
    if (!r.sizeOk) { r.message = "SLUS_203.65 has the wrong size (" + std::to_string(size) + " bytes): different disc or version."; return r; }
    // md5 of the ELF, cached by path|size|mtime
    struct stat st{};
    ::stat(path.c_str(), &st);
    const std::string key = path + "|" + std::to_string((long long)st.st_size) + "|" + std::to_string((long long)st.st_mtime);
    if (cache && cache->md5Key == key && cache->md5Ok != 0)
        r.md5 = cache->md5Ok;
    else
    {
        std::vector<uint8_t> elf(size);
        f.clear();
        f.seekg(uint64_t(lba) * 2048u);
        if (f.read(reinterpret_cast<char *>(elf.data()), elf.size()))
        {
            Md5 m;
            m.update(elf.data(), elf.size());
            r.md5 = (m.hex() == kElfMd5) ? 1 : -1;
            if (cache) { cache->md5Key = key; cache->md5Ok = r.md5; }
        }
    }
    r.message = r.md5 > 0 ? "OK: NTSC-U v1.00 (SLUS_203.65 md5 verified)."
                          : (r.md5 < 0 ? "SLUS_203.65 md5 mismatch: not the NTSC-U v1.00 disc this port targets." : "OK (md5 not checked).");
    return r;
}

std::string defaultElf(const char *argv0)
{
    std::error_code ec;
    if (fs::is_regular_file("game/SLUS_203.65", ec))
        return fs::absolute("game/SLUS_203.65", ec).string();
    const fs::path root = repoRootGuess(argv0);
    if (!root.empty() && fs::is_regular_file(root / "game" / "SLUS_203.65", ec))
        return (root / "game" / "SLUS_203.65").string();
    return {};
}

void applyToEnv(const Settings &s)
{
    auto set = [](const char *k, const std::string &v) { ::setenv(k, v.c_str(), 0); };
    if (!s.iso.empty()) set("BLACKKAT_ISO", s.iso);
    set("PS2X_GS_GPU", s.gpu ? "1" : "0");
    set("PS2X_GS_BACKEND", !s.gpu ? "cpu" : s.vulkan ? "vulkan" : "gl");
    set("PS2X_GS_SCALE", std::to_string(s.gpu && !s.vulkan ? std::clamp(s.scale, 1, 4) : 1));
    // Vulkan renders at its own scale (PS2X_VK_SCALE); its OpenGL reference stays at 1x
    set("PS2X_VK_SCALE", std::to_string(s.gpu && s.vulkan ? std::clamp(s.scale, 1, 4) : 1));
    set("PS2X_VK_AA", s.gpu && s.vulkan && s.aa ? "fxaa" : "off");
    set("PS2X_VK_TEXT_FILTER", g_textFilter == 0 ? "off" : g_textFilter == 1 ? "smooth" : "sharp");
    set("PS2X_VK_RIPPLE", g_ripple == 2 ? "0" : g_ripple == 1 ? "0.4" : "1");
    set("PS2X_VK_FONT", g_menuFont ? "imfell" : "off");
    if (!s.audio) set("PS2X_HOST_AUDIO", "0");
    set("BLACKKAT_PLAY_FMV", s.movies ? "1" : "0");
    set("PS2X_FULLSCREEN", s.fullscreen ? "1" : "0");
    // 16:9: the picture is presented at 16:9, so the window gets a 16:9 width for the chosen height.
    const int winW = s.widescreen ? ((s.winH * 16 / 9) + 1) / 2 * 2 : s.winW;
    set("PS2X_WINDOW", std::to_string(winW) + "x" + std::to_string(s.winH));
    set("BLACKKAT_WIDESCREEN", s.widescreen ? "1" : "0");
    if (!s.monitor.empty()) set("PS2X_MONITOR", s.monitor);
    set("BLACKKAT_BUTTON_STYLE", s.buttons == 1 ? "xbox" : s.buttons == 2 ? "auto" : "ps2");
    set("PS2X_PAD_RUMBLE", s.rumble ? "1" : "0");
    char dz[16];
    std::snprintf(dz, sizeof dz, "%.2f", std::clamp(s.deadzone, 0.0f, 0.9f));
    set("PS2X_PAD_DEADZONE", dz);
}

static bool runLauncherUi(Settings &s, const char *argv0);

// The launcher window runs in a forked child process. raylib keeps its window config flags
// (FLAG_WINDOW_HIGHDPI, needed for the launcher at fractional desktop scaling) and other
// global state across CloseWindow(); with the launcher in-process the game window inherited
// HIGHDPI and was offset/cropped at 125% scaling. The child sends the chosen settings back
// through a pipe. BLACKKAT_LAUNCHER_INPROC=1 restores the in-process launcher.
bool runLauncher(Settings &s, const char *argv0)
{
    const char *inproc = std::getenv("BLACKKAT_LAUNCHER_INPROC");
    int fds[2];
    if ((inproc && inproc[0] == '1') || pipe(fds) != 0)
        return runLauncherUi(s, argv0);
    std::fflush(nullptr);
    const pid_t pid = fork();
    if (pid < 0)
    {
        close(fds[0]);
        close(fds[1]);
        return runLauncherUi(s, argv0);
    }
    if (pid == 0)
    {
        close(fds[0]);
        const bool go = runLauncherUi(s, argv0);
        if (go)
        {
            const std::string out = serializeSettings(s);
            size_t off = 0;
            while (off < out.size())
            {
                const ssize_t n = write(fds[1], out.data() + off, out.size() - off);
                if (n <= 0)
                    break;
                off += static_cast<size_t>(n);
            }
        }
        close(fds[1]);
        std::fflush(nullptr);
        _exit(go ? 0 : 1);
    }
    close(fds[1]);
    std::string in;
    char buf[4096];
    for (;;)
    {
        const ssize_t n = read(fds[0], buf, sizeof(buf));
        if (n > 0)
            in.append(buf, static_cast<size_t>(n));
        else if (n == 0 || errno != EINTR)
            break;
    }
    close(fds[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || in.empty())
        return false;
    std::istringstream is(in);
    parseSettings(is, s);
    return true;
}

// ---- launcher UI (pirate theme) ------------------------------------------------------------
namespace
{
// System font via fontconfig (fc-match). Returns "" when not found or not a TrueType/OpenType file.
std::string fcMatch(const char *pattern)
{
    if (!onPath("fc-match"))
        return {};
    const std::string cmd = std::string("fc-match -f '%{file}' ") + shellQuote(pattern) + " 2>/dev/null";
    std::string out;
    if (FILE *p = popen(cmd.c_str(), "r"))
    {
        char buf[1024];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, p)) > 0)
            out.append(buf, n);
        pclose(p);
    }
    out = trim(out);
    std::error_code ec;
    if (out.empty() || !fs::is_regular_file(out, ec))
        return {};
    std::string ext = fs::path(out).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return (ext == ".ttf" || ext == ".otf") ? out : std::string();
}

struct UiFonts
{
    ImFont *ui = nullptr;      // body text (sans)
    ImFont *title = nullptr;   // title / big button (serif bold)
    ImFont *section = nullptr; // section headers (serif bold)
};

const ImVec4 kGold(0.95f, 0.78f, 0.38f, 1.0f);
const ImVec4 kGoldHi(1.00f, 0.88f, 0.55f, 1.0f);
const ImVec4 kGoldDim(0.70f, 0.55f, 0.27f, 1.0f);
const ImVec4 kParch(0.93f, 0.87f, 0.74f, 1.0f);
const ImVec4 kParchDim(0.72f, 0.66f, 0.55f, 1.0f);
const ImVec4 kOk(0.50f, 0.86f, 0.50f, 1.0f);
const ImVec4 kBad(0.98f, 0.50f, 0.42f, 1.0f);

inline ImU32 u32(const ImVec4 &c, float a = -1.0f) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, a < 0 ? c.w : a)); }

void applyPirateStyle()
{
    ImGuiStyle &st = ImGui::GetStyle();
    st.WindowRounding = 14.0f;
    st.WindowBorderSize = 2.0f;
    st.WindowPadding = ImVec2(26, 20);
    st.FrameRounding = 7.0f;
    st.FramePadding = ImVec2(10, 6);
    st.ItemSpacing = ImVec2(10, 9);
    st.ItemInnerSpacing = ImVec2(8, 6);
    st.GrabRounding = 7.0f;
    st.GrabMinSize = 14.0f;
    st.FrameBorderSize = 1.0f;
    st.PopupRounding = 8.0f;
    ImVec4 *c = st.Colors;
    c[ImGuiCol_Text] = kParch;
    c[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.50f, 0.42f, 1.0f);
    c[ImGuiCol_WindowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f); // the panel is drawn by hand (wood + trim)
    c[ImGuiCol_PopupBg] = ImVec4(0.14f, 0.09f, 0.05f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(kGoldDim.x, kGoldDim.y, kGoldDim.z, 0.55f);
    c[ImGuiCol_FrameBg] = ImVec4(0.09f, 0.06f, 0.04f, 0.85f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.13f, 0.07f, 0.95f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.27f, 0.17f, 0.08f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.27f, 0.17f, 0.09f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.45f, 0.29f, 0.12f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.60f, 0.42f, 0.16f, 1.0f);
    c[ImGuiCol_CheckMark] = kGoldHi;
    c[ImGuiCol_SliderGrab] = kGold;
    c[ImGuiCol_SliderGrabActive] = kGoldHi;
    c[ImGuiCol_Header] = ImVec4(0.45f, 0.29f, 0.12f, 1.0f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.55f, 0.37f, 0.15f, 1.0f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.65f, 0.46f, 0.18f, 1.0f);
    c[ImGuiCol_Separator] = ImVec4(kGoldDim.x, kGoldDim.y, kGoldDim.z, 0.5f);
    c[ImGuiCol_NavCursor] = kGoldHi;
    c[ImGuiCol_TextSelectedBg] = ImVec4(0.65f, 0.46f, 0.18f, 0.6f);
}

// Night sea: navy -> teal gradient, moon glow, twinkling stars, drifting wave layers with a moving
// moon reflection. Plain raylib drawing, logical coordinates.
void drawSea(float t, int w, int h)
{
    DrawRectangleGradientV(0, 0, w, h, Color{6, 14, 36, 255}, Color{8, 72, 92, 255});
    const float mx = w * 0.86f, my = h * 0.075f;
    DrawCircleGradient(static_cast<int>(mx), static_cast<int>(my), h * 0.22f, Color{255, 236, 190, 46}, Color{255, 236, 190, 0});
    DrawCircle(static_cast<int>(mx), static_cast<int>(my), h * 0.035f, Color{255, 246, 222, 235});
    for (int i = 0; i < 70; ++i)
    {
        const uint32_t r = 2654435761u * static_cast<uint32_t>(i + 1);
        const float sx = static_cast<float>(r % 997u) / 997.0f * w;
        const float sy = static_cast<float>((r >> 10) % 991u) / 991.0f * h * 0.5f;
        const float tw = 0.55f + 0.45f * std::sin(t * (0.8f + (i % 7) * 0.3f) + i);
        DrawCircleV(Vector2{sx, sy}, (i % 9 == 0) ? 1.6f : 1.0f, Color{230, 240, 255, static_cast<unsigned char>(120 * tw)});
    }
    // moon path on the water
    for (int i = 0; i < 26; ++i)
    {
        const float y = h * 0.60f + i * (h * 0.40f / 26.0f);
        const float half = 6.0f + i * 3.2f;
        const float off = std::sin(t * 1.7f + i * 0.9f) * (4.0f + i * 0.6f);
        const unsigned char a = static_cast<unsigned char>(std::max(0.0f, 70.0f - i * 2.2f) * (0.6f + 0.4f * std::sin(t * 3.1f + i * 1.3f)));
        DrawRectangleRec(Rectangle{mx - half + off, y, half * 2, 2.0f}, Color{255, 236, 190, a});
    }
    struct Layer { float y, amp, k, speed; Color body, crest; };
    const Layer layers[] = {
        {0.60f, 7.0f, 0.020f, 0.55f, Color{10, 52, 78, 120}, Color{120, 190, 210, 70}},
        {0.70f, 9.0f, 0.016f, -0.75f, Color{8, 44, 68, 150}, Color{110, 180, 200, 70}},
        {0.81f, 11.0f, 0.013f, 0.95f, Color{6, 36, 58, 185}, Color{100, 170, 190, 75}},
        {0.92f, 13.0f, 0.011f, -1.15f, Color{4, 28, 46, 215}, Color{90, 160, 180, 80}},
    };
    const int step = 6;
    for (const Layer &L : layers)
    {
        Vector2 prev{};
        for (int x = 0; x <= w + step; x += step)
        {
            const float fx = static_cast<float>(x);
            const float y = h * L.y + L.amp * std::sin(fx * L.k + t * L.speed) + L.amp * 0.4f * std::sin(fx * L.k * 2.3f - t * L.speed * 1.6f);
            DrawRectangleRec(Rectangle{fx, y, static_cast<float>(step), h - y}, L.body);
            const Vector2 cur{fx, y};
            if (x > 0)
                DrawLineEx(prev, cur, 1.5f, L.crest);
            prev = cur;
        }
    }
}

// Dark wood panel with a gold double trim and corner studs (behind the ImGui window contents).
void drawPanel(ImDrawList *dl, ImVec2 a, ImVec2 b)
{
    const float r = ImGui::GetStyle().WindowRounding;
    dl->AddRectFilled(ImVec2(a.x + 6, a.y + 8), ImVec2(b.x + 6, b.y + 10), IM_COL32(0, 0, 0, 90), r); // shadow
    dl->AddRectFilled(a, b, IM_COL32(44, 27, 15, 250), r);
    // wood grain
    for (int i = 0; i < 46; ++i)
    {
        const uint32_t h = 2246822519u * static_cast<uint32_t>(i + 3);
        const float y = a.y + 14 + (b.y - a.y - 34) * (static_cast<float>(i) / 46.0f) + static_cast<float>(h % 7u);
        const float x0 = a.x + 14 + static_cast<float>(h % 61u), x1 = b.x - 14 - static_cast<float>((h >> 8) % 73u);
        dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), (i & 1) ? IM_COL32(70, 44, 24, 70) : IM_COL32(26, 15, 8, 80), 1.0f);
    }
    dl->AddRectFilledMultiColor(ImVec2(a.x + 3, a.y + 3), ImVec2(b.x - 3, a.y + 70), IM_COL32(255, 220, 150, 14), IM_COL32(255, 220, 150, 14), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
    dl->AddRect(a, b, u32(kGold), r, 0, 2.5f);
    dl->AddRect(ImVec2(a.x + 7, a.y + 7), ImVec2(b.x - 7, b.y - 7), u32(kGoldDim, 0.65f), r - 5, 0, 1.0f);
    const ImVec2 corners[] = {ImVec2(a.x + 7, a.y + 7), ImVec2(b.x - 7, a.y + 7), ImVec2(a.x + 7, b.y - 7), ImVec2(b.x - 7, b.y - 7)};
    for (const ImVec2 &c : corners)
    {
        dl->AddCircleFilled(c, 4.5f, u32(kGold));
        dl->AddCircleFilled(ImVec2(c.x - 1.2f, c.y - 1.2f), 1.6f, u32(kGoldHi));
    }
}

void sectionHeader(const UiFonts &f, const char *text)
{
    ImGui::Dummy(ImVec2(0, 2));
    if (f.section) ImGui::PushFont(f.section, 21.0f);
    ImGui::TextColored(kGold, "%s", text);
    if (f.section) ImGui::PopFont();
    const ImVec2 p = ImGui::GetItemRectMin(), q = ImGui::GetItemRectMax();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float x1 = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
    const float y = (p.y + q.y) * 0.5f + 1;
    dl->AddRectFilledMultiColor(ImVec2(q.x + 12, y), ImVec2(x1, y + 1.5f), u32(kGold, 0.8f), u32(kGold, 0.0f), u32(kGold, 0.0f), u32(kGold, 0.8f));
}

void rowLabel(const char *text, float col)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kParchDim, "%s", text);
    ImGui::SameLine(col);
}

// Segmented control: equal-width buttons, the selected one gold. Returns true when changed.
// Screenshot of the framebuffer. raylib's LoadImageFromScreen() multiplies the screen size by the
// desktop DPI even without FLAG_WINDOW_HIGHDPI (reads past the framebuffer at 125%).
Image screenshotImage()
{
    int w = 0, h = 0;
    glfwGetFramebufferSize(static_cast<GLFWwindow *>(GetWindowHandle()), &w, &h);
    Image img = {};
    img.data = rlReadScreenPixels(w, h);
    img.width = w;
    img.height = h;
    img.mipmaps = 1;
    img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    return img;
}

// GLFW (inside raylib) for the real framebuffer size in logs; raylib has no getter for the mouse scale.
Vector2 GetMouseScaleForLog()
{
    // GetMousePosition() = cursor * scale; probe with a known cursor is not possible without moving
    // the real cursor, so report the ratio of screen size to framebuffer size (raylib's HIGHDPI scale).
    int fbw = 0, fbh = 0;
    glfwGetFramebufferSize(static_cast<GLFWwindow *>(GetWindowHandle()), &fbw, &fbh);
    return Vector2{fbw ? static_cast<float>(GetScreenWidth()) / fbw : 1.0f, fbh ? static_cast<float>(GetScreenHeight()) / fbh : 1.0f};
}

// Test/probe support: rect (UI units) of named controls and the control under the mouse this frame.
struct ItemRect { ImVec2 a, b; };
std::map<std::string, ItemRect> g_itemRects;
std::string g_hoverName;
void trackItem(const char *name)
{
    g_itemRects[name] = ItemRect{ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        g_hoverName = name;
}

bool segmented(const char *id, const char *const *labels, int n, int *v, float width)
{
    bool changed = false;
    ImGui::PushID(id);
    const float sp = 4.0f;
    const float bw = (width - sp * (n - 1)) / n;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(sp, ImGui::GetStyle().ItemSpacing.y));
    for (int i = 0; i < n; ++i)
    {
        if (i) ImGui::SameLine();
        const bool sel = (*v == i);
        if (sel)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.86f, 0.66f, 0.27f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.76f, 0.34f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 0.84f, 0.45f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.16f, 0.09f, 0.03f, 1.0f));
        }
        ImGui::PushID(i);
        if (ImGui::Button(labels[i], ImVec2(bw, 0)) && !sel) { *v = i; changed = true; }
        trackItem(labels[i]);
        ImGui::PopID();
        if (sel) ImGui::PopStyleColor(4);
    }
    ImGui::PopStyleVar();
    ImGui::PopID();
    return changed;
}

void statusLine(bool ok, const std::string &msg, float col)
{
    ImGui::SetCursorPosX(col);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float hgt = ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + 5, p.y + hgt * 0.5f), 4.5f, u32(ok ? kOk : kBad));
    ImGui::SetCursorPosX(col + 16);
    ImGui::TextColored(ok ? kOk : kBad, "%s", msg.c_str());
}
} // namespace

static bool runLauncherUi(Settings &s, const char *argv0)
{
    // Test runs: PS2X_TEST_WINDOW=unfocused|hidden keeps the launcher from taking focus.
    // BLACKKAT_LAUNCHER_SHOT=<png> saves a screenshot after ~1 s; BLACKKAT_LAUNCHER_AUTO=1 then
    // presses Set Sail (or closes the launcher if the settings are not launchable).
    // BLACKKAT_LAUNCHER_NAV=1 shows the controller/keyboard focus cursor from the start (screenshots).
    const char *tw = std::getenv("PS2X_TEST_WINDOW");
    // Scaling: the window works in real pixels (no FLAG_WINDOW_HIGHDPI). raylib 5.5's HIGHDPI mode
    // on X11 keeps the startup screen scale/mouse scale after a resize while the resize callback
    // switches the screen size to pixels, so the UI was drawn 1.25x too big, cropped and mis-hit
    // after any resize. Instead the UI is laid out in fixed "UI units" (design 920x760) and scaled
    // uniformly to the window (ui = min(w/920, h/760), clamped), with fonts rasterised at that
    // scale. BLACKKAT_LAUNCHER_RAYLIB_HIDPI=1 restores the old raylib HIGHDPI path (for comparison).
    const bool legacyHidpi = std::getenv("BLACKKAT_LAUNCHER_RAYLIB_HIDPI") && std::getenv("BLACKKAT_LAUNCHER_RAYLIB_HIDPI")[0] == '1';
    unsigned flags = FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | (legacyHidpi ? FLAG_WINDOW_HIGHDPI : 0u);
    const bool testHidden = tw && (tw[0] == 'h' || tw[0] == 'H');
    const bool testUnfocused = tw && (tw[0] == 'u' || tw[0] == 'U');
    if (testHidden) flags |= FLAG_WINDOW_HIDDEN | FLAG_WINDOW_UNFOCUSED;
    else if (testUnfocused) flags |= FLAG_WINDOW_UNFOCUSED;
    else flags |= FLAG_WINDOW_HIDDEN; // shown after it is placed on the chosen monitor
    SetConfigFlags(flags);
    SetTraceLogLevel(LOG_WARNING);
    constexpr float kDesignW = 920.0f, kDesignH = 802.0f, kMinUi = 0.75f, kMaxUi = 3.0f;
    InitWindow(static_cast<int>(kDesignW), static_cast<int>(kDesignH), "Black Kat - BlackKatRecomp launcher");
    const float dpi = std::clamp(GetWindowScaleDPI().y, 1.0f, 4.0f); // desktop scale (X11: Xft.dpi/96)
    if (!legacyHidpi)
    {
        SetWindowSize(static_cast<int>(std::lround(kDesignW * dpi)), static_cast<int>(std::lround(kDesignH * dpi)));
        SetWindowMinSize(static_cast<int>(std::ceil(kDesignW * kMinUi)), static_cast<int>(std::ceil(kDesignH * kMinUi)));
    }
    else
        SetWindowMinSize(840, 740);
    // Open on the same monitor the game will use (PS2X_MONITOR overrides the saved monitor=).
    {
        const char *envMon = std::getenv("PS2X_MONITOR");
        const int mon = ps2xMonitorResolve(envMon && envMon[0] ? envMon : s.monitor.c_str(), "launcher");
        if (mon >= 0)
            ps2xMonitorPlace(mon, false, testHidden, false, "launcher");
        if (!testHidden && !testUnfocused)
        {
            ClearWindowState(FLAG_WINDOW_HIDDEN);
            if (mon >= 0)
                ps2xMonitorPlace(mon, false, false, false, "launcher"); // again after mapping (WM placement)
        }
    }
    // Monitor picker entries: "Primary (default)" + one per connected monitor. Under XWayland
    // GLFW reports every monitor in X11 units (scaled by the largest desktop scale), so the real
    // mode and per-monitor scale come from kscreen-doctor (KDE, read-only) when it is available.
    struct KsOut { std::string mode; int hz = 0; double scale = 0.0; };
    std::map<std::string, KsOut> ks;
    if (onPath("kscreen-doctor"))
        if (FILE *pk = popen("kscreen-doctor -o 2>/dev/null", "r"))
        {
            std::string txt;
            char buf[4096];
            size_t nr;
            while ((nr = std::fread(buf, 1, sizeof buf, pk)) > 0) txt.append(buf, nr);
            pclose(pk);
            std::string clean; // strip ANSI colour codes
            for (size_t i = 0; i < txt.size(); ++i)
            {
                if (txt[i] == '\x1b') { while (i < txt.size() && txt[i] != 'm') ++i; continue; }
                clean += txt[i];
            }
            std::istringstream is(clean);
            std::string line, cur;
            while (std::getline(is, line))
            {
                std::istringstream ls(line);
                std::string w;
                ls >> w;
                if (w == "Output:") { std::string idx; ls >> idx >> cur; }
                else if (!cur.empty() && w == "Modes:")
                {
                    std::string tok;
                    while (ls >> tok)
                        if (!tok.empty() && tok.back() == '*')
                        {
                            tok.pop_back();
                            if (tok.back() == '!') tok.pop_back();
                            const size_t c = tok.find(':'), at = tok.find('@');
                            if (c != std::string::npos && at != std::string::npos)
                            {
                                ks[cur].mode = tok.substr(c + 1, at - c - 1);
                                ks[cur].hz = static_cast<int>(std::lround(std::atof(tok.c_str() + at + 1)));
                            }
                        }
                }
                else if (!cur.empty() && w == "Scale:") { double sc = 0; ls >> sc; ks[cur].scale = sc; }
            }
        }

    struct MonEntry { std::string value, label; };
    std::vector<MonEntry> monEntries;
    monEntries.push_back({"", "Primary (default)"});
    {
        std::istringstream ml(ps2xMonitorListText());
        std::string line;
        int idx = 0;
        while (std::getline(ml, line))
        {
            std::string name, size, hz;
            std::istringstream ls(line);
            std::getline(ls, name, '|'); std::getline(ls, size, '|'); std::getline(ls, hz, '|');
            std::string label = (name.empty() ? "Monitor " + std::to_string(idx + 1) : name) + "   ";
            if (auto it = ks.find(name); it != ks.end() && !it->second.mode.empty())
            {
                label += it->second.mode + " @" + std::to_string(it->second.hz);
                if (it->second.scale > 0.0 && std::fabs(it->second.scale - 1.0) > 0.01)
                    label += "   " + std::to_string(static_cast<int>(std::lround(it->second.scale * 100))) + "%";
            }
            else
                label += size + " @" + hz;
            if (idx == 0) label += "   (primary)";
            monEntries.push_back({name.empty() ? std::to_string(idx) : name, label});
            ++idx;
        }
    }
    SetTraceLogLevel(LOG_INFO);
    SetTargetFPS(60);
    SetExitKey(KEY_NULL); // Esc / B cancel inside the UI; close with the window button or Quit
    rlImGuiSetup(true);
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    std::fprintf(stderr, "[launcher] window %dx%d dpi=%.2f scaling=%s\n", GetScreenWidth(), GetScreenHeight(), dpi, legacyHidpi ? "raylib-highdpi(legacy)" : "ui-units");

    // Fonts: system fonts through fontconfig (Noto/DejaVu/Liberation, whatever fc-match picks);
    // raylib/ImGui's built-in font if fontconfig finds nothing. Nothing is bundled.
    UiFonts fonts;
    {
        const std::string sans = fcMatch("sans-serif:style=Regular");
        const std::string serif = fcMatch("serif:style=Bold");
        if (!sans.empty())
            fonts.ui = io.Fonts->AddFontFromFileTTF(sans.c_str(), 17.0f);
        if (!serif.empty())
            fonts.title = fonts.section = io.Fonts->AddFontFromFileTTF(serif.c_str(), 34.0f);
        if (fonts.ui)
            io.FontDefault = fonts.ui;
        else
            ImGui::GetStyle().FontScaleMain = 1.25f;
        std::fprintf(stderr, "[launcher] fonts: ui=%s title=%s\n", sans.empty() ? "(built-in)" : sans.c_str(), serif.empty() ? "(built-in)" : serif.c_str());
    }
    applyPirateStyle();

    const std::string shotPath = std::getenv("BLACKKAT_LAUNCHER_SHOT") ? std::getenv("BLACKKAT_LAUNCHER_SHOT") : "";
    const bool autoLaunch = std::getenv("BLACKKAT_LAUNCHER_AUTO") && std::getenv("BLACKKAT_LAUNCHER_AUTO")[0] == '1';
    const bool navShow = std::getenv("BLACKKAT_LAUNCHER_NAV") && std::getenv("BLACKKAT_LAUNCHER_NAV")[0] == '1';
    const fs::path repo = repoRootGuess(argv0);

    char isoBuf[4096], elfBuf[4096];
    copyStr(isoBuf, sizeof isoBuf, s.iso);
    if (s.elf.empty()) s.elf = defaultElf(argv0);
    copyStr(elfBuf, sizeof elfBuf, s.elf);
    IsoCheck iso = checkIso(s.iso, &s);
    std::string checkedIso = s.iso;
    std::future<std::string> dialog;
    int dialogFor = 0; // 1 iso, 2 elf
    std::string extractMsg;
    bool launch = false, done = false;
    int frame = 0;
    const int winSizes[][2] = {{640, 448}, {960, 672}, {1280, 896}, {1600, 1120}, {1920, 1344}};
    const char *winNames[] = {"640 x 448", "960 x 672", "1280 x 896", "1600 x 1120", "1920 x 1344", "Custom"};
    const char *scaleLabels[] = {"1x", "2x", "3x", "4x"};
    const char *rendererLabels[] = {"OpenGL", "Vulkan", "CPU (software)"};
    const char *modeLabels[] = {"Windowed", "Fullscreen"};
    const char *promptLabels[] = {"PS2  (original)", "Xbox  (A B X Y)", "Auto"};
    bool gpB = false;
    // BLACKKAT_LAUNCHER_RESIZE_TEST=<png prefix>: resize to several sizes, screenshot each
    // (<prefix><name>.png), log sizes and which control is under synthetic mouse positions.
    const std::string resizePrefix = std::getenv("BLACKKAT_LAUNCHER_RESIZE_TEST") ? std::getenv("BLACKKAT_LAUNCHER_RESIZE_TEST") : "";
    struct TestSize { const char *name; int w, h; };
    const std::vector<TestSize> testSizes = {
        {"min", static_cast<int>(std::ceil(kDesignW * kMinUi)), static_cast<int>(std::ceil(kDesignH * kMinUi))},
        {"1280x800", static_cast<int>(std::lround(1280 * dpi)), static_cast<int>(std::lround(800 * dpi))},
        {"1920x1080", static_cast<int>(std::lround(1920 * dpi)), static_cast<int>(std::lround(1080 * dpi))},
        {"wide", static_cast<int>(std::lround(2400 * dpi)), static_cast<int>(std::lround(700 * dpi))},
        {"tall", static_cast<int>(std::lround(800 * dpi)), static_cast<int>(std::lround(1200 * dpi))},
        {"default", static_cast<int>(std::lround(kDesignW * dpi)), static_cast<int>(std::lround(kDesignH * dpi))},
    };
    const char *probeNames[] = {"Set Sail", "Browse##iso", "4x", "##monitor", "Quit", "Xbox  (A B X Y)"};
    constexpr int kProbeN = 6, kStep = 50;
    bool probeActive = false;
    float probeX = 0, probeY = 0;
    const char *probeName = nullptr;
    // F11 / Alt+Enter: borderless fullscreen toggle (maximise works through the window manager).

    while (!done && !WindowShouldClose())
    {
        ++frame;
        if (dialog.valid() && dialog.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            const std::string picked = dialog.get();
            if (!picked.empty())
                copyStr(dialogFor == 1 ? isoBuf : elfBuf, sizeof isoBuf, picked);
        }
        s.iso = trim(isoBuf);
        s.elf = trim(elfBuf);
        if (s.iso != checkedIso)
        {
            iso = checkIso(s.iso, &s);
            checkedIso = s.iso;
        }
        std::error_code ec;
        const bool elfOk = !s.elf.empty() && fs::is_regular_file(s.elf, ec);
        const bool canLaunch = iso.ok() && elfOk;
        // Pixel size of the window (legacy: raylib's logical screen size) and the UI scale.
        const int PW = GetScreenWidth(), PH = GetScreenHeight();
        const float ui = legacyHidpi ? 1.0f : std::clamp(std::min(PW / kDesignW, PH / kDesignH), kMinUi, kMaxUi);
        const float W = PW / ui, H = PH / ui; // window size in UI units (>= 920x760 in one axis)
        const float t = static_cast<float>(GetTime());
        if (IsKeyPressed(KEY_F11) || (IsKeyPressed(KEY_ENTER) && (IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT))))
            ToggleBorderlessWindowed();

        // Resize test: phase per size: 0 resize, 30 log + screenshot (after EndDrawing), 32.. probes.
        probeActive = false;
        probeName = nullptr;
        if (!resizePrefix.empty())
        {
            const int idx = (frame - 20) / kStep, ph = (frame - 20) % kStep;
            if (frame >= 20 && idx < static_cast<int>(testSizes.size()))
            {
                if (ph == 0)
                    SetWindowSize(testSizes[idx].w, testSizes[idx].h);
                if (ph >= 32 && ph < 32 + kProbeN)
                {
                    probeName = probeNames[ph - 32];
                    auto it = g_itemRects.find(probeName);
                    if (it != g_itemRects.end())
                    {
                        probeX = (it->second.a.x + it->second.b.x) * 0.5f * ui; // control centre in window pixels
                        probeY = (it->second.a.y + it->second.b.y) * 0.5f * ui;
                        probeActive = true;
                    }
                }
            }
            else if (frame >= 20)
                done = true;
        }

        // Controller: B = back/cancel (ImGui Escape), Start = Set Sail. rlImGui maps A/D-pad/stick.
        const bool bDown = IsGamepadAvailable(0) && IsGamepadButtonDown(0, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT);
        if (bDown != gpB) { io.AddKeyEvent(ImGuiKey_Escape, bDown); gpB = bDown; }
        if (canLaunch && IsGamepadAvailable(0) && IsGamepadButtonPressed(0, GAMEPAD_BUTTON_MIDDLE_RIGHT))
            launch = done = true;

        BeginDrawing();
        ClearBackground(Color{6, 14, 36, 255});
        drawSea(t, PW, PH); // background in window pixels, full size every frame
        g_hoverName.clear();
        if (legacyHidpi)
            rlImGuiBegin();
        else
        {
            // rlImGuiBegin() split up so the UI can run in UI units: display size = pixels / ui,
            // fonts rasterised at ui (framebuffer scale), mouse positions from the backend divided by ui.
            ImGui_ImplRaylib_NewFrame();
            ImGui_ImplRaylib_ProcessEvents();
            io.DisplaySize = ImVec2(W, H);
            io.DisplayFramebufferScale = ImVec2(ui, ui);
            const float tiny = std::numeric_limits<float>::min(); // rlImGui's "no mouse" position
            for (ImGuiInputEvent &e : ImGui::GetCurrentContext()->InputEventsQueue)
                if (e.Type == ImGuiInputEventType_MousePos && e.MousePos.PosX != tiny && e.MousePos.PosX != -FLT_MAX)
                {
                    e.MousePos.PosX /= ui;
                    e.MousePos.PosY /= ui;
                }
            if (probeActive)
                io.AddMousePosEvent(probeX / ui, probeY / ui);
            ImGui::NewFrame();
        }
        if (navShow && frame > 2) ImGui::SetNavCursorVisible(true);
        // Layout in UI units: a 920x760 block (title + panel) centred in the window; the panel grows
        // sideways up to 1040 wide on wide windows and keeps its design height on tall ones.
        const float margin = 24.0f;
        const float y0 = std::max(0.0f, std::floor((H - kDesignH) * 0.5f));
        const float top = y0 + 96.0f;
        const float panelW = std::min(W - 2 * margin, 1040.0f);
        const float panelX = std::floor((W - panelW) * 0.5f);
        // Title on the night sky, above the panel
        {
            ImDrawList *bg = ImGui::GetBackgroundDrawList();
            const char *title = "Pirates: The Legend of Black Kat";
            ImFont *tf = fonts.title ? fonts.title : ImGui::GetFont();
            const float tsz = fonts.title ? 40.0f : 26.0f;
            const ImVec2 sz = tf->CalcTextSizeA(tsz, FLT_MAX, 0.0f, title);
            const ImVec2 p((W - sz.x) * 0.5f, y0 + 14.0f);
            bg->AddText(tf, tsz, ImVec2(p.x + 2, p.y + 3), IM_COL32(0, 0, 0, 160), title);
            bg->AddText(tf, tsz, p, u32(kGold), title);
            const char *sub = "BlackKatRecomp  \xC2\xB7  NTSC-U SLUS-20365";
            ImFont *uf = ImGui::GetFont();
            const float usz = 17.0f;
            const ImVec2 ss = uf->CalcTextSizeA(usz, FLT_MAX, 0.0f, sub);
            bg->AddText(uf, usz, ImVec2((W - ss.x) * 0.5f, p.y + sz.y + 4), u32(kParchDim), sub);
        }
        ImGui::SetNextWindowPos(ImVec2(panelX, top));
        ImGui::SetNextWindowSize(ImVec2(panelW, kDesignH - 96.0f - margin));
        ImGui::Begin("launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
        const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        drawPanel(ImGui::GetBackgroundDrawList(), wp, ImVec2(wp.x + ws.x, wp.y + ws.y));
        const float innerW = ImGui::GetContentRegionAvail().x;
        const float col = 170.0f; // label column

        const float browseW = 96.0f;
        sectionHeader(fonts, "Game");
        rowLabel("Disc image", col);
        ImGui::SetNextItemWidth(innerW - col - browseW - 10);
        ImGui::InputTextWithHint("##iso", "path to the Black Kat (NTSC-U) .iso", isoBuf, sizeof isoBuf);
        ImGui::SameLine();
        ImGui::BeginDisabled(dialog.valid());
        const bool browseIso = ImGui::Button("Browse##iso", ImVec2(browseW, 0));
        trackItem("Browse##iso");
        if (browseIso)
        {
            const std::string start = s.iso.empty() ? std::string() : fs::path(s.iso).parent_path().string();
            dialogFor = 1;
            dialog = std::async(std::launch::async, [start]() { return fileDialog(start, true, "Choose the Black Kat disc image"); });
        }
        ImGui::EndDisabled();
        statusLine(iso.ok(), iso.message, col);

        rowLabel("Game folder", col);
        ImGui::SetNextItemWidth(innerW - col - browseW - 10);
        ImGui::InputTextWithHint("##elf", "extracted SLUS_203.65 (saves live in mc0 next to it)", elfBuf, sizeof elfBuf);
        ImGui::SameLine();
        ImGui::BeginDisabled(dialog.valid());
        if (ImGui::Button("Browse##elf", ImVec2(browseW, 0)))
        {
            const std::string start = s.elf.empty() ? std::string() : fs::path(s.elf).parent_path().string();
            dialogFor = 2;
            dialog = std::async(std::launch::async, [start]() { return fileDialog(start, false, "Choose SLUS_203.65 (extracted game folder)"); });
        }
        ImGui::EndDisabled();
        if (elfOk)
            statusLine(true, "SLUS_203.65 found", col);
        else
        {
            statusLine(false, "Not found. Extract the disc first (tools/extract_iso.sh).", col);
            if (!repo.empty() && iso.ok())
            {
                ImGui::SameLine();
                if (ImGui::SmallButton("Extract from this ISO"))
                {
                    const std::string cmd = "cd " + shellQuote(repo.string()) + " && bash tools/extract_iso.sh " + shellQuote(s.iso) + " >/dev/null 2>&1";
                    const int rc = std::system(cmd.c_str());
                    extractMsg = rc == 0 ? "Extracted to " + (repo / "game").string() : "Extraction failed (exit " + std::to_string(rc) + ")";
                    if (rc == 0) copyStr(elfBuf, sizeof elfBuf, (repo / "game" / "SLUS_203.65").string());
                }
            }
            if (!extractMsg.empty()) { ImGui::SetCursorPosX(col); ImGui::TextDisabled("%s", extractMsg.c_str()); }
        }
        rowLabel("Sound & movies", col);
        ImGui::Checkbox("Sound##audio", &s.audio);
        ImGui::SameLine(0, 28);
        ImGui::Checkbox("Intro and story movies##movies", &s.movies);

        sectionHeader(fonts, "Display");
        const float segW = std::min(420.0f, innerW - col);
        rowLabel("Renderer", col);
        int r = !s.gpu ? 2 : s.vulkan ? 1 : 0;
        if (segmented("renderer", rendererLabels, 3, &r, segW)) { s.gpu = (r != 2); s.vulkan = (r == 1); }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("OpenGL is the default. Vulkan is experimental and falls back to OpenGL if it fails.");
        rowLabel("Resolution", col);
        ImGui::BeginDisabled(!s.gpu);
        int si = std::clamp(s.scale, 1, 4) - 1;
        if (segmented("scale", scaleLabels, 4, &si, segW)) s.scale = si + 1;
        ImGui::EndDisabled();
        ImGui::SameLine(0, 14);
        ImGui::AlignTextToFramePadding();
        if (s.gpu)
            ImGui::TextColored(kParchDim, "%d x %d", 640 * s.scale, 448 * s.scale);
        else
            ImGui::TextDisabled(s.vulkan && s.gpu ? "640 x 448 (OpenGL only)" : "640 x 448 (GPU only)");
        ImGui::SameLine(0, 18);
        ImGui::BeginDisabled(!(s.gpu && s.vulkan));
        ImGui::Checkbox("FXAA##aa", &s.aa);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Anti-aliasing (FXAA) on the final picture: smooths jagged edges. Vulkan only.");
        rowLabel("Aspect ratio", col);
        {
            const char *aspectLabels[] = {"4:3  (original)", "16:9  (widescreen)"};
            int ar = s.widescreen ? 1 : 0;
            ImGui::BeginGroup();
            if (segmented("aspect", aspectLabels, 2, &ar, segW)) s.widescreen = (ar == 1);
            ImGui::EndGroup();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("16:9 widens the 3D view (more on the sides, nothing stretched).\nThe HUD, menus and movies are stretched sideways. Windowed: the window is made 16:9 for the chosen height.");
        }
        rowLabel("Remaster", col);
        {   // vk20: text upscale + water reflection ripple (Vulkan only)
            const char *txL[] = {"Pixel text", "Smooth", "Sharp"};
            const char *rpL[] = {"Ripple", "Soft", "Calm"};
            const float hw = (segW - 14.0f) * 0.5f;
            ImGui::BeginDisabled(!(s.gpu && s.vulkan));
            ImGui::BeginGroup();
            segmented("textf", txL, 3, &g_textFilter, hw);
            ImGui::EndGroup();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Upscales menu / HUD text and 2D art at 2x-4x resolution.\nSharp: smooth crisp letter outlines. Smooth: soft filtering. Pixel text: original look. Vulkan only.");
            ImGui::SameLine(0, 14);
            ImGui::BeginGroup();
            segmented("ripple", rpL, 3, &g_ripple, hw);
            ImGui::EndGroup();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Water reflections: Ripple is the original effect (breaks the reflection into small squares),\nSoft reduces the ripple, Calm gives a clean mirror. Vulkan only.");
            // vk21: front-end menu font: the original bitmap font or IM FELL English Italic (SIL OFL)
            const char *mfL[] = {"Original font", "IM FELL"};
            ImGui::SetCursorPosX(col);
            ImGui::BeginGroup();
            segmented("mfont", mfL, 2, &g_menuFont, hw);
            ImGui::EndGroup();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Menu text: IM FELL redraws the main-menu lettering from a smooth vector font (IM FELL English Italic)\nat the internal resolution, keeping the game's colours, outline, shadow and layout. Original font is the game's bitmap font. Vulkan only.");
            ImGui::EndDisabled();
        }
        rowLabel("Window", col);
        int mode = s.fullscreen ? 1 : 0;
        if (segmented("mode", modeLabels, 2, &mode, 260.0f)) s.fullscreen = (mode == 1);
        ImGui::SameLine(0, 14);
        ImGui::BeginDisabled(s.fullscreen);
        int wi = 5;
        for (int i = 0; i < 5; ++i) if (winSizes[i][0] == s.winW && winSizes[i][1] == s.winH) wi = i;
        ImGui::SetNextItemWidth(150);
        if (ImGui::Combo("##win", &wi, winNames, 6) && wi < 5) { s.winW = winSizes[wi][0]; s.winH = winSizes[wi][1]; }
        if (wi == 5)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70); ImGui::InputInt("##ww", &s.winW, 0); ImGui::SameLine(); ImGui::Text("x"); ImGui::SameLine();
            ImGui::SetNextItemWidth(70); ImGui::InputInt("##wh", &s.winH, 0);
            s.winW = std::clamp(s.winW, 320, 7680); s.winH = std::clamp(s.winH, 200, 4320);
        }
        ImGui::EndDisabled();
        rowLabel("Monitor", col);
        {
            auto ieq = [](const std::string &a, const std::string &b) {
                if (a.size() != b.size()) return false;
                for (size_t i = 0; i < a.size(); ++i)
                    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
                return true;
            };
            int sel = -1;
            if (s.monitor.empty() || ieq(s.monitor, "primary")) sel = 0;
            for (size_t i = 1; i < monEntries.size() && sel < 0; ++i)
                if (ieq(monEntries[i].value, s.monitor)) sel = static_cast<int>(i);
            if (sel < 0 && !s.monitor.empty() && std::all_of(s.monitor.begin(), s.monitor.end(), [](char c) { return c >= '0' && c <= '9'; }))
            {
                const int ix = std::atoi(s.monitor.c_str()) + 1;
                if (ix > 0 && ix < static_cast<int>(monEntries.size())) sel = ix;
            }
            const std::string preview = sel >= 0 ? monEntries[sel].label : s.monitor + "  (not connected: primary)";
            ImGui::SetNextItemWidth(segW);
            if (ImGui::BeginCombo("##monitor", preview.c_str()))
            {
                for (size_t i = 0; i < monEntries.size(); ++i)
                {
                    const bool on = static_cast<int>(i) == sel;
                    if (ImGui::Selectable(monEntries[i].label.c_str(), on)) s.monitor = monEntries[i].value;
                    if (on) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            trackItem("##monitor");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Screen for the game window (windowed: centred, fullscreen: borderless on that screen).\nThe launcher opens there next time.");
        }

        sectionHeader(fonts, "Controls");
        rowLabel("Button prompts", col);
        segmented("prompts", promptLabels, 3, &s.buttons, segW);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Auto: Xbox prompts when an Xbox-style controller is connected at start, PS2 otherwise");
        rowLabel("Controller", col);
        ImGui::Checkbox("Rumble##rumble", &s.rumble);
        ImGui::SameLine(0, 28);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kParchDim, "Stick deadzone");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        ImGui::SliderFloat("##dz", &s.deadzone, 0.0f, 0.5f, "%.2f");
        s.deadzone = std::clamp(s.deadzone, 0.0f, 0.9f);

        // Footer: big gold Set Sail, Quit, hints
        const float footerH = 64.0f;
        const float fy = ws.y - ImGui::GetStyle().WindowPadding.y - footerH;
        if (ImGui::GetCursorPosY() < fy) ImGui::SetCursorPosY(fy);
        ImGui::BeginDisabled(!canLaunch);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.84f, 0.62f, 0.22f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.96f, 0.76f, 0.32f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 0.86f, 0.48f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.17f, 0.09f, 0.03f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Border, kGoldHi);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
        if (fonts.title) ImGui::PushFont(fonts.title, 26.0f);
        const bool sail = ImGui::Button("Set Sail", ImVec2(240, footerH - 8));
        trackItem("Set Sail");
        if (frame <= 2) ImGui::SetItemDefaultFocus();
        if (canLaunch && (ImGui::IsItemHovered() || ImGui::IsItemFocused()))
        {
            const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
            ImDrawList *dl = ImGui::GetWindowDrawList();
            const float pulse = 0.5f + 0.5f * std::sin(t * 4.0f);
            for (int g = 1; g <= 4; ++g)
                dl->AddRect(ImVec2(r0.x - g * 2.0f, r0.y - g * 2.0f), ImVec2(r1.x + g * 2.0f, r1.y + g * 2.0f), u32(kGoldHi, (0.40f - g * 0.08f) * (0.6f + 0.4f * pulse)), 12.0f + g * 2.0f, 0, 2.0f);
        }
        if (fonts.title) ImGui::PopFont();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(5);
        ImGui::EndDisabled();
        if (sail || (canLaunch && ImGui::IsKeyPressed(ImGuiKey_Enter) && !io.WantTextInput))
            launch = done = true;
        ImGui::SameLine(0, 14);
        ImGui::BeginGroup();
        const bool quitPressed = ImGui::Button("Quit", ImVec2(110, 0));
        trackItem("Quit");
        if (quitPressed)
            done = true;
        ImGui::TextDisabled("Settings: %s", configPath().c_str());
        ImGui::EndGroup();
        ImGui::SameLine();
        {
            const char *hint = "D-pad / stick: move   A: select   B: back   Start: Set Sail";
            const ImVec2 hs = ImGui::CalcTextSize(hint);
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ws.x - ImGui::GetStyle().WindowPadding.x - hs.x));
            ImGui::TextDisabled("%s", hint);
        }
        if (!canLaunch)
        {
            ImGui::SetCursorPosY(fy - 26);
            ImGui::TextColored(kBad, "Choose the disc image and the extracted game folder to set sail.");
        }
        ImGui::End();
        if (legacyHidpi)
            rlImGuiEnd();
        else
        {
            // Render the UI-unit draw data scaled to window pixels (vertices and clip rects); the
            // backend's scissor maths then works in pixels (it uses io.DisplaySize for the flip).
            ImGui::Render();
            ImDrawData *dd = ImGui::GetDrawData();
            for (ImDrawList *dl : dd->CmdLists)
                for (ImDrawVert &v : dl->VtxBuffer)
                {
                    v.pos.x *= ui;
                    v.pos.y *= ui;
                }
            dd->ScaleClipRects(ImVec2(ui, ui));
            io.DisplaySize = ImVec2(static_cast<float>(PW), static_cast<float>(PH));
            ImGui_ImplRaylib_RenderDrawData(dd);
        }
        EndDrawing();

        if (!resizePrefix.empty() && frame >= 20)
        {
            const int idx = (frame - 20) / kStep, ph = (frame - 20) % kStep;
            if (idx < static_cast<int>(testSizes.size()))
            {
                if (ph == 30)
                {
                    int fbw = 0, fbh = 0;
                    glfwGetFramebufferSize(static_cast<GLFWwindow *>(GetWindowHandle()), &fbw, &fbh);
                    const Vector2 ms = GetMouseScaleForLog();
                    std::fprintf(stderr, "[launcher:resize] %-9s requested=%dx%d screen=%dx%d render=%dx%d framebuffer=%dx%d ui=%.3f ui_units=%.0fx%.0f screen_per_fb=%.3f\n",
                                 testSizes[idx].name, testSizes[idx].w, testSizes[idx].h, GetScreenWidth(), GetScreenHeight(), GetRenderWidth(), GetRenderHeight(),
                                 fbw, fbh, ui, W, H, ms.x);
                    const std::string path = resizePrefix + testSizes[idx].name + ".png";
                    Image img = screenshotImage();
                    ExportImage(img, path.c_str());
                    UnloadImage(img);
                }
                if (ph >= 32 && ph < 32 + kProbeN && probeName)
                    std::fprintf(stderr, "[launcher:probe] %-9s mouse px=(%.0f,%.0f) target='%s' -> hovered='%s'%s\n", testSizes[idx].name, probeX, probeY,
                                 probeName, g_hoverName.c_str(), (probeActive && g_hoverName == probeName) ? "" : "  <-- MISMATCH");
            }
        }

        if (!shotPath.empty() && frame == 60)
        {
            Image img = screenshotImage();
            ExportImage(img, shotPath.c_str());
            UnloadImage(img);
            std::fprintf(stderr, "[launcher] screenshot %s\n", shotPath.c_str());
        }
        if (autoLaunch && frame == 70)
        {
            launch = canLaunch;
            done = true;
        }
    }
    if (dialog.valid()) dialog.wait();
    rlImGuiShutdown();
    CloseWindow();
    s.iso = trim(isoBuf);
    s.elf = trim(elfBuf);
    if (launch)
        saveSettings(s);
    return launch;
}
} // namespace bklaunch
