// Graphics Settings in the game's own menus (graphics_menu.cpp): the rows of game_options' Graphics Settings page
// from config.ini's settings, saved and applied as they change.
#pragma once

#include "app_paths.h"
#include "settings_page.h"

#include <functional>
#include <mutex>

namespace mh3u::gfx {
class Presenter;
}

namespace mh3u::boot {

// What changes while the game runs: display mode, window size, scaling, filters, effects, texture packs (internal
// resolution, frame rate and VSync are read at start-up). Host main thread.
void apply_live_settings(gfx::Presenter& presenter, const Settings& settings);

class GraphicsMenu {
public:
    // presenter: null when headless (nothing to resize). on_saved: told of saved changes (from apply_pending).
    GraphicsMenu(const AppPaths& paths, gfx::Presenter* presenter, Settings settings, std::function<void(const Settings&)> on_saved);
    game::SettingsPage page();          // all the rows, four pages (the field menu's CFG tab shows one each)
    game::SettingsPage display_page();  // Display Settings: the screen (mode, size, resolution, scaling, frame rate, VSync)
    game::SettingsPage graphics_page(); // Graphics Settings: the look (Tri's look, bloom, DOF, AO, GI, reflections, vignette)
    game::SettingsPage camera_page();   // Camera Settings: the Modern Camera, the camera's distance
    game::SettingsPage mods_page();     // Mods: Cha-Cha's mods (mods.h, Texture Packs among them), on / off
    // Called after a mod is switched here (to save mods.ini).
    void set_mods_saved(std::function<void()> saved) { mods_saved_ = std::move(saved); }
    // Host main thread, every loop iteration: applies what changed since the last call.
    void apply_pending();

private:
    std::vector<game::SettingRow> rows();
    game::SettingsPage part(const char* title, int first, int end);
    void change(int row, int value);

    const AppPaths& paths_;
    gfx::Presenter* presenter_;
    std::function<void(const Settings&)> on_saved_;
    std::function<void()> mods_saved_;
    std::mutex m_;
    Settings settings_;
    bool pending_ = false;
};

}  // namespace mh3u::boot
