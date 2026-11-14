// Host input (SDL3) merged into one Wii U GamePad state: keyboard + mouse and any number of SDL gamepads (Xbox,
// PlayStation DualShock 4 / DualSense, Switch Pro, generic XInput/DirectInput pads – SDL maps them all to one
// layout). Events arrive on the host main thread (Presenter::pump_events); the guest reads snapshots from its own
// thread (VPADRead).
//
// Mouse look: while the mouse is captured (click in the window; Esc releases it) mouse movement becomes right-stick
// deflection, proportional to how far the mouse moved since the game's previous read - the game's camera turns at
// a rate set by the stick, so the mouse sets that rate.
//
// Bindings: built-in defaults, overridden by <repo>/input.ini (see docs/input.md).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mh3u::input {

// VPAD button bits (vpad/input.h)
enum : uint32_t {
    kSync = 0x0001, kHome = 0x0002, kMinus = 0x0004, kPlus = 0x0008, kR = 0x0010, kL = 0x0020, kZR = 0x0040,
    kZL = 0x0080, kDown = 0x0100, kUp = 0x0200, kRight = 0x0400, kLeft = 0x0800, kY = 0x1000, kX = 0x2000,
    kB = 0x4000, kA = 0x8000, kTV = 0x10000, kStickR = 0x20000, kStickL = 0x40000,
    // stick emulation (set from the stick positions)
    kRStickDown = 0x00800000, kRStickUp = 0x01000000, kRStickRight = 0x02000000, kRStickLeft = 0x04000000,
    kLStickDown = 0x08000000, kLStickUp = 0x10000000, kLStickRight = 0x20000000, kLStickLeft = 0x40000000,
};

struct PadState {
    uint32_t buttons = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0;  // -1..1, y up positive
    bool touch = false;
    uint16_t touch_x = 0, touch_y = 0;  // GamePad screen pixels (854x480)
};

// Loads bindings (input.ini in the data folder if present) and, if sdl is true, opens the gamepad subsystem and every
// controller already connected.
void init(const std::filesystem::path& data_dir, bool sdl);
// SDL hints for controllers; call before any SDL subsystem is started (init() calls it too).
void set_hints();
// Main thread: one SDL_Event (passed as void* to keep SDL out of this header), and the window for mouse capture.
void handle_event(const void* sdl_event);
void set_window(void* sdl_window);
// Main thread, every event-loop iteration: applies queued rumble and window-title changes.
void update();
// Main thread: status shown after the window's title (e.g. "30 FPS"); hidden while text entry owns the title.
void set_title_status(const std::string& status);
// Rebinding (the launcher's CONTROLS page). Targets are input.ini's names: "a", "zl", "lstick_up", ...
struct BindingRow {
    const char* target;
    const char* label;
    bool pad;  // a controller button can be bound to it (stick directions stay on the sticks)
};
const std::vector<BindingRow>& binding_rows();
// What is bound to a target, for display ("W, Up"; "" when nothing): keys / mouse, and controller buttons.
std::string key_names(const std::string& target);
std::string pad_names(const std::string& target);
// Binds the key / mouse button / wheel (pad false) or controller button / trigger (pad true) pressed in this
// SDL_Event to target, replacing the target's earlier binding; false if the event is not such a press. Main thread.
bool bind_from_event(const std::string& target, bool pad, const void* sdl_event);
void clear_binding(const std::string& target, bool pad);
// The GamePad buttons (VPAD bits) a controller button is bound to (SDL_GamepadButton value), so menus outside the game
// answer the same buttons as the game does. 0 if unbound.
uint32_t pad_buttons_for(int sdl_gamepad_button);
// The controller buttons that open / close the settings over the game: the touchpad click (PlayStation), Home / guide
// and misc (mic, Share, Capture). The game uses none of them, so they are never bound to a game button.
bool is_settings_button(int sdl_gamepad_button);
// Select + Options (Back + Start, View + Menu, Share + Options) pressed together since the last call: opens / closes the
// settings over the game. Neither button reaches the game during the chord; a lone press waits 80 ms for the other.
// Main thread, after handle_event.
bool take_settings_chord();
// Short name of the controller button bound to a game button, as printed on the connected controller ("Cross",
// "Circle", "A", "Options", "LB", ...); "" if none is bound or no controller is connected. For on-screen prompts.
std::string pad_prompt(const std::string& target);
struct ControlOptions {
    bool nintendo_layout = true;  // face buttons by position (the default; changing it resets the controller bindings)
    float mouse_sensitivity = 0.05f;
    bool invert_y = false;
    float camera_speed = 1.0f;  // 0.3 .. 1.0: the game's camera turn / tilt steps scaled by it (game::set_camera_speed)
};
ControlOptions control_options();
void set_control_options(const ControlOptions& options);
void reset_bindings();  // the built-in defaults
// The controller and the keyboard / mouse on their own (Controller Settings / Keyboard Settings each reset only theirs):
// the controller back to the Nintendo layout's buttons, the keys / mouse back to Cha-Cha's defaults.
void reset_pad_bindings();
void reset_key_bindings();
// The controller buttons are the Nintendo layout's defaults (else a custom mapping: "Custom" in Controller Settings).
bool pad_is_default();
bool save_bindings();   // input.ini in the data folder

