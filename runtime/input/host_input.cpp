#include "host_input.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace mh3u::input {
namespace {

// A binding target: GamePad button(s) (mask; two or more for a key that presses them together, "x+a") or one
// direction of a stick axis (0 lx, 1 ly, 2 rx, 3 ry).
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
    if (t.axis < 0 && t.mask) {  // buttons together: their names joined with '+'
        std::string all;
        for (const auto& [name, mask] : kButtonTargets)
            if ((t.mask & mask) == mask) all += (all.empty() ? "" : "+") + name;
        n = all;
    }
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
    if (name.find('+') != std::string::npos) {  // buttons together ("X+A")
        for (size_t from = 0; from <= name.size();) {
            const size_t to = std::min(name.find('+', from), name.size());
            const auto b = buttons.find(name.substr(from, to - from));
            if (b == buttons.end()) return t = {}, false;
            t.mask |= b->second;
            from = to + 1;
        }
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
    float camera_speed = 1.0f;  // [camera] speed: applied to the game's camera steps (src/game/camera_speed.cpp)
    int wheel_frames = 3;  // reads a wheel notch holds its button
};

struct Pad {
    SDL_Gamepad* pad = nullptr;
    uint32_t buttons = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0, zl = 0, zr = 0;
    bool touch = false;
    float tx = 0, ty = 0;
    // Select + Options (Back + Start) together open the settings: when each went down, and the chord already fired
    // for this hold (both masked from the game until released)
    std::chrono::steady_clock::time_point back_at{}, start_at{};
    bool back = false, start = false, chord = false;
};
uint32_t g_prev_buttons = 0, g_edges = 0;  // note_buttons: the last buttons the game got, presses not yet taken
std::chrono::steady_clock::time_point g_edges_at{};  // the last new press: older ones are not handed out
bool g_chord = false;  // Select + Options pressed: take_settings_chord() hands it to the host once
constexpr auto kChordWait = std::chrono::milliseconds(80);  // a lone Select / Options waits this long for the other

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
bool g_cursor_hidden = false;
bool g_freed = false;  // the mouse was released with Esc / F11: its cursor stays visible until captured again
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
// Menu pointer (set_menu_open): while a game menu is open the mouse is a pointer - not captured, no mouse look; left
// click = A, right click = B, wheel = up / down. Switched on the main thread (update()); the capture comes back when the
// menu closes if the mouse was captured when it opened.
bool g_menu_open = false, g_pointer = false, g_recapture = false;
std::set<int> g_pointer_down;  // mouse buttons held in pointer mode (pointer_button gives their game buttons)
float g_ptr_x = -1, g_ptr_y = -1;  // the pointer in window coordinates
int g_win_w = 0, g_win_h = 0;        // the window's size in the same units (for pointer_position)
int g_clicks = 0;                    // left clicks queued for take_clicks
std::map<uint32_t, int> g_taps;      // tap: buttons -> reads left
SDL_Cursor* g_cursor = nullptr;  // the pointer's cursor (make_cursor), for this window height
int g_cursor_h = 0;
// The GamePad touch screen from the mouse (set_touch_view): while the GamePad image is shown, the left button held on
// it (with the mouse not captured for mouse look) touches the screen there, and dragging moves the touch.
int g_touch_view = 0;        // 0 not shown, 1 inset in the TV image's bottom-right corner, 2 instead of the TV image
bool g_mouse_touch = false;  // the left button went down on the GamePad image and is still held
float g_touch_u = 0, g_touch_v = 0;  // where, 0..1 across and down the GamePad screen
bool g_both = false;         // set_both_controllers

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
    // Laid out after Monster Hunter: World's PC defaults (Type 1), mapped onto MH3U's buttons (X attack, A special,
    // B roll, Y item, R dash / sheathe / guard, L camera, A talk / gather; user 2026-11-03): WASD move, O K L ; camera,
    // Shift dash, Space roll, left / right mouse the two attacks, mouse 4 both together, mouse 5 guard / weapon action,
    // E use item, F interact, Q and Ctrl the camera, wheel through the items. Kept from before: arrows = d-pad,
    // Return / Backspace confirm / back, Z / C = ZL / ZR, Tab = +, M = -, V and the middle button the stick clicks.
    bind(SDL_SCANCODE_W, "lstick_up");
    bind(SDL_SCANCODE_S, "lstick_down");
    bind(SDL_SCANCODE_A, "lstick_left");
    bind(SDL_SCANCODE_D, "lstick_right");
    bind(SDL_SCANCODE_O, "rstick_up");
    bind(SDL_SCANCODE_L, "rstick_down");
    bind(SDL_SCANCODE_K, "rstick_left");
    bind(SDL_SCANCODE_SEMICOLON, "rstick_right");
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
    bind(SDL_SCANCODE_LCTRL, "l");
    bind(SDL_SCANCODE_Z, "zl");
    bind(SDL_SCANCODE_C, "zr");
    bind(SDL_SCANCODE_TAB, "plus");
    bind(SDL_SCANCODE_M, "minus");
    bind(SDL_SCANCODE_V, "lstick");
    bind(kMouseLeft, "x");
    bind(kMouseRight, "a");
    bind(kMouseMiddle, "rstick");
    bind(kMouseX1, "a+x");
    bind(kMouseX2, "r");
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
    if (n.rfind("scancode_", 0) == 0) return std::atoi(n.c_str() + 9);  // a key whose name the file cannot hold
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
        else if (section == "camera" && key == "speed") c.camera_speed = std::clamp(std::stof(value), 0.1f, 1.0f);
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
    const auto now = std::chrono::steady_clock::now();
    const bool back = SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_BACK), start = SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_START);
    if (back && !p.back) p.back_at = now;
    if (start && !p.start) p.start_at = now;
    p.back = back;
    p.start = start;
    if (back && start && !p.chord) {  // the chord: settings, and neither button reaches the game until let go
        p.chord = true;
        g_chord = true;
        const auto mask_of = [](int src) {
            const auto it = g_cfg.pad.find(src);
            return it == g_cfg.pad.end() ? 0u : it->second;
        };
        g_masked |= mask_of(SDL_GAMEPAD_BUTTON_BACK) | mask_of(SDL_GAMEPAD_BUTTON_START);
    }
    if (!back && !start) p.chord = false;
    p.lx = axis(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX));
    p.ly = -axis(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY));
    p.rx = axis(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX));
    p.ry = -axis(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY));
    deadzone(p.lx, p.ly, g_cfg.deadzone);
    deadzone(p.rx, p.ry, g_cfg.deadzone);
}

