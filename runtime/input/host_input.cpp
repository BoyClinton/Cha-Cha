#include "host_input.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace mh3u::input {
namespace {

// A binding target: a GamePad button (mask) or one direction of a stick axis (0 lx, 1 ly, 2 rx, 3 ry).
struct Target {
    uint32_t mask = 0;
    int axis = -1;
    float dir = 0;
};

const std::map<std::string, uint32_t> kButtonTargets = {
    {"A", kA}, {"B", kB}, {"X", kX}, {"Y", kY}, {"L", kL}, {"R", kR}, {"ZL", kZL}, {"ZR", kZR}, {"PLUS", kPlus},
    {"MINUS", kMinus}, {"HOME", kHome}, {"UP", kUp}, {"DOWN", kDown}, {"LEFT", kLeft}, {"RIGHT", kRight},
    {"LSTICK", kStickL}, {"RSTICK", kStickR}, {"TV", kTV}};
const std::map<std::string, std::pair<int, float>> kAxisTargets = {
    {"LSTICK_UP", {1, 1.0f}}, {"LSTICK_DOWN", {1, -1.0f}}, {"LSTICK_LEFT", {0, -1.0f}}, {"LSTICK_RIGHT", {0, 1.0f}},
    {"RSTICK_UP", {3, 1.0f}}, {"RSTICK_DOWN", {3, -1.0f}}, {"RSTICK_LEFT", {2, -1.0f}}, {"RSTICK_RIGHT", {2, 1.0f}}};

// input.ini name of a target (lower case), "none" for an empty one
std::string target_name(const Target& t) {
    std::string n = "NONE";
    for (const auto& [name, mask] : kButtonTargets)
        if (t.axis < 0 && t.mask == mask) n = name;
    for (const auto& [name, a] : kAxisTargets)
        if (t.axis == a.first && t.dir == a.second) n = name;
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return n;
}

bool parse_target(std::string name, Target& t) {
    for (char& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const auto& buttons = kButtonTargets;
    const auto& axes = kAxisTargets;
    t = {};
    if (name == "NONE") return true;
    if (auto b = buttons.find(name); b != buttons.end()) {
        t.mask = b->second;
        return true;
    }
    if (auto a = axes.find(name); a != axes.end()) {
        t.axis = a->second.first;
        t.dir = a->second.second;
        return true;
    }
    return false;
}

// Mouse sources share the key map with codes past the scancode range.
enum : int { kMouseLeft = 1000, kMouseRight, kMouseMiddle, kMouseX1, kMouseX2, kWheelUp, kWheelDown };
// Controller sources: SDL_GamepadButton values, and the two triggers (pressed past half way).
enum : int { kPadLeftTrigger = 200, kPadRightTrigger };

struct Config {
    std::map<int, Target> bindings;  // SDL scancode or mouse source -> target
    std::map<int, uint32_t> pad;     // controller button / trigger -> GamePad button(s)
    bool pad_set = false;            // pad filled (defaults for the layout, or input.ini's [controller])
    bool nintendo_layout = true;     // face buttons by position (Wii U layout; the default, user 2026-10-28) or by Xbox label
    float deadzone = 0.15f;
    float mouse_sensitivity = 0.05f;  // stick deflection per pixel moved between two game reads
    bool invert_y = false;
    int wheel_frames = 3;  // reads a wheel notch holds its button
};

struct Pad {
    SDL_Gamepad* pad = nullptr;
    uint32_t buttons = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0, zl = 0, zr = 0;
    bool touch = false;
    float tx = 0, ty = 0;
};

std::mutex g_m;
Config g_cfg;
std::filesystem::path g_ini;  // input.ini in the data folder (save_bindings)
uint32_t g_repeat_button = 0, g_repeat_reads = 0;  // repeat_held
std::chrono::steady_clock::time_point g_repeat_until;
bool g_sdl = false;
SDL_Window* g_window = nullptr;
bool g_captured = false;
std::set<int> g_down;  // held keys / mouse buttons
std::map<int, int> g_pulses;  // wheel: source -> reads left
float g_mouse_dx = 0, g_mouse_dy = 0, g_prev_rx = 0, g_prev_ry = 0;
std::map<SDL_JoystickID, Pad> g_pads;
float g_rumble = 0;
uint32_t g_rumble_ms = 0;
bool g_rumble_pending = false;
// text entry (begin_text): state, and SDL calls queued for the main thread
TextEntry g_text;
size_t g_text_max = 0;
bool g_text_start = false, g_text_stop = false, g_title_dirty = false;
std::string g_base_title, g_title_status;  // window title at set_window, and set_title_status's text
// set_suspended: the launcher overlay owns input. Buttons still held when it ends stay masked until released.
bool g_suspended = false;
bool g_soft_keyboard = false;  // set_soft_keyboard: the launcher's keyboard takes the controller and the title
uint32_t g_masked = 0;

std::string utf8(const std::u16string& s) {
    std::string out;
    for (char16_t c : s) {
        const uint32_t u = c;
        if (u < 0x80) out += static_cast<char>(u);
        else if (u < 0x800) out += {static_cast<char>(0xC0 | (u >> 6)), static_cast<char>(0x80 | (u & 0x3F))};
        else out += {static_cast<char>(0xE0 | (u >> 12)), static_cast<char>(0x80 | ((u >> 6) & 0x3F)), static_cast<char>(0x80 | (u & 0x3F))};
    }
    return out;
}

// UTF-8 -> UTF-16 (characters outside the BMP are dropped: the game's fonts have none)
std::u16string utf16(const char* s) {
    std::u16string out;
    for (const auto* p = reinterpret_cast<const unsigned char*>(s); *p;) {
        uint32_t u = *p++;
        int extra = u >= 0xF0 ? 3 : u >= 0xE0 ? 2 : u >= 0xC0 ? 1 : 0;
        if (extra) u &= 0x3F >> extra;
        for (; extra && (*p & 0xC0) == 0x80; --extra) u = (u << 6) | (*p++ & 0x3F);
        if (u < 0x10000 && u >= 0x20) out += static_cast<char16_t>(u);
    }
    return out;
}

// Called with g_m held when the entry changes.
void text_changed() {
    ++g_text.revision;
    g_title_dirty = true;
}

void set_defaults(Config& c) {
    const auto bind = [&](int src, const char* target) {
        Target t;
        parse_target(target, t);
        c.bindings[src] = t;
    };
    bind(SDL_SCANCODE_W, "lstick_up");
    bind(SDL_SCANCODE_S, "lstick_down");
    bind(SDL_SCANCODE_A, "lstick_left");
    bind(SDL_SCANCODE_D, "lstick_right");
    bind(SDL_SCANCODE_I, "rstick_up");
    bind(SDL_SCANCODE_K, "rstick_down");
    bind(SDL_SCANCODE_J, "rstick_left");
    bind(SDL_SCANCODE_L, "rstick_right");
    bind(SDL_SCANCODE_UP, "up");
    bind(SDL_SCANCODE_DOWN, "down");
    bind(SDL_SCANCODE_LEFT, "left");
    bind(SDL_SCANCODE_RIGHT, "right");
    bind(SDL_SCANCODE_RETURN, "a");
    bind(SDL_SCANCODE_BACKSPACE, "b");
    bind(SDL_SCANCODE_SPACE, "b");
    bind(SDL_SCANCODE_F, "a");
    bind(SDL_SCANCODE_E, "y");
    bind(SDL_SCANCODE_LSHIFT, "r");
    bind(SDL_SCANCODE_Q, "l");
    bind(SDL_SCANCODE_Z, "zl");
    bind(SDL_SCANCODE_C, "zr");
    bind(SDL_SCANCODE_TAB, "plus");
    bind(SDL_SCANCODE_M, "minus");
    bind(SDL_SCANCODE_V, "lstick");
    bind(kMouseLeft, "x");
    bind(kMouseRight, "a");
    bind(kMouseMiddle, "rstick");
    bind(kMouseX1, "zl");
    bind(kMouseX2, "zr");
    bind(kWheelUp, "left");
    bind(kWheelDown, "right");
}

// Controller defaults (the user's own mapping, made the default 2026-10-28): by position as on the GamePad (bottom =
// B; the default) or by Xbox label (A bottom = Wii U A, so PS Cross confirms); Back = +, Start = -.
void default_pad(Config& c) {
    const bool nin = c.nintendo_layout;
    c.pad = {{SDL_GAMEPAD_BUTTON_SOUTH, nin ? kB : kA},  {SDL_GAMEPAD_BUTTON_EAST, nin ? kA : kB},
             {SDL_GAMEPAD_BUTTON_WEST, nin ? kY : kX},   {SDL_GAMEPAD_BUTTON_NORTH, nin ? kX : kY},
             {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kL},     {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kR},
             {kPadLeftTrigger, kZL},                     {kPadRightTrigger, kZR},
             {SDL_GAMEPAD_BUTTON_START, kMinus},         {SDL_GAMEPAD_BUTTON_BACK, kPlus},
             {SDL_GAMEPAD_BUTTON_LEFT_STICK, kStickL},   {SDL_GAMEPAD_BUTTON_RIGHT_STICK, kStickR},
             {SDL_GAMEPAD_BUTTON_DPAD_UP, kUp},          {SDL_GAMEPAD_BUTTON_DPAD_DOWN, kDown},
             {SDL_GAMEPAD_BUTTON_DPAD_LEFT, kLeft},      {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kRight}};
    c.pad_set = true;
}

// input.ini names of controller sources: SDL's ("a", "leftshoulder", ...) and the triggers' axis names
std::string pad_source_name(int src) {
    if (src == kPadLeftTrigger) return "lefttrigger";
    if (src == kPadRightTrigger) return "righttrigger";
    const char* n = SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(src));
    return n ? n : "";
}
int parse_pad_source(const std::string& name) {
    if (name == "lefttrigger") return kPadLeftTrigger;
    if (name == "righttrigger") return kPadRightTrigger;
    const SDL_GamepadButton b = SDL_GetGamepadButtonFromString(name.c_str());
    return b == SDL_GAMEPAD_BUTTON_INVALID ? -1 : static_cast<int>(b);
}

std::string trim(std::string s) {
    const auto sp = [](unsigned char ch) { return std::isspace(ch) != 0; };
    while (!s.empty() && sp(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t i = 0;
    while (i < s.size() && sp(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

int parse_source(const std::string& name) {
    std::string n = name;
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const std::map<std::string, int> mouse = {{"mouse_left", kMouseLeft}, {"mouse_right", kMouseRight},
                                                     {"mouse_middle", kMouseMiddle}, {"mouse_x1", kMouseX1},
                                                     {"mouse_x2", kMouseX2}, {"wheel_up", kWheelUp},
                                                     {"wheel_down", kWheelDown}};
    if (auto m = mouse.find(n); m != mouse.end()) return m->second;
    const SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
    return sc == SDL_SCANCODE_UNKNOWN ? -1 : static_cast<int>(sc);
}

// input.ini: [gamepad] layout = xbox|nintendo, deadzone; [mouse] sensitivity, invert_y, wheel_frames;
// [bindings] <SDL key name | mouse_left/right/middle/x1/x2 | wheel_up/down> = <target | none>;
// [controller] <SDL gamepad button name | lefttrigger | righttrigger> = <button target | none>.
// Each binding section changes the defaults line by line, or replaces them when it starts with "clear = 1" (what the
// launcher's CONTROLS page writes).
void load_ini(const std::filesystem::path& path, Config& c) {
    std::ifstream f(path);
    if (!f) return;
    std::string section, line;
    int n = 0;
    while (std::getline(f, line)) {
        ++n;
        if (const size_t h = line.find_first_of(";#"); h != std::string::npos) line = line.substr(0, h);
        line = trim(line);
        if (line.empty()) continue;
        if (line.front() == '[') {
            section = trim(line.substr(1, line.find(']') - 1));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        bool ok = true;
        if (section == "gamepad" && key == "layout") c.nintendo_layout = value == "nintendo";
        else if (section == "gamepad" && key == "deadzone") c.deadzone = std::stof(value);
        else if (section == "mouse" && key == "sensitivity") c.mouse_sensitivity = std::stof(value);
        else if (section == "mouse" && key == "invert_y") c.invert_y = value == "1" || value == "true";
        else if (section == "mouse" && key == "wheel_frames") c.wheel_frames = std::stoi(value);
        else if ((section == "bindings" || section == "controller") && key == "clear") {
            if (section == "bindings") c.bindings.clear();
            else c.pad.clear(), c.pad_set = true;
        } else if (section == "bindings") {
            const int src = parse_source(key);
            Target t;
            ok = src >= 0 && parse_target(value, t);
            if (ok) c.bindings[src] = t;
        } else if (section == "controller") {
            if (!c.pad_set) default_pad(c);
            const int src = parse_pad_source(key);
            Target t;
            ok = src >= 0 && parse_target(value, t) && t.axis < 0;
            if (ok && is_settings_button(src)) c.pad.erase(src);  // an older input.ini's "touchpad = tv"
            else if (ok && t.mask) c.pad[src] = t.mask;
            else if (ok) c.pad.erase(src);
        } else ok = false;
        if (!ok) std::fprintf(stderr, "%s:%d: ignored \"%s\"\n", path.string().c_str(), n, line.c_str());
    }
    std::printf("input: loaded %s\n", path.string().c_str());
}

float axis(Sint16 v) { return std::clamp(static_cast<float>(v) / 32767.0f, -1.0f, 1.0f); }

// Radial deadzone, rescaled so the stick still reaches 1.
void deadzone(float& x, float& y, float dz) {
    const float m = std::sqrt(x * x + y * y);
    if (m <= dz) {
        x = y = 0;
        return;
    }
    const float s = std::min(1.0f, (m - dz) / (1.0f - dz)) / m;
    x *= s;
    y *= s;
}

// Called with g_m held: the full state of one gamepad, after any of its events.
void refresh(Pad& p) {
    SDL_Gamepad* g = p.pad;
    p.zl = axis(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
    p.zr = axis(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    p.buttons = 0;
    for (const auto& [src, mask] : g_cfg.pad) {
        const bool down = src == kPadLeftTrigger    ? p.zl > 0.5f
                          : src == kPadRightTrigger ? p.zr > 0.5f
                                                    : SDL_GetGamepadButton(g, static_cast<SDL_GamepadButton>(src));
        if (down) p.buttons |= mask;
    }
    p.lx = axis(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX));
    p.ly = -axis(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY));
    p.rx = axis(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX));
    p.ry = -axis(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY));
    deadzone(p.lx, p.ly, g_cfg.deadzone);
    deadzone(p.rx, p.ry, g_cfg.deadzone);
}

void capture(bool on) {
    g_captured = on && g_window;
    if (g_window) SDL_SetWindowRelativeMouseMode(g_window, g_captured);
    g_mouse_dx = g_mouse_dy = 0;
}

// Called with g_m held. Opens a controller once (the same pad may be reported by "added" and by the scan in init()).
void open_pad(SDL_JoystickID id) {
    if (g_pads.count(id)) return;
    SDL_Gamepad* p = SDL_OpenGamepad(id);
    if (!p) {
        std::fprintf(stderr, "input: cannot open controller %u: %s\n", static_cast<unsigned>(id), SDL_GetError());
        return;
    }
    g_pads[id].pad = p;
    std::printf("input: gamepad connected: %s\n", SDL_GetGamepadName(p));
    SDL_SetGamepadSensorEnabled(p, SDL_SENSOR_GYRO, false);
    refresh(g_pads[id]);
}

void press(int src, bool down) {
    if (down) g_down.insert(src);
    else g_down.erase(src);
}

}  // namespace

void set_hints() {
    // PlayStation controllers through SDL's HIDAPI drivers (touchpad, rumble, DualSense over USB and Bluetooth);
    // must be set before any SDL subsystem starts (the setup screen starts one before init())
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
}

void init(const std::filesystem::path& data_dir, bool sdl) {
    std::lock_guard g(g_m);
    set_defaults(g_cfg);
    g_ini = data_dir / "input.ini";
    load_ini(g_ini, g_cfg);
    if (!g_cfg.pad_set) default_pad(g_cfg);
    if (sdl) {
        set_hints();
        g_sdl = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
        if (!g_sdl) std::fprintf(stderr, "input: SDL gamepad init failed: %s\n", SDL_GetError());
        // Controllers connected before this point may have announced themselves to an earlier SDL user (the setup
        // screen) already: open whatever is connected now rather than waiting for "added" events.
        int n = 0;
        if (SDL_JoystickID* ids = SDL_GetGamepads(&n)) {
            for (int i = 0; i < n; ++i) open_pad(ids[i]);
            SDL_free(ids);
        }
        if (!n) std::printf("input: no controller connected (keyboard + mouse)\n");
    }
}

void set_window(void* w) {
    std::lock_guard g(g_m);
    g_window = static_cast<SDL_Window*>(w);
    const char* t = g_window ? SDL_GetWindowTitle(g_window) : nullptr;
    g_base_title = t ? t : "";
}

void set_title_status(const std::string& status) {
    std::lock_guard g(g_m);
    if (status == g_title_status) return;
    g_title_status = status;
    g_title_dirty = true;
}

void handle_event(const void* ev) {
    const SDL_Event& e = *static_cast<const SDL_Event*>(ev);
    std::lock_guard g(g_m);
    if (g_text.active && !g_text.done && !g_text.cancelled) {
        if (e.type == SDL_EVENT_TEXT_INPUT) {
            for (char16_t c : utf16(e.text.text))
                if (!g_text_max || g_text.text.size() < g_text_max) g_text.text += c;
            text_changed();
            return;
        }
        if (e.type == SDL_EVENT_KEY_DOWN) {
            if (e.key.scancode == SDL_SCANCODE_BACKSPACE && !g_text.text.empty()) {
                g_text.text.pop_back();
                text_changed();
            } else if (e.key.scancode == SDL_SCANCODE_RETURN || e.key.scancode == SDL_SCANCODE_KP_ENTER) {
                g_text.done = true;
                text_changed();
            } else if (e.key.scancode == SDL_SCANCODE_ESCAPE) {
                g_text.cancelled = true;
                text_changed();
            }
            return;  // no key bindings while typing
        }
        if (e.type == SDL_EVENT_KEY_UP) return;
        // a connected controller confirms / cancels too: its A (by the configured layout) or Start = OK, B = cancel
        // (unless the on-screen keyboard is up: there A types and B deletes)
        if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && !g_soft_keyboard) {
            const bool nin = g_cfg.nintendo_layout;
            const auto b = static_cast<SDL_GamepadButton>(e.gbutton.button);
            const SDL_GamepadButton ok = nin ? SDL_GAMEPAD_BUTTON_EAST : SDL_GAMEPAD_BUTTON_SOUTH;
            const SDL_GamepadButton cancel = nin ? SDL_GAMEPAD_BUTTON_SOUTH : SDL_GAMEPAD_BUTTON_EAST;
            if (b == ok || b == SDL_GAMEPAD_BUTTON_START) g_text.done = true;
            else if (b == cancel) g_text.cancelled = true;
            if (g_text.done || g_text.cancelled) text_changed();
        }
    }
    if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {  // the first presses go to the log: is the controller reaching us?
        static int logged = 0;
        if (logged++ < 12) {
            const auto it = g_cfg.pad.find(e.gbutton.button);
            std::printf("input: controller %u button %s -> game buttons %08X%s\n", static_cast<unsigned>(e.gbutton.which),
                        SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(e.gbutton.button)), it == g_cfg.pad.end() ? 0u : it->second,
                        g_suspended ? " (menu open: for the launcher)" : "");
        }
    }
    const bool pointer_or_key = e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                                e.type == SDL_EVENT_MOUSE_BUTTON_UP || e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_WHEEL;
    if (g_suspended && pointer_or_key) return;  // the overlay's (no mouse capture, no key bindings)
    switch (e.type) {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            if (e.key.repeat) break;
            if (e.key.scancode == SDL_SCANCODE_ESCAPE) {
                if (e.type == SDL_EVENT_KEY_DOWN && g_captured) capture(false);
                break;
            }
            press(e.key.scancode, e.type == SDL_EVENT_KEY_DOWN);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            const bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            if (down && !g_captured && e.button.button == SDL_BUTTON_LEFT) {
                capture(true);  // the click that captures the mouse is not passed to the game
                break;
            }
            if (!g_captured) break;
            const int src = e.button.button == SDL_BUTTON_LEFT ? kMouseLeft : e.button.button == SDL_BUTTON_RIGHT ? kMouseRight
                          : e.button.button == SDL_BUTTON_MIDDLE ? kMouseMiddle : e.button.button == SDL_BUTTON_X1 ? kMouseX1
                                                                                                                : kMouseX2;
            press(src, down);
            break;
        }
        case SDL_EVENT_MOUSE_MOTION:
            if (g_captured) {
                g_mouse_dx += e.motion.xrel;
                g_mouse_dy += e.motion.yrel;
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            if (g_captured && e.wheel.y != 0) g_pulses[e.wheel.y > 0 ? kWheelUp : kWheelDown] = g_cfg.wheel_frames;
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            g_down.clear();
            capture(false);
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            open_pad(e.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (auto it = g_pads.find(e.gdevice.which); it != g_pads.end()) {
                SDL_CloseGamepad(it->second.pad);
                g_pads.erase(it);
            }
            break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            if (auto it = g_pads.find(e.gbutton.which); it != g_pads.end()) refresh(it->second);
            break;
        case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
        case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
        case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
            // DualShock 4 / DualSense touchpad (finger 0) -> GamePad touch screen
            if (auto it = g_pads.find(e.gtouchpad.which); it != g_pads.end() && e.gtouchpad.finger == 0) {
                it->second.touch = e.type != SDL_EVENT_GAMEPAD_TOUCHPAD_UP;
                it->second.tx = e.gtouchpad.x;
                it->second.ty = e.gtouchpad.y;
            }
            break;
        default:
            break;
    }
}

void update() {
    std::lock_guard g(g_m);
    if (g_window) {
        if (g_text_start) {
            g_text_start = false;
            SDL_StartTextInput(g_window);
            g_title_dirty = true;
        }
        if (g_text_stop) {
            g_text_stop = false;
            SDL_StopTextInput(g_window);
            g_title_dirty = true;
        }
        if (g_title_dirty) {
            g_title_dirty = false;
            std::string title;
            if (g_text.active && !g_soft_keyboard) {
                const std::string state = g_text.done ? "  [confirmed]" : g_text.cancelled ? "  [cancelled]" : "_   (Enter or A = OK, Esc or B = cancel)";
                title = "Type: " + utf8(g_text.text) + state;
            } else {
                title = g_title_status.empty() ? g_base_title : g_base_title + "  |  " + g_title_status;
            }
            SDL_SetWindowTitle(g_window, title.c_str());
        }
    }
    if (!g_rumble_pending) return;
    g_rumble_pending = false;
    const Uint16 s = static_cast<Uint16>(std::clamp(g_rumble, 0.0f, 1.0f) * 65535.0f);
    for (auto& [id, p] : g_pads) SDL_RumbleGamepad(p.pad, s, s, g_rumble_ms);
}

void set_suspended(bool on) {
    std::lock_guard g(g_m);
    if (on == g_suspended) return;
    g_suspended = on;
    if (on) {
        g_down.clear();
        g_pulses.clear();
        capture(false);
        return;
    }
    g_masked = 0;
    for (const auto& [id, p] : g_pads) g_masked |= p.buttons;
}

PadState read() {
    std::lock_guard g(g_m);
    PadState s;
    if (g_suspended) return s;
    float axes[4] = {};
    for (int src : g_down) {
        auto it = g_cfg.bindings.find(src);
        if (it == g_cfg.bindings.end()) continue;
        if (it->second.axis >= 0) axes[it->second.axis] += it->second.dir;
        else s.buttons |= it->second.mask;
    }
    for (auto it = g_pulses.begin(); it != g_pulses.end();) {
        if (auto b = g_cfg.bindings.find(it->first); b != g_cfg.bindings.end()) {
            if (b->second.axis >= 0) axes[b->second.axis] += b->second.dir;
            else s.buttons |= b->second.mask;
        }
        it = --it->second <= 0 ? g_pulses.erase(it) : std::next(it);
    }
    // keys: digital stick, diagonals normalised
    for (int a = 0; a < 4; a += 2) {
        float& x = axes[a];
        float& y = axes[a + 1];
        x = std::clamp(x, -1.0f, 1.0f);
        y = std::clamp(y, -1.0f, 1.0f);
        if (x && y) {
            x *= 0.7071f;
            y *= 0.7071f;
        }
    }
    s.lx = axes[0], s.ly = axes[1], s.rx = axes[2], s.ry = axes[3];
    // mouse look: movement since the previous read, averaged with that read to smooth single-frame spikes
    if (g_captured) {
        const float rx = g_mouse_dx * g_cfg.mouse_sensitivity, ry = (g_cfg.invert_y ? 1.0f : -1.0f) * g_mouse_dy * g_cfg.mouse_sensitivity;
        const float mx = 0.5f * (rx + g_prev_rx), my = 0.5f * (ry + g_prev_ry);
        g_prev_rx = rx;
        g_prev_ry = ry;
        s.rx = std::clamp(s.rx + mx, -1.0f, 1.0f);
        s.ry = std::clamp(s.ry + my, -1.0f, 1.0f);
    }
    g_mouse_dx = g_mouse_dy = 0;
    // gamepads: buttons OR'ed, the stick with the larger deflection wins
    const auto merge = [](float& x, float& y, float px, float py) {
        if (px * px + py * py > x * x + y * y) x = px, y = py;
    };
    for (const auto& [id, p] : g_pads) {
        s.buttons |= p.buttons;
        merge(s.lx, s.ly, p.lx, p.ly);
        merge(s.rx, s.ry, p.rx, p.ry);
        if (p.touch) {
            s.touch = true;
            s.touch_x = static_cast<uint16_t>(std::clamp(p.tx, 0.0f, 1.0f) * 853.0f);
            s.touch_y = static_cast<uint16_t>(std::clamp(p.ty, 0.0f, 1.0f) * 479.0f);
        }
    }
    const auto emulate = [&](float x, float y, uint32_t l, uint32_t r, uint32_t u, uint32_t d) {
        if (x < -0.5f) s.buttons |= l;
        if (x > 0.5f) s.buttons |= r;
        if (y > 0.5f) s.buttons |= u;
        if (y < -0.5f) s.buttons |= d;
    };
    emulate(s.lx, s.ly, kLStickLeft, kLStickRight, kLStickUp, kLStickDown);
    emulate(s.rx, s.ry, kRStickLeft, kRStickRight, kRStickUp, kRStickDown);
    g_masked &= s.buttons;  // released since the overlay closed: no longer masked
    s.buttons &= ~g_masked;
    // hold to gather: the held button let go for 2 reads of every 8 while repeat_held's time lasts
    if (g_repeat_button && (s.buttons & g_repeat_button) && std::chrono::steady_clock::now() < g_repeat_until) {
        if (++g_repeat_reads % 8 >= 6) s.buttons &= ~g_repeat_button;
    } else {
        g_repeat_reads = 0;
    }
    return s;
}

void repeat_held(uint32_t button, uint32_t ms) {
    std::lock_guard g(g_m);
    g_repeat_button = button;
    g_repeat_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
}

void begin_text(const std::u16string& initial, size_t max_len) {
    std::lock_guard g(g_m);
    g_text = TextEntry{};
    g_text.active = true;
    g_text_max = max_len;
    g_text.text = max_len && initial.size() > max_len ? initial.substr(0, max_len) : initial;
    g_down.clear();  // keys held for the game are released; typing must not move the hunter
    capture(false);
    if (!g_window) {  // headless: scripted text, confirmed at once
        char* env = nullptr;
        size_t len = 0;
        std::u16string t = u"Hunter";
        if (_dupenv_s(&env, &len, "MH3U_TEXT_INPUT") == 0 && env) t = utf16(env);
        std::free(env);
        g_text.text = max_len && t.size() > max_len ? t.substr(0, max_len) : t;
        g_text.done = true;
        std::printf("input: text entry -> \"%s\"\n", utf8(g_text.text).c_str());
    } else {
        g_text_start = true;
        g_text_stop = false;
    }
    ++g_text.revision;
}

void end_text() {
    std::lock_guard g(g_m);
    if (!g_text.active) return;
    g_text.active = false;
    if (g_window) {
        g_text_stop = true;
        g_text_start = false;
    }
}

void set_soft_keyboard(bool on) {
    std::lock_guard g(g_m);
    g_soft_keyboard = on;
    g_title_dirty = true;
}

void set_text(const std::u16string& text) {
    std::lock_guard g(g_m);
    if (!g_text.active || g_text.done || g_text.cancelled) return;
    g_text.text = g_text_max && text.size() > g_text_max ? text.substr(0, g_text_max) : text;
    text_changed();
}

void finish_text(bool confirm) {
    std::lock_guard g(g_m);
    if (!g_text.active || g_text.done || g_text.cancelled) return;
    (confirm ? g_text.done : g_text.cancelled) = true;
    text_changed();
}

size_t text_max() {
    std::lock_guard g(g_m);
    return g_text_max;
}

TextEntry text_state() {
    std::lock_guard g(g_m);
    return g_text;
}

void rumble(float strength, uint32_t ms) {
    std::lock_guard g(g_m);
    g_rumble = strength;
    g_rumble_ms = ms;
    g_rumble_pending = true;  // applied on the main thread by update()
}

// ---- rebinding (the launcher's CONTROLS page) --------------------------------------------------------------------
namespace {

const char* kMouseNames[] = {"mouse_left", "mouse_right", "mouse_middle", "mouse_x1", "mouse_x2", "wheel_up", "wheel_down"};
const char* kMouseLabels[] = {"Left click", "Right click", "Middle click", "Mouse 4", "Mouse 5", "Wheel up", "Wheel down"};

std::string key_source_name(int src) {  // input.ini name
    if (src >= kMouseLeft && src <= kWheelDown) return kMouseNames[src - kMouseLeft];
    const char* n = SDL_GetScancodeName(static_cast<SDL_Scancode>(src));
    return n ? n : "";
}
std::string key_source_label(int src) {
    if (src >= kMouseLeft && src <= kWheelDown) return kMouseLabels[src - kMouseLeft];
    return key_source_name(src);
}
// controller buttons by their usual labels (Xbox / PlayStation)
std::string pad_source_label(int src) {
    switch (src) {
        case SDL_GAMEPAD_BUTTON_SOUTH: return "A / Cross";
        case SDL_GAMEPAD_BUTTON_EAST: return "B / Circle";
        case SDL_GAMEPAD_BUTTON_WEST: return "X / Square";
        case SDL_GAMEPAD_BUTTON_NORTH: return "Y / Triangle";
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "LB / L1";
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "RB / R1";
        case kPadLeftTrigger: return "LT / L2";
        case kPadRightTrigger: return "RT / R2";
        case SDL_GAMEPAD_BUTTON_START: return "Start / Options";
        case SDL_GAMEPAD_BUTTON_BACK: return "Back / Create";
        case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "L3";
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "R3";
        case SDL_GAMEPAD_BUTTON_DPAD_UP: return "D-pad up";
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "D-pad down";
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "D-pad left";
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "D-pad right";
        case SDL_GAMEPAD_BUTTON_TOUCHPAD: return "Touchpad";
        case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1: return "Paddle 1";
        case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1: return "Paddle 2";
        case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2: return "Paddle 3";
        case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2: return "Paddle 4";
        default: return pad_source_name(src);
    }
}

std::string join(const std::vector<std::string>& v) {
    std::string out;
    for (const std::string& s : v) out += (out.empty() ? "" : ", ") + s;
    return out;
}

}  // namespace

const std::vector<BindingRow>& binding_rows() {
    static const std::vector<BindingRow> rows = {
        {"a", "A", true},
        {"b", "B", true},
        {"x", "X", true},
        {"y", "Y", true},
        {"l", "L", true},
        {"r", "R", true},
        {"zl", "ZL", true},
        {"zr", "ZR", true},
        {"plus", "+ (Start)", true},
        {"minus", "- (Select)", true},
        {"lstick", "Left stick click", true},
        {"rstick", "Right stick click", true},
        {"up", "D-pad up", true},
        {"down", "D-pad down", true},
        {"left", "D-pad left", true},
        {"right", "D-pad right", true},
        {"lstick_up", "Move up", false},
        {"lstick_down", "Move down", false},
        {"lstick_left", "Move left", false},
        {"lstick_right", "Move right", false},
        {"rstick_up", "Camera up", false},
        {"rstick_down", "Camera down", false},
        {"rstick_left", "Camera left", false},
        {"rstick_right", "Camera right", false},
    };
    return rows;
}

std::string key_names(const std::string& target) {
    std::lock_guard g(g_m);
    std::vector<std::string> names;
    for (const auto& [src, t] : g_cfg.bindings)
        if (target_name(t) == target) names.push_back(key_source_label(src));
    return join(names);
}

std::string pad_names(const std::string& target) {
    Target t;
    if (!parse_target(target, t)) return {};
    if (t.axis >= 0) return t.axis < 2 ? "Left stick" : "Right stick";
    std::lock_guard g(g_m);
    std::vector<std::string> names;
    for (const auto& [src, mask] : g_cfg.pad)
        if (mask == t.mask) names.push_back(pad_source_label(src));
    return join(names);
}

bool bind_from_event(const std::string& target, bool pad, const void* ev) {
    const SDL_Event& e = *static_cast<const SDL_Event*>(ev);
    Target t;
    if (!parse_target(target, t)) return false;
    int src = -1;
    if (!pad) {
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) src = e.key.scancode;
        else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
            src = e.button.button == SDL_BUTTON_LEFT ? kMouseLeft : e.button.button == SDL_BUTTON_RIGHT ? kMouseRight
                : e.button.button == SDL_BUTTON_MIDDLE ? kMouseMiddle : e.button.button == SDL_BUTTON_X1 ? kMouseX1 : kMouseX2;
        else if (e.type == SDL_EVENT_MOUSE_WHEEL && e.wheel.y != 0) src = e.wheel.y > 0 ? kWheelUp : kWheelDown;
    } else if (t.axis < 0) {
        if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && !is_settings_button(e.gbutton.button)) src = e.gbutton.button;
        else if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && e.gaxis.value > 16384 &&
                 (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))
            src = e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? kPadLeftTrigger : kPadRightTrigger;
    }
    if (src < 0) return false;
    std::lock_guard g(g_m);
    // one source per target: the old ones of this target go, and the new source leaves its old target
    if (!pad) {
        for (auto it = g_cfg.bindings.begin(); it != g_cfg.bindings.end();)
            it = target_name(it->second) == target ? g_cfg.bindings.erase(it) : std::next(it);
        g_cfg.bindings[src] = t;
    } else {
        for (auto it = g_cfg.pad.begin(); it != g_cfg.pad.end();) it = it->second == t.mask ? g_cfg.pad.erase(it) : std::next(it);
        g_cfg.pad[src] = t.mask;
    }
    return true;
}

