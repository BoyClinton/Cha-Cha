// The launcher (Dusklight-style), drawn over the game's own window: logo and menu PLAY / SETTINGS / MODS / QUIT, the
// game's status bottom-left, "Compiling ..." toasts while shaders are translated and pipelines built, the build
// version bottom-right. The game boots behind it as soon as it can (its intro and title screen play underneath) and
// gets no input until PLAY. Before the game runs (first start: choose the dump, translate shaders) a warm backdrop
// stands in for it.
//
// Headless and --install runs use run_setup instead: shaders are translated with console output, no window.
#pragma once

#include "app_paths.h"
#include "game_image.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mh3u::gfx {
class Presenter;
}

namespace mh3u::boot {

struct SetupResult {
    std::optional<fs::GameImage> game;  // null: headless setup failed
    std::filesystem::path shader_dir;   // translated shaders, current for this game
    Settings settings;
};

// Without a window: checks the game and translates its shaders if the cache is out of date (console output).
SetupResult run_setup(const AppPaths& paths, std::optional<fs::GameImage> game, std::string error);

// What the host tells the launcher each frame.
struct LaunchState {
    bool game_running = false;   // booted (its frames may still be loading)
    bool game_frames = false;    // a game frame is on screen: drawn over it, else over the backdrop
    size_t pipelines_done = 0, pipelines_total = 0;
    bool pipelines_ready = false;  // the pipeline prebuild finished: PLAY is available
};

class Launcher {
public:
    // sources/game/error: what the normal start found. show_menu false (--play): the overlay goes away by itself
    // once the game can be played.
    Launcher(const AppPaths& paths, gfx::Presenter& presenter, std::vector<std::filesystem::path> sources,
             std::optional<fs::GameImage> game, std::string error, Settings settings, bool show_menu);
    ~Launcher();
    Launcher(const Launcher&) = delete;
    Launcher& operator=(const Launcher&) = delete;

    // Host main thread: every window event (an SDL_Event).
    void handle_event(const void* sdl_event);
    // Host main thread, every loop iteration (draws at most ~60 times a second). False once the user chose QUIT.
    bool frame(const LaunchState& state);
    bool visible() const;  // the menu is up: the game gets no input
    // In game (after PLAY): F1 or a controller's Home / guide button opens the settings over the game, and again
    // (or BACK, Esc, B) returns to it. Host main thread.
    void toggle_settings();
    // F2: the MODS panel (quests, gameplay) the same way.
    void toggle_mods();
    // A toast over whatever is on screen (any thread; shown from the next frame).
    void post_toast(std::string text);
    // The game is chosen and its shaders are current: the host can boot it (take_game, once).
    bool ready_to_boot() const;
    fs::GameImage take_game();
    const Settings& settings() const;
    std::filesystem::path shader_dir() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mh3u::boot