// Pointer mode's fixed mouse buttons: right = B; the wheel moves the menu cursor up / down. Left clicks are queued
// (take_clicks): what they hit is the game side's to decide.
uint32_t pointer_button(int src) {
    return src == kMouseRight ? kB : src == kWheelUp ? kUp : src == kWheelDown ? kDown : 0;
}

// Called with g_m held: mouse buttons held across a mode change are dropped, so the click that closes a menu does not
// reach the game as a gameplay binding (or the other way round).
void drop_mouse_buttons() {
    for (int src = kMouseLeft; src <= kWheelDown; ++src) g_down.erase(src), g_pulses.erase(src);
    g_pointer_down.clear();
    g_clicks = 0;
}

// The menu pointer's cursor, drawn here in the style of the game's menus (no game art): an arrow with a dark brown
// outline, a thin gold rim, a cream fill shading to tan, and a soft shadow. 32 px tall at 1080 lines.
SDL_Cursor* make_cursor(int window_h) {
    const int size = std::clamp(window_h * 32 / 1080, 20, 96);
    const float s = static_cast<float>(size);
    // the arrow, in pixels (tip at the hot spot)
    const float pts[][2] = {{0.00f, 0.00f}, {0.00f, 0.80f}, {0.20f, 0.63f}, {0.33f, 0.93f}, {0.46f, 0.87f},
                            {0.33f, 0.58f}, {0.58f, 0.58f}};
    constexpr int n = 7;
    const float pad = std::max(2.0f, s / 12.0f);
    float px[n], py[n];
    for (int i = 0; i < n; ++i) px[i] = pad + pts[i][0] * s, py[i] = pad + pts[i][1] * s * 0.95f;
    const int w = static_cast<int>(pad * 2 + 0.6f * s) + 2, h = static_cast<int>(pad * 2 + s) + 2;
    // signed distance to the arrow (negative inside)
    const auto dist = [&](float x, float y) {
        float d = 1e9f;
        bool inside = false;
        for (int i = 0, j = n - 1; i < n; j = i++) {
            const float ax = px[j], ay = py[j], bx = px[i], by = py[i];
            const float ex = bx - ax, ey = by - ay;
            const float t = std::clamp(((x - ax) * ex + (y - ay) * ey) / (ex * ex + ey * ey), 0.0f, 1.0f);
            const float dx = x - (ax + ex * t), dy = y - (ay + ey * t);
            d = std::min(d, std::sqrt(dx * dx + dy * dy));
            if ((ay > y) != (by > y) && x < ax + (y - ay) * ex / ey) inside = !inside;
        }
        return inside ? -d : d;
    };
    SDL_Surface* surf = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    if (!surf) return nullptr;
    const float outline = std::max(1.5f, s / 14.0f), rim = std::max(1.0f, s / 24.0f), shadow = std::max(1.0f, s / 16.0f);
    const auto cover = [](float d) { return std::clamp(0.5f - d, 0.0f, 1.0f); };  // 1 px anti-aliasing
    for (int y = 0; y < h; ++y) {
        auto* row = static_cast<uint8_t*>(surf->pixels) + y * surf->pitch;
        for (int x = 0; x < w; ++x) {
            const float fx = x + 0.5f, fy = y + 0.5f;
            const float d = dist(fx, fy);
            // shadow, then outline (dark brown), rim (gold), fill (cream -> tan, top to bottom)
            float r = 0, g = 0, b = 0, a = 0.35f * cover(dist(fx - shadow, fy - shadow) - outline);
            const auto over = [&](float cr, float cg, float cb, float ca) {
                const float oa = ca + a * (1 - ca);
                if (oa <= 0) return;
                r = (cr * ca + r * a * (1 - ca)) / oa, g = (cg * ca + g * a * (1 - ca)) / oa, b = (cb * ca + b * a * (1 - ca)) / oa;
                a = oa;
            };
            over(0.23f, 0.14f, 0.06f, cover(d - outline));
            over(0.78f, 0.60f, 0.24f, cover(d + outline * 0.15f));
            const float t = std::clamp((fy - pad) / s, 0.0f, 1.0f);
            over(0.95f - 0.10f * t, 0.90f - 0.15f * t, 0.77f - 0.24f * t, cover(d + outline * 0.15f + rim));
            row[x * 4 + 0] = static_cast<uint8_t>(r * 255.0f + 0.5f);
            row[x * 4 + 1] = static_cast<uint8_t>(g * 255.0f + 0.5f);
            row[x * 4 + 2] = static_cast<uint8_t>(b * 255.0f + 0.5f);
            row[x * 4 + 3] = static_cast<uint8_t>(a * 255.0f + 0.5f);
        }
    }
    SDL_Cursor* c = SDL_CreateColorCursor(surf, static_cast<int>(px[0]), static_cast<int>(py[0]));
    SDL_DestroySurface(surf);
    return c;
}