void clear_binding(const std::string& target, bool pad) {
    Target t;
    if (!parse_target(target, t)) return;
    std::lock_guard g(g_m);
    if (!pad) {
        for (auto it = g_cfg.bindings.begin(); it != g_cfg.bindings.end();)
            it = target_name(it->second) == target ? g_cfg.bindings.erase(it) : std::next(it);
    } else if (t.axis < 0) {
        for (auto it = g_cfg.pad.begin(); it != g_cfg.pad.end();) it = it->second == t.mask ? g_cfg.pad.erase(it) : std::next(it);
    }
}

uint32_t pad_buttons_for(int sdl_gamepad_button) {
    std::lock_guard g(g_m);
    const auto it = g_cfg.pad.find(sdl_gamepad_button);
    return it == g_cfg.pad.end() ? 0 : it->second;
}

bool is_settings_button(int b) {
    return b == SDL_GAMEPAD_BUTTON_TOUCHPAD || b == SDL_GAMEPAD_BUTTON_GUIDE || b == SDL_GAMEPAD_BUTTON_MISC1;
}

std::string pad_prompt(const std::string& target) {
    Target t;
    if (!parse_target(target, t) || t.axis >= 0) return {};
    std::lock_guard g(g_m);
    if (g_pads.empty()) return {};
    const SDL_GamepadType type = SDL_GetGamepadType(g_pads.begin()->second.pad);
    const bool ps = type == SDL_GAMEPAD_TYPE_PS3 || type == SDL_GAMEPAD_TYPE_PS4 || type == SDL_GAMEPAD_TYPE_PS5;
    const bool nintendo = type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO || type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR;
    for (const auto& [src, mask] : g_cfg.pad) {
        if (mask != t.mask) continue;
        if (src == kPadLeftTrigger) return ps ? "L2" : nintendo ? "ZL" : "LT";
        if (src == kPadRightTrigger) return ps ? "R2" : nintendo ? "ZR" : "RT";
        switch (SDL_GetGamepadButtonLabelForType(type, static_cast<SDL_GamepadButton>(src))) {
            case SDL_GAMEPAD_BUTTON_LABEL_A: return "A";
            case SDL_GAMEPAD_BUTTON_LABEL_B: return "B";
            case SDL_GAMEPAD_BUTTON_LABEL_X: return "X";
            case SDL_GAMEPAD_BUTTON_LABEL_Y: return "Y";
            case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return "Cross";
            case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return "Circle";
            case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return "Square";
            case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return "Triangle";
            default: break;
        }
        switch (src) {
            case SDL_GAMEPAD_BUTTON_START: return ps ? "Options" : nintendo ? "+" : "Menu";
            case SDL_GAMEPAD_BUTTON_BACK: return ps ? "Create" : nintendo ? "-" : "View";
            case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ps ? "L1" : nintendo ? "L" : "LB";
            case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ps ? "R1" : nintendo ? "R" : "RB";
            case SDL_GAMEPAD_BUTTON_LEFT_STICK: return ps ? "L3" : "LS";
            case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return ps ? "R3" : "RS";
            default: return pad_source_label(src);
        }
    }
    return {};
}

