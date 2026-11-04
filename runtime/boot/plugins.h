// Mods: folders in <data folder>/mods (docs/modding.md). Each mod can bring
//   content/   files that replace or add to the game's /vol/content (an overlay over the dump, nothing is copied)
//   plugins/   native plugins (.dll / .so, include/chacha_plugin.h): game hooks, a HUD layer, settings rows
//   mod.ini    name, author, version, description (all optional)
// Which mods are on is kept in <data folder>/mods.ini (mod.<folder>=on|off; new mods start on). Changes apply on the
// next start: content and plugins are loaded once, before the game boots.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

struct ImDrawList;
struct ImFont;

namespace mh3u::plugins {

struct PluginState {
    std::string file;   // the library's file name
    std::string name, version;
    bool loaded = false;
    std::string error;  // why it is not loaded
    void* handle = nullptr;  // the ChaChaPlugin (opaque here)
};

struct Mod {
    std::string folder;  // name of its folder in mods/
    std::filesystem::path dir;
    std::string name, author, version, description;
    bool enabled = true;      // as saved in mods.ini (applies on the next start)
    bool active = false;      // loaded in this session
    bool has_content = false; // content/ exists
    size_t content_files = 0;
    std::vector<std::filesystem::path> plugin_files;
    std::vector<PluginState> plugins;  // this session's, when active
};

std::filesystem::path mods_dir(const std::filesystem::path& data);
// Reads the mods folder (creating it) and the on / off states from mods.ini.
std::vector<Mod> scan(const std::filesystem::path& data);
// mod.<folder>=on|off, keeping mods.ini's other lines (the quest pack modes).
void save_enabled(const std::filesystem::path& data, const std::string& folder, bool on);

// Start-up, before the game's code runs and after Cha-Cha's own overrides are registered: scans the mods, loads the
// enabled mods' plugins (their init installs hooks). guest_base / sha1: the game's memory and RPX hash.
void load(const std::filesystem::path& data, uint8_t* guest_base, const std::string& rpx_sha1);
// The enabled mods' content/ folders, highest priority first (mods in folder-name order).
std::vector<std::filesystem::path> content_dirs();
// This session's mods (as loaded; enabled reflects later changes in the MODS tab).
std::vector<Mod>& mods();
// The launcher before the game boots: the mods folder read once, for the MODS tab.
void ensure_scanned(const std::filesystem::path& data);
void set_enabled(size_t mod, bool on);
// Plugin calls go through these; the launcher hands its toast here.
void set_toast(std::function<void(const std::string&)> fn);

// ---- for the launcher (host main thread) ---------------------------------------------------------------------
bool has_hud();        // some plugin draws over the game
bool fonts_pending();  // fonts asked for by plugins (and the HUD font) still to add to the atlas
// Adds the pending fonts to ImGui's atlas (the caller rebuilds the font texture). hud_font_path: a bold sans for
// the HUD's default font.
void add_pending_fonts(const std::filesystem::path& hud_font_path);
// Calls every plugin's on_hud, drawing into dl; display_w / h: the window in pixels.
void draw_hud(ImDrawList* dl, float display_w, float display_h);

// The settings rows a plugin's on_settings draws with (the launcher's own row widgets).
struct SettingsUi {
    std::function<void(const char*)> group;
    std::function<bool(const char*, bool&, const char*)> toggle;
    std::function<int(const char*, const std::string&, const char*)> value;  // -1 / +1 / 2 (activated) / 0
    std::function<bool(const char*, const char*, const std::string&)> button;
    std::function<void(const char*, const std::string&, const char*)> info;
    std::function<void()> end_group;
};
// Draws mod `index`'s plugins' settings (each plugin's rows in their own group unless it opens its own).
void draw_settings(size_t index, const SettingsUi& ui);
// any loaded plugin of the mod has settings
bool has_settings(size_t index);
// The Mods page's "Save settings" / "Restore defaults": every loaded plugin's on_save / on_defaults; how many had one.
int save_all();
int defaults_all();
void shutdown();

}  // namespace mh3u::plugins