// Called with g_m held. Window point -> place on the GamePad screen (0..1, not clamped); false when the GamePad image is
// not shown or the point is outside it. Placed as the renderer and presenter do: GamePad only = the 854x480 image
// fitted into the window, centred; inset = 30 % of the TV image's width, its aspect kept, 1/64 of the TV width from
// the TV image's right and bottom edges (VkRenderer::end_frame), the TV image being 16:9 fitted and centred.
bool touch_at(float x, float y, float& u, float& v) {
    if (!g_touch_view || g_win_w <= 0 || g_win_h <= 0) return false;
    float ix, iy, iw, ih;
    if (g_touch_view == 2) {
        const float scale = std::min(g_win_w / 854.0f, g_win_h / 480.0f);
        iw = 854.0f * scale, ih = 480.0f * scale;
        ix = (g_win_w - iw) * 0.5f, iy = (g_win_h - ih) * 0.5f;
    } else {
        const float scale = std::min(g_win_w / 16.0f, g_win_h / 9.0f);
        const float tw = 16.0f * scale, th = 9.0f * scale, tx = (g_win_w - tw) * 0.5f, ty = (g_win_h - th) * 0.5f;
        const float margin = tw / 64.0f;
        iw = tw * 0.3f, ih = iw * 480.0f / 854.0f;
        ix = tx + tw - margin - iw, iy = ty + th - margin - ih;
    }
    u = (x - ix) / iw;
    v = (y - iy) / ih;
    return u >= 0 && u <= 1 && v >= 0 && v <= 1;
}