ControlOptions control_options() {
    std::lock_guard g(g_m);
    return {g_cfg.nintendo_layout, g_cfg.mouse_sensitivity, g_cfg.invert_y};
}

void set_control_options(const ControlOptions& o) {
    std::lock_guard g(g_m);
    const bool relayout = o.nintendo_layout != g_cfg.nintendo_layout;
    g_cfg.nintendo_layout = o.nintendo_layout;
    g_cfg.mouse_sensitivity = o.mouse_sensitivity;
    g_cfg.invert_y = o.invert_y;
    if (relayout) default_pad(g_cfg);  // the controller defaults follow the layout
}

void reset_bindings() {
    std::lock_guard g(g_m);
    g_cfg.bindings.clear();
    set_defaults(g_cfg);
    default_pad(g_cfg);
}

bool save_bindings() {
    std::lock_guard g(g_m);
    if (g_ini.empty()) return false;
    std::ofstream f(g_ini, std::ios::trunc);
    f << "# Cha-Cha controls, written by the launcher (Settings > Controls). See docs/input.md.\n"
      << "[gamepad]\nlayout = " << (g_cfg.nintendo_layout ? "nintendo" : "xbox") << "\ndeadzone = " << g_cfg.deadzone << "\n\n"
      << "[mouse]\nsensitivity = " << g_cfg.mouse_sensitivity << "\ninvert_y = " << (g_cfg.invert_y ? 1 : 0)
      << "\nwheel_frames = " << g_cfg.wheel_frames << "\n\n[bindings]\nclear = 1\n";
    for (const auto& [src, t] : g_cfg.bindings) f << key_source_name(src) << " = " << target_name(t) << '\n';
    f << "\n[controller]\nclear = 1\n";
    for (const auto& [src, mask] : g_cfg.pad) {
        Target t;
        t.mask = mask;
        f << pad_source_name(src) << " = " << target_name(t) << '\n';
    }
    return static_cast<bool>(f);
}

}  // namespace mh3u::input
