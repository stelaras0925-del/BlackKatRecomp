// SPDX-License-Identifier: GPL-3.0-only
//
// Built-in launcher (see launcher.h). Shown by `blackkat` with no arguments or with --launcher.

#include "launcher.h"

#include "raylib.h"
#include "imgui.h"
#include "rlImGui.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
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
        else if (k == "renderer") s.gpu = (v != "cpu");
        else if (k == "scale") s.scale = std::clamp(std::atoi(v.c_str()), 1, 4);
        else if (k == "audio") s.audio = b();
        else if (k == "movies") s.movies = b();
        else if (k == "fullscreen") s.fullscreen = b();
        else if (k == "window") { int w = 0, h = 0; if (std::sscanf(v.c_str(), "%dx%d", &w, &h) == 2 && w >= 320 && h >= 200) { s.winW = w; s.winH = h; } }
        else if (k == "iso_md5_key") s.md5Key = v;
        else if (k == "iso_md5_ok") s.md5Ok = std::atoi(v.c_str());
    }
}

static std::string serializeSettings(const Settings &s)
{
    std::ostringstream f;
    f << "iso=" << s.iso << "\n"
      << "elf=" << s.elf << "\n"
      << "renderer=" << (s.gpu ? "gpu" : "cpu") << "\n"
      << "scale=" << s.scale << "\n"
      << "audio=" << (s.audio ? 1 : 0) << "\n"
      << "movies=" << (s.movies ? 1 : 0) << "\n"
      << "fullscreen=" << (s.fullscreen ? 1 : 0) << "\n"
      << "window=" << s.winW << "x" << s.winH << "\n"
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
    set("PS2X_GS_SCALE", std::to_string(s.gpu ? std::clamp(s.scale, 1, 4) : 1));
    if (!s.audio) set("PS2X_HOST_AUDIO", "0");
    set("BLACKKAT_PLAY_FMV", s.movies ? "1" : "0");
    set("PS2X_FULLSCREEN", s.fullscreen ? "1" : "0");
    set("PS2X_WINDOW", std::to_string(s.winW) + "x" + std::to_string(s.winH));
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

static bool runLauncherUi(Settings &s, const char *argv0)
{
    // Test runs: PS2X_TEST_WINDOW=unfocused|hidden keeps the launcher from taking focus.
    // BLACKKAT_LAUNCHER_SHOT=<png> saves a screenshot after ~1 s; BLACKKAT_LAUNCHER_AUTO=1 then
    // presses Launch (or closes the launcher if the settings are not launchable).
    const char *tw = std::getenv("PS2X_TEST_WINDOW");
    unsigned flags = FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_WINDOW_HIGHDPI; // HIGHDPI: fractional desktop scaling (KDE 125%)
    if (tw && (tw[0] == 'h' || tw[0] == 'H')) flags |= FLAG_WINDOW_HIDDEN | FLAG_WINDOW_UNFOCUSED;
    else if (tw && (tw[0] == 'u' || tw[0] == 'U')) flags |= FLAG_WINDOW_UNFOCUSED;
    SetConfigFlags(flags);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(760, 560, "BlackKatRecomp launcher");
    SetTraceLogLevel(LOG_INFO);
    SetTargetFPS(60);
    rlImGuiSetup(true);
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().FontGlobalScale = 1.25f;
    ImGuiStyle &style = ImGui::GetStyle();
    style.FrameRounding = 4.0f;
    style.FramePadding = ImVec2(8, 5);
    style.ItemSpacing = ImVec2(10, 8);

    const std::string shotPath = std::getenv("BLACKKAT_LAUNCHER_SHOT") ? std::getenv("BLACKKAT_LAUNCHER_SHOT") : "";
    const bool autoLaunch = std::getenv("BLACKKAT_LAUNCHER_AUTO") && std::getenv("BLACKKAT_LAUNCHER_AUTO")[0] == '1';
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
    const char *scaleNames[] = {"Native (1x)  640x448", "2x  1280x896", "3x  1920x1344", "4x  2560x1792"};
    const int winSizes[][2] = {{640, 448}, {960, 672}, {1280, 896}, {1600, 1120}, {1920, 1344}};
    const char *winNames[] = {"640x448", "960x672", "1280x896", "1600x1120", "1920x1344", "Custom"};

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

        BeginDrawing();
        ClearBackground(Color{18, 22, 30, 255});
        rlImGuiBegin();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(GetScreenWidth()), static_cast<float>(GetScreenHeight())));
        ImGui::Begin("launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.35f, 1), "Pirates: The Legend of Black Kat");
        ImGui::SameLine();
        ImGui::TextDisabled("BlackKatRecomp (NTSC-U SLUS-20365)");
        ImGui::Separator();

        const float browseW = 90.0f;
        ImGui::Text("Disc image (ISO)");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - browseW - 10);
        ImGui::InputText("##iso", isoBuf, sizeof isoBuf);
        ImGui::SameLine();
        ImGui::BeginDisabled(dialog.valid());
        if (ImGui::Button("Browse##iso", ImVec2(browseW, 0)))
        {
            const std::string start = s.iso.empty() ? std::string() : fs::path(s.iso).parent_path().string();
            dialogFor = 1;
            dialog = std::async(std::launch::async, [start]() { return fileDialog(start, true, "Choose the Black Kat disc image"); });
        }
        ImGui::EndDisabled();
        ImGui::TextColored(iso.ok() ? ImVec4(0.4f, 0.85f, 0.4f, 1) : ImVec4(0.95f, 0.45f, 0.4f, 1), "%s", iso.message.c_str());

        ImGui::Text("Game folder (extracted SLUS_203.65; saves live in mc0 next to it)");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - browseW - 10);
        ImGui::InputText("##elf", elfBuf, sizeof elfBuf);
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
            ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.4f, 1), "OK");
        else
        {
            ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1), "Not found. Extract the disc first (tools/extract_iso.sh).");
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
            if (!extractMsg.empty()) ImGui::TextDisabled("%s", extractMsg.c_str());
        }
        ImGui::Separator();

        ImGui::Text("Renderer");
        ImGui::SameLine(200);
        int r = s.gpu ? 0 : 1;
        ImGui::RadioButton("GPU (OpenGL)", &r, 0);
        ImGui::SameLine();
        ImGui::RadioButton("CPU (software)", &r, 1);
        s.gpu = (r == 0);

        ImGui::Text("Internal resolution");
        ImGui::SameLine(200);
        ImGui::BeginDisabled(!s.gpu);
        ImGui::SetNextItemWidth(260);
        int si = std::clamp(s.scale, 1, 4) - 1;
        ImGui::Combo("##scale", &si, scaleNames, 4);
        s.scale = si + 1;
        ImGui::EndDisabled();
        if (!s.gpu) { ImGui::SameLine(); ImGui::TextDisabled("(GPU renderer only)"); }

        ImGui::Text("Audio");
        ImGui::SameLine(200);
        ImGui::Checkbox("Sound on##audio", &s.audio);
        ImGui::Text("Movies");
        ImGui::SameLine(200);
        ImGui::Checkbox("Play intro and story movies##movies", &s.movies);

        ImGui::Text("Display");
        ImGui::SameLine(200);
        ImGui::Checkbox("Fullscreen##fs", &s.fullscreen);
        ImGui::SameLine();
        ImGui::BeginDisabled(s.fullscreen);
        int wi = 5;
        for (int i = 0; i < 5; ++i) if (winSizes[i][0] == s.winW && winSizes[i][1] == s.winH) wi = i;
        ImGui::SetNextItemWidth(150);
        if (ImGui::Combo("Window##win", &wi, winNames, 6) && wi < 5) { s.winW = winSizes[wi][0]; s.winH = winSizes[wi][1]; }
        if (wi == 5)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80); ImGui::InputInt("##ww", &s.winW, 0); ImGui::SameLine(); ImGui::Text("x"); ImGui::SameLine();
            ImGui::SetNextItemWidth(80); ImGui::InputInt("##wh", &s.winH, 0);
            s.winW = std::clamp(s.winW, 320, 7680); s.winH = std::clamp(s.winH, 200, 4320);
        }
        ImGui::EndDisabled();

        ImGui::Separator();
        ImGui::BeginDisabled(!canLaunch);
        if (ImGui::Button("Launch", ImVec2(160, 40)) || (canLaunch && ImGui::IsKeyPressed(ImGuiKey_Enter) && !ImGui::GetIO().WantTextInput))
            launch = done = true;
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Quit", ImVec2(100, 40)))
            done = true;
        ImGui::SameLine();
        ImGui::TextDisabled("Settings: %s\nCommand-line options and env vars override them.", configPath().c_str());
        ImGui::End();
        rlImGuiEnd();
        EndDrawing();

        if (!shotPath.empty() && frame == 60)
        {
            Image img = LoadImageFromScreen();
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