// Main thread: while suspended (the launcher overlay is up) the game reads no input and the mouse is never captured;
// controller buttons held when it ends are ignored until released (the press that chose PLAY).
void set_suspended(bool on);
// Guest thread: the merged state for this read; consumes accumulated mouse movement and wheel pulses.
PadState read();
// Guest thread: only the GamePad touch screen of this read (mouse on the shown GamePad image, PlayStation touchpad);
// consumes nothing.
PadState touch_state();
// How the GamePad image is shown (gx2 cycle_gamepad_view: 0 not, 1 inset, 2 instead of the TV image): while it is,
// the left mouse button held on it touches the GamePad screen there (when the mouse is not captured for mouse look;
// with the GamePad image alone, clicks never capture it). Any thread.
void set_touch_view(int mode);
// Whether the game sees both a Pro Controller and a GamePad driven by host input (MH3U_CONTROLLER=both, the default),
// so either choice in the game's Controller Settings works. Set by runtime/os/input.cpp. Any thread.
void set_both_controllers(bool on);
bool both_controllers();
// Hold to gather (src/game/carving.cpp, after each item obtained): while `button` (VPAD bits) stays held, the game
// sees it released for 2 reads out of every 8 - pressed again and again, as when mashing it - until `ms` after this
// call. Any thread.
void repeat_held(uint32_t button, uint32_t ms);
// Guest thread, once a frame (src/game/menu_mouse.cpp): whether one of the game's menus is open. While one is, the
// mouse is a pointer (a cursor in the game's style; right click = B, wheel = up / down) and left clicks are queued for
// the game side (take_clicks), which decides what they hit.
void set_menu_open(bool open);
// Direct mouse look (the Modern Camera mod, src/game/modern_camera.cpp; guest thread, each camera update): while on,
// captured mouse movement no longer deflects the right stick; take_look returns it - degrees in right-stick directions
// (x right, y up; Mouse Sensitivity and Invert Camera Y applied) - since the last call.
void set_direct_look(bool on);
void take_look(float& x, float& y);
// The pointer on the TV image, 0..1 across and down (the image is fitted into the window, centred); false when the
// mouse is not a pointer now or is outside the image.
bool pointer_position(float& u, float& v);
// Left clicks in pointer mode since the last call.
int take_clicks();
// Presses buttons (VPAD bits) for the next 3 reads, as a click does; d-pad directions alone for the next read only
// (one step: held longer, some menus step twice).
void tap(uint32_t buttons);
// The buttons (VPAD bits) the game was handed this read, scripted presses included (runtime/os/input.cpp).
void note_buttons(uint32_t buttons);
// True when one of `buttons` (VPAD bits) was newly pressed in the game's input within the last 300 ms and not taken
// yet (a press, not a hold). Any thread.
bool take_pressed(uint32_t buttons);
// VPADControlMotor: rumble on every connected gamepad.
void rumble(float strength, uint32_t ms);

// Text entry from the PC keyboard, used instead of the Wii U software keyboard (swkbd). While active, typed text
// (SDL text input) edits the string, Backspace deletes, Enter confirms, Esc cancels – and on a connected controller
// A (by the configured layout) or Start confirms, B cancels; key bindings are suspended and the window title shows
// the text. Without a window (--headless) the text is MH3U_TEXT_INPUT, or "Hunter", and is
// confirmed at once.
struct TextEntry {
    bool active = false, done = false, cancelled = false;
    std::u16string text;
    uint32_t revision = 0;  // bumped on every change of text
};
// Any thread: start / stop an entry (max_len in UTF-16 units; 0 = unlimited).
void begin_text(const std::u16string& initial, size_t max_len);
void end_text();
TextEntry text_state();
// The on-screen keyboard (src/game/soft_keyboard.cpp, any thread): while on, the controller and the window title belong
// to it; it edits the text and confirms / cancels through these. The physical keyboard keeps typing into the same text.
void set_soft_keyboard(bool on);
// Guest thread, every pad read (VPADRead / KPADReadEx, after the input script): its buttons (VPAD bits) are kept for the
// on-screen keyboard (last_buttons), and while the keyboard is on the game reads an idle pad instead.
void pass_read(PadState& state);
uint32_t last_buttons();
void set_text(const std::u16string& text);
void finish_text(bool confirm);
size_t text_max();  // the game's length limit (0 = none)

}  // namespace mh3u::input
