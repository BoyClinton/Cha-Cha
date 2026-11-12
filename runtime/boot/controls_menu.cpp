// Cha-Cha's remapping pages in the game's own menus (user 2026-11-06: controller and keyboard apart).
//   Controller Settings: Options (Controller Layout: Nintendo, the default, or Custom - shown by itself once a button
//     is remapped; choosing Nintendo again resets the buttons -, Invert Camera Y, Reset: the controller only), then
//     every game button that takes a controller button.
//   Keyboard Settings: Options (Mouse Sensitivity, Invert Camera Y, Reset: keys and mouse only), then every game
//     button and stick direction on keyboard + mouse.
// Seven rows a page. A binding row waits for the next key or button (Esc cancels, Delete clears, 6 s without a press
// gives up); the game reads no input meanwhile. Everything touching input (runtime/input) runs on the host main
// thread: the page reads rows cached there and queues what it asks for.
#include "controls_menu.h"

#include "host_input.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace mh3u::boot {
namespace {

const std::vector<float> kSens = {0.025f, 0.05f, 0.08f, 0.12f};
const std::vector<std::string> kSensNames = {"Low", "Medium", "High", "Very High"};
const std::vector<std::string> kOffOn = {"Off", "On"};
constexpr int kPerPage = 7;
enum { kFirstOption, kInvertY, kReset };  // kFirstOption: Controller Layout / Mouse Sensitivity

}  // namespace

ControlsMenu::ControlsMenu(Kind kind) : kind_(kind) {
    for (const input::BindingRow& r : input::binding_rows())
        if (!pad() || r.pad) targets_.push_back(r.target);
    shown_ = build();
}

game::SettingsPage ControlsMenu::page() {
    game::SettingsPage p;
    p.title = pad() ? "Controller Settings" : "Keyboard Settings";
    p.page_starts = {0};
    p.page_titles = {"Options"};
    for (int i = 0; i < static_cast<int>(targets_.size()); i += kPerPage)
        p.page_starts.push_back(option_rows_ + i), p.page_titles.push_back(pad() ? "Buttons" : "Keys");
    p.rows = [this] { return rows(); };
    p.change = [this](int row, int value) { change(row, value); };
    p.activate = [this](int row) { activate(row); };
    p.live = true;
    return p;
}

std::optional<std::string> ControlsMenu::target(int row) const {
    const int i = row - option_rows_;
    if (i < 0 || i >= static_cast<int>(targets_.size())) return std::nullopt;
    return targets_[static_cast<size_t>(i)];
}

std::vector<game::SettingRow> ControlsMenu::build() const {
    const input::ControlOptions o = input::control_options();
    std::vector<game::SettingRow> r;
    if (pad()) {
        r.push_back({"Controller Layout",
                     "Nintendo: the buttons by their place, as\non the Pro Controller. Custom: remapped\nbelow. Nintendo again resets them.",
                     {"Nintendo", "Custom"}, input::pad_is_default() ? 0 : 1});
    } else {
        int sens = 0;
        for (size_t i = 0; i < kSens.size(); ++i)
            if (std::abs(kSens[i] - o.mouse_sensitivity) < std::abs(kSens[static_cast<size_t>(sens)] - o.mouse_sensitivity)) sens = static_cast<int>(i);
        r.push_back({"Mouse Sensitivity", "How fast the mouse turns the camera.", kSensNames, sens});
    }
    r.push_back({"Invert Camera Y", "Moving the mouse or stick up turns the\ncamera down.", kOffOn, o.invert_y ? 1 : 0});
    r.push_back({"Reset to Defaults", pad() ? "Every controller button back to the\nNintendo layout." : "Every key and mouse button back to\nCha-Cha's defaults.",
                 {"Reset"}, 0, true});
    for (const std::string& t : targets_) {
        const char* label = "";
        for (const input::BindingRow& row : input::binding_rows())
            if (t == row.target) label = row.label;
        const bool waiting = capturing_.load() && capture_ == t;
        std::string value = waiting ? (pad() ? "Press a button..." : "Press a key...") : pad() ? input::pad_names(t) : input::key_names(t);
        if (value.empty()) value = "-";
        const char* help = pad() ? "Press A, then the controller button for\nit. Delete clears it, Esc cancels."
                                 : "Press A, then the key or mouse button\nfor it. Delete clears it, Esc cancels.";
        r.push_back({label, help, {value}, 0, true});
    }
    return r;
}

std::vector<game::SettingRow> ControlsMenu::rows() {
    std::lock_guard g(m_);
    return shown_;
}

void ControlsMenu::change(int row, int value) {
    std::lock_guard g(m_);
    changes_.push_back({row, value});
}

void ControlsMenu::activate(int row) {
    std::lock_guard g(m_);
    activations_.push_back(row);
}

void ControlsMenu::finish_capture() {
    capturing_ = false;
    input::save_bindings();
}

bool ControlsMenu::handle_event(const void* ev) {
    if (!capturing_.load()) return false;
    const SDL_Event& e = *static_cast<const SDL_Event*>(ev);
    if (e.type == SDL_EVENT_GAMEPAD_ADDED || e.type == SDL_EVENT_GAMEPAD_REMOVED) return false;
    if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_ESCAPE) {
        capturing_ = false;
    } else if (e.type == SDL_EVENT_KEY_DOWN && (e.key.scancode == SDL_SCANCODE_DELETE || e.key.scancode == SDL_SCANCODE_BACKSPACE)) {
        input::clear_binding(capture_, pad());
        finish_capture();
    } else if (input::bind_from_event(capture_, pad(), &e)) {
        std::printf("controls: %s bound (%s)\n", capture_.c_str(), pad() ? "controller" : "keyboard / mouse");
        finish_capture();
    }
    return true;  // the press is the binding, not input for the game
}

void ControlsMenu::update() {
    std::vector<std::pair<int, int>> changes;
    std::vector<int> activations;
    {
        std::lock_guard g(m_);
        changes.swap(changes_);
        activations.swap(activations_);
    }
    bool saved = false;
    for (auto [row, value] : changes) {
        input::ControlOptions o = input::control_options();
        if (row == kFirstOption && pad()) {
            if (value == 0 && !input::pad_is_default()) input::reset_pad_bindings();  // Custom -> Nintendo
            saved = true;  // (Custom on its own changes nothing: it comes from remapping)
            continue;
        }
        if (row == kFirstOption) o.mouse_sensitivity = kSens[static_cast<size_t>(std::clamp(value, 0, 3))];
        else if (row == kInvertY) o.invert_y = value != 0;
        else continue;
        input::set_control_options(o);
        saved = true;
    }
    for (int row : activations) {
        if (row == kReset) {
            if (pad()) input::reset_pad_bindings();
            else input::reset_key_bindings();
            saved = true;
        } else if (const auto t = target(row)) {
            capture_ = *t;
            capture_since_ = std::chrono::steady_clock::now();
            capturing_ = true;
        }
    }
    if (saved) input::save_bindings();
    const auto now = std::chrono::steady_clock::now();
    if (capturing_.load() && now - capture_since_ > std::chrono::seconds(6)) capturing_ = false;
    // the shown rows: again after anything happened, else ten times a second (a binding may have been made)
    const bool was = capturing_.load();
    if (changes.empty() && activations.empty() && was == shown_capturing_ && now - built_ < std::chrono::milliseconds(100)) return;
    built_ = now;
    shown_capturing_ = was;
    std::vector<game::SettingRow> fresh = build();
    std::lock_guard g(m_);
    shown_ = std::move(fresh);
}

}  // namespace mh3u::boot