void capture(bool on) {
    g_captured = on && g_window;
    if (g_captured) g_freed = false;
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
            // Esc, and F11 (the presenter also leaves fullscreen): the mouse is released, with its cursor shown
            if (e.key.scancode == SDL_SCANCODE_ESCAPE || e.key.scancode == SDL_SCANCODE_F11) {
                if (e.type == SDL_EVENT_KEY_DOWN && g_captured) capture(false);
                if (e.type == SDL_EVENT_KEY_DOWN) g_recapture = false;  // released on purpose: stays free after the menu
                if (e.type == SDL_EVENT_KEY_DOWN) g_freed = true;
                break;
            }
            press(e.key.scancode, e.type == SDL_EVENT_KEY_DOWN);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            const bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            // the GamePad touch screen: a left press on the shown GamePad image (mouse not captured), held to drag
            if (e.button.button == SDL_BUTTON_LEFT && !g_captured) {
                float u = 0, v = 0;
                g_ptr_x = e.button.x, g_ptr_y = e.button.y;
                if (down && touch_at(e.button.x, e.button.y, u, v)) {
                    g_mouse_touch = true;
                    g_touch_u = u, g_touch_v = v;
                    break;
                }
                if (!down && g_mouse_touch) {
                    g_mouse_touch = false;
                    break;
                }
                if (g_touch_view == 2) break;  // only the GamePad is shown: clicks never capture the mouse
            }
            if (g_pointer) {
                const int src = e.button.button == SDL_BUTTON_LEFT ? kMouseLeft : e.button.button == SDL_BUTTON_RIGHT ? kMouseRight : -1;
                if (src < 0) break;
                g_ptr_x = e.button.x, g_ptr_y = e.button.y;
                if (src == kMouseLeft) {
                    if (down) ++g_clicks;
                    break;
                }
                if (down) g_pointer_down.insert(src);
                else g_pointer_down.erase(src);
                break;
            }
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
            g_ptr_x = e.motion.x, g_ptr_y = e.motion.y;
            if (g_mouse_touch) {
                float u = 0, v = 0;
                touch_at(e.motion.x, e.motion.y, u, v);
                g_touch_u = std::clamp(u, 0.0f, 1.0f), g_touch_v = std::clamp(v, 0.0f, 1.0f);
            }
            if (g_captured) {
                g_mouse_dx += e.motion.xrel;
                g_mouse_dy += e.motion.yrel;
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            if ((g_captured || g_pointer) && e.wheel.y != 0) g_pulses[e.wheel.y > 0 ? kWheelUp : kWheelDown] = g_cfg.wheel_frames;
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            g_down.clear();
            g_mouse_touch = false;
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
    if (g_window) SDL_GetWindowSize(g_window, &g_win_w, &g_win_h);
    if (g_touch_view == 2 && g_captured) capture(false);  // only the GamePad is shown: the mouse is its touch pen
    if (g_window && g_menu_open != g_pointer && !g_suspended && !g_text.active) {
        g_pointer = g_menu_open;
        drop_mouse_buttons();
        if (g_pointer) {
            int ww = 0, wh = 0;
            SDL_GetWindowSize(g_window, &ww, &wh);
            if (!g_cursor || wh != g_cursor_h) {
                if (g_cursor) SDL_DestroyCursor(g_cursor);
                g_cursor = make_cursor(wh);
                g_cursor_h = wh;
            }
            g_recapture = g_captured;
            const bool was = g_captured;
            capture(false);
            if (was) {  // the pointer starts in the middle of the window
                int w = 0, h = 0;
                SDL_GetWindowSize(g_window, &w, &h);
                SDL_WarpMouseInWindow(g_window, w * 0.5f, h * 0.5f);
            }
        } else {
            SDL_SetCursor(SDL_GetDefaultCursor());
            if (g_recapture && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS)) capture(true);
        }
    }
    // no menu, no overlay, no text entry: the mouse is not a pointer, so no cursor over the game (user 2026-11-03) -
    // unless it was released with Esc / F11 (issue #8: to reach the window's close button)
    // (shown too while the GamePad image is up and the mouse is free: it is the touch pen)
    const bool hide = g_window && !g_pointer && !g_suspended && !g_text.active && !g_freed && !(g_touch_view && !g_captured) &&
                      (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS);
    if (hide != g_cursor_hidden) {
        if (hide) SDL_HideCursor();
        else SDL_ShowCursor();
        g_cursor_hidden = hide;
    }
    // the pointer's cursor (again after the launcher overlay, whose UI sets its own)
    if (g_window && g_pointer && !g_suspended && g_cursor && SDL_GetCursor() != g_cursor) SDL_SetCursor(g_cursor);
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
    for (int src : g_pointer_down) s.buttons |= pointer_button(src);
    for (auto it = g_taps.begin(); it != g_taps.end();) {
        s.buttons |= it->first;
        it = --it->second <= 0 ? g_taps.erase(it) : std::next(it);
    }
    for (auto it = g_pulses.begin(); it != g_pulses.end();) {
        if (g_pointer) s.buttons |= pointer_button(it->first);
        else if (auto b = g_cfg.bindings.find(it->first); b != g_cfg.bindings.end()) {
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
    if (g_mouse_touch) {
        s.touch = true;
        s.touch_x = static_cast<uint16_t>(g_touch_u * 853.0f);
        s.touch_y = static_cast<uint16_t>(g_touch_v * 479.0f);
    }
    for (const auto& [id, p] : g_pads) {
        uint32_t held_back = 0;  // a lone Select / Options just pressed: wait a moment for the other half of the chord
        const auto now = std::chrono::steady_clock::now();
        const auto mask_of = [](int src) {
            const auto it = g_cfg.pad.find(src);
            return it == g_cfg.pad.end() ? 0u : it->second;
        };
        if (p.back && !p.start && now - p.back_at < kChordWait) held_back |= mask_of(SDL_GAMEPAD_BUTTON_BACK);
        if (p.start && !p.back && now - p.start_at < kChordWait) held_back |= mask_of(SDL_GAMEPAD_BUTTON_START);
        s.buttons |= p.buttons & ~held_back;
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

PadState touch_state() {
    std::lock_guard g(g_m);
    PadState s;
    if (g_suspended) return s;
    if (g_mouse_touch) {
        s.touch = true;
        s.touch_x = static_cast<uint16_t>(g_touch_u * 853.0f);
        s.touch_y = static_cast<uint16_t>(g_touch_v * 479.0f);
    }
    for (const auto& [id, p] : g_pads)
        if (p.touch) {
            s.touch = true;
            s.touch_x = static_cast<uint16_t>(std::clamp(p.tx, 0.0f, 1.0f) * 853.0f);
            s.touch_y = static_cast<uint16_t>(std::clamp(p.ty, 0.0f, 1.0f) * 479.0f);
        }
    return s;
}

void set_touch_view(int mode) {
    std::lock_guard g(g_m);
    g_touch_view = mode;
    if (!mode) g_mouse_touch = false;
}

void set_both_controllers(bool on) {
    std::lock_guard g(g_m);
    g_both = on;
}
bool both_controllers() {
    std::lock_guard g(g_m);
    return g_both;
}

void set_menu_open(bool open) {
    std::lock_guard g(g_m);
    g_menu_open = open;
}

bool pointer_position(float& u, float& v) {
    std::lock_guard g(g_m);
    if (!g_pointer || g_win_w <= 0 || g_win_h <= 0 || g_ptr_x < 0) return false;
    // as the presenter places the TV image: 16:9 fitted inside the window, centred
    const float scale = std::min(g_win_w / 16.0f, g_win_h / 9.0f);
    const float w = 16.0f * scale, h = 9.0f * scale;
    u = (g_ptr_x - (g_win_w - w) * 0.5f) / w;
    v = (g_ptr_y - (g_win_h - h) * 0.5f) / h;
    return u >= 0 && u <= 1 && v >= 0 && v <= 1;
}

int take_clicks() {
    std::lock_guard g(g_m);
    const int n = g_clicks;
    g_clicks = 0;
    return n;
}

void tap(uint32_t buttons) {
    std::lock_guard g(g_m);
    g_taps[buttons] = 3;
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
    // ';' and '#' start a comment in input.ini and '=' splits its lines: such keys go by number
    if (n && std::strpbrk(n, ";#=")) return "scancode_" + std::to_string(src);
    return n ? n : "";
}
std::string key_source_label(int src) {
    if (src >= kMouseLeft && src <= kWheelDown) return kMouseLabels[src - kMouseLeft];
    const char* n = SDL_GetScancodeName(static_cast<SDL_Scancode>(src));  // the key's own name, ';' included
    return n ? n : "";
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
        {"a+x", "X + A together", true},
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

void note_buttons(uint32_t buttons) {
    std::lock_guard g(g_m);
    if (const uint32_t fresh = buttons & ~g_prev_buttons) {  // pressed since the last read: take_pressed
        g_edges |= fresh;
        g_edges_at = std::chrono::steady_clock::now();
    }
    g_prev_buttons = buttons;
}

bool take_pressed(uint32_t buttons) {
    std::lock_guard g(g_m);
    // only a press of the last moment: one made long before (+ for the pause menu) must not skip a later cutscene
    const bool hit = (g_edges & buttons) != 0 && std::chrono::steady_clock::now() - g_edges_at < std::chrono::milliseconds(300);
    g_edges &= ~buttons;
    return hit;
}

bool take_settings_chord() {
    std::lock_guard g(g_m);
    const bool c = g_chord;
    g_chord = false;
    return c;
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
    // the Nintendo layout names the buttons as printed on a Nintendo pad (right A, bottom B, top X, left Y; + -
    // L R ZL ZR), also on pads that report Xbox labels (third-party Nintendo-style pads)
    const bool nintendo = !ps && (g_cfg.nintendo_layout || type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO ||
                                  type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR);
    for (const auto& [src, mask] : g_cfg.pad) {
        if (mask != t.mask) continue;
        if (nintendo) {
            if (src == SDL_GAMEPAD_BUTTON_SOUTH) return "B";
            if (src == SDL_GAMEPAD_BUTTON_EAST) return "A";
            if (src == SDL_GAMEPAD_BUTTON_WEST) return "Y";
            if (src == SDL_GAMEPAD_BUTTON_NORTH) return "X";
        }
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
    return {g_cfg.nintendo_layout, g_cfg.mouse_sensitivity, g_cfg.invert_y, g_cfg.camera_speed};
}

void set_control_options(const ControlOptions& o) {
    std::lock_guard g(g_m);
    const bool relayout = o.nintendo_layout != g_cfg.nintendo_layout;
    g_cfg.nintendo_layout = o.nintendo_layout;
    g_cfg.mouse_sensitivity = o.mouse_sensitivity;
    g_cfg.invert_y = o.invert_y;
    g_cfg.camera_speed = std::clamp(o.camera_speed, 0.1f, 1.0f);
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
      << "\nwheel_frames = " << g_cfg.wheel_frames << "\n\n[camera]\nspeed = " << g_cfg.camera_speed
      << "\n\n[bindings]\nclear = 1\n";
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
