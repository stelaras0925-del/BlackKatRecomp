// SPDX-License-Identifier: GPL-3.0-only
//
// Built-in launcher for the blackkat binary: settings screen (raylib + Dear ImGui through
// rlImGui), ~/.config/blackkat/config.ini, ISO validation, and the mapping from settings to
// the runtime's environment variables.
#pragma once

#include <string>

namespace bklaunch
{
struct Settings
{
    std::string iso;        // disc image
    std::string elf;        // extracted SLUS_203.65 (game folder); empty = auto-detect
    bool gpu = true;        // GS renderer: GPU (OpenGL) or CPU
    int scale = 1;          // internal resolution multiplier (GPU only), 1..4
    bool audio = true;      // host audio output
    bool movies = true;     // play FMVs (IPU) or skip them
    bool fullscreen = false;
    bool widescreen = false; // aspect ratio: false 4:3 (original), true 16:9 (hor+ FOV, HUD stretched)
    int winW = 1280, winH = 896;
    std::string monitor;    // monitor for the game (and launcher) window: connector name (DP-2...), index, or "" = primary
    // Options
    int buttons = 0;        // button prompts: 0 PS2 (original), 1 Xbox, 2 auto (Xbox when an Xbox-style pad is connected)
    bool rumble = true;     // controller vibration
    float deadzone = 0.20f; // stick deadzone (0..0.9)
    // ISO md5 cache (md5 of SLUS_203.65 inside the image), keyed by path|size|mtime
    std::string md5Key;
    int md5Ok = 0;          // 1 ok, -1 mismatch, 0 unknown
};

struct IsoCheck
{
    bool exists = false;
    bool iso9660 = false;
    bool hasElf = false;
    bool sizeOk = false;
    int md5 = 0;            // 1 ok, -1 mismatch, 0 not checked
    std::string message;
    bool ok() const { return exists && iso9660 && hasElf && sizeOk && md5 >= 0; }
};

std::string configPath();
bool loadSettings(Settings &s);
bool saveSettings(const Settings &s);
IsoCheck checkIso(const std::string &path, Settings *md5Cache);
std::string defaultElf(const char *argv0);
// Shows the launcher window. Returns true when the user pressed Launch (settings updated and
// saved), false when the window was closed.
bool runLauncher(Settings &s, const char *argv0);
// Sets the runtime environment variables for these settings. Variables already set in the
// environment are left alone (env overrides the launcher/config).
void applyToEnv(const Settings &s);
}
