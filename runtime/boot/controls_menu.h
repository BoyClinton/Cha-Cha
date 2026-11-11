// Cha-Cha's remapping in the game's own menus (controls_menu.cpp), as native settings pages (src/game/settings_page.h):
// Controller Settings (the controller's buttons; replaces the game's own page) and Keyboard Settings (keyboard + mouse;
// in place of the game's Game Settings). One ControlsMenu per page.
#pragma once

#include "settings_page.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace mh3u::boot {

class ControlsMenu {
public:
    enum class Kind { Controller, Keyboard };
    explicit ControlsMenu(Kind kind);
    game::SettingsPage page();
    // Host main thread, every event: true when the event was taken (a binding is being waited for).
    bool handle_event(const void* sdl_event);
    // Host main thread, every loop iteration: applies queued changes, times out a wait, refreshes the shown rows.
    void update();
    // A binding is being waited for: the game reads no input meanwhile.
    bool capturing() const { return capturing_.load(); }

private:
    std::vector<game::SettingRow> rows();       // guest thread: the cached rows
    void change(int row, int value);           // guest thread: queued
    void activate(int row);                    // guest thread: queued
    std::optional<std::string> target(int row) const;
    std::vector<game::SettingRow> build() const;  // main thread
    void finish_capture();

    Kind kind_;
    bool pad() const { return kind_ == Kind::Controller; }
    std::vector<std::string> targets_;  // per row after the option rows
    int option_rows_ = 3;
    std::mutex m_;
    std::vector<game::SettingRow> shown_;
    std::vector<std::pair<int, int>> changes_;  // row, value
    std::vector<int> activations_;
    std::atomic<bool> capturing_{false};
    std::string capture_;
    std::chrono::steady_clock::time_point capture_since_, built_;
    bool shown_capturing_ = false;  // main thread: capturing_ when shown_ was built
};

}  // namespace mh3u::boot
