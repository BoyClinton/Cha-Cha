// Where the port keeps its own files, and which copy of the game it plays.
//
// Data folder (settings, saves, shader/pipeline caches; never game files):
//   --data-dir <dir>                  explicit
//   portable.txt next to Cha-Cha.exe  -> <exe dir>/data      (portable install, e.g. on a USB drive)
//   run inside the source checkout  -> the checkout        (save/, input.ini, cache/ are gitignored)
//   otherwise                       -> %APPDATA%\Cha-Cha
// The game: the sources recorded in <data>/config.ini ("game=<path>" lines, written by --install or the setup
// screen), overridden by --game <path> (repeatable); in the checkout extracted/ or game/*.wua are found by
// themselves.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace mh3u::boot {

struct AppPaths {
    std::filesystem::path exe_dir;
    std::filesystem::path data;   // see above
    std::filesystem::path repo;   // the source checkout, if running from one (empty otherwise)
    std::filesystem::path config() const { return data / "config.ini"; }
    std::filesystem::path saves() const { return data / "save"; }
    std::filesystem::path cache() const { return data / "cache"; }
};

AppPaths find_paths(const std::filesystem::path& exe, const std::filesystem::path& data_override);

// config.ini: "key=value" lines; game sources are the "game" entries, in order.
std::vector<std::filesystem::path> load_game_sources(const AppPaths& p);
bool save_game_sources(const AppPaths& p, const std::vector<std::filesystem::path>& sources);
// Player settings (config.ini keys; the launcher's SETTINGS page edits them).
struct Settings {
    bool skip_boot_notices = true;  // boot notices and save prompts confirmed by the game (src/game/boot_prompts.cpp)
    float render_scale = 1.0f;      // internal resolution, x the native 1280x720: 0.5, 0.75, 1, 1.5, 2, 3, 4
    std::string display_mode = "windowed";  // "windowed", "borderless" (desktop-sized) or "fullscreen" (exclusive)
    std::string window_size = "1280x720";   // windowed mode; independent of render_scale
    bool bloom = true;
    // "off", "original" (the game's own DOF filters, where it uses them) or "enhanced" (also our gameplay DOF)
    std::string depth_of_field = "original";
    bool ambient_occlusion = false;  // our screen-space AO (the game has none)
    bool global_illumination = false;  // our screen-space bounce light (the game has none)
    bool vignette = false;           // darker screen corners (our post pass)
    bool texture_packs = true;       // replacement textures from <data>/texture_replacements
    std::string look = "original";  // "original" (MH3U) or "tri" (Wii-style grade)
    int fps_limit = 0;              // 0 = display refresh rate, -1 = unlimited, else frames per second
    bool vsync = false;             // FIFO presentation (no tearing, waits for the display)
    bool operator==(const Settings&) const = default;
};
Settings load_settings(const AppPaths& p);
bool save_settings(const AppPaths& p, const Settings& s);

// Dump locations inside a source checkout (extracted/, game/*.wua), for development runs.
std::vector<std::filesystem::path> repo_game_sources(const AppPaths& p);

}  // namespace mh3u::boot
