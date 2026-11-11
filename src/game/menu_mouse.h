// Menu mouse (menu_mouse.cpp): the mouse is a pointer while one of the game's menus is open. Always on.
#pragma once

#include <cstdint>
#include <functional>

#include "ppc_context.h"

namespace mh3u::game {

// Host input's side of the pointer (runtime/input).
struct Pointer {
    std::function<void(bool)> set_menu_open;         // once a frame: whether a menu is open
    std::function<bool(float&, float&)> position;    // 0..1 across / down the TV image; false if none
    std::function<int()> take_clicks;                 // left clicks since the last call
    std::function<void(uint32_t)> tap;                // presses VPAD buttons for a few reads
};
void register_menu_mouse(Pointer pointer);

// For menus with hover (src/game, guest thread, from their draw): the pointer in TV pixels (1920 x 1080); false when
// the mouse is not a pointer or is off the picture. Calling it (either way) tells menu_mouse that this menu handles
// left clicks itself for the next frames; menus that never call it get A for a click.
bool menu_pointer(float& x, float& y);
// One left click for such a menu (true once per click; check after menu_pointer).
bool menu_click();
// A, as a click on an item gives (the menu moves its cursor to the item first).
void menu_confirm();

// The menu pointer's hover, as on the item pouch (user rule 2026-11-06, for every menu Cha-Cha adds): when the pointer
// moves onto an item the cursor goes straight to it (the menu's own move, with its sound); a pointer resting on an
// item does nothing, so keys and the d-pad keep moving the cursor; a click on another item goes there, then chooses.
struct HoverWalk {
    int target = -1;       // the item the cursor walks to
    bool confirm = false;  // a click: choose it on arrival
    float last_x = -1, last_y = -1;
    // Once a frame with the pointer and the item under it (-1: none): how far to move the cursor now (target -
    // cursor; 0 for nothing).
    int step(float x, float y, int hit, int cursor);
    // A click on hit (-1: beside the items).
    void click(int hit);
    // The click's item reached this frame: choose it (once).
    bool arrived(int cursor);
    void reset() { target = -1, confirm = false; }
};

// The field menu's extra tab (field_gfx.cpp): called once a frame right after the pad update, before the menus read
// the pad; it may change the pad words. Returns true while it owns the field menu's list (its own hover and clicks).
void set_field_menu_hook(std::function<bool(ppc::Context& ctx, uint8_t* base)> hook);

// A Cha-Cha overlay over whatever the game shows (the on-screen keyboard, soft_keyboard.cpp): `active` is asked once a
// frame right after the pad update; while it is true the pointer is on, every click goes to the overlay, the game's
// menus get no hover, and `step` runs (it reads the pad words and clears them, so the game sees no presses).
void set_overlay_hook(std::function<bool()> active, std::function<void(ppc::Context& ctx, uint8_t* base)> step);
// Game-side work that runs once a frame right after the pad update (Auto Delivery, the quest marker probe), in the
// order added; menu_open = one of the game's menus was open last frame.
void add_frame_hook(std::function<void(ppc::Context& ctx, uint8_t* base, bool menu_open)> hook);
// The lobby quest list (FUN_021E3978: Quest Counter, village, Arena) ran in the last couple of frames; list = its
// state block (+0x16 cursor, +0x18 rows, +0x1A quest ids, u16 each).
bool quest_list_open(uint32_t* list);
// Pad updates so far (one a frame, guest thread).
uint32_t menu_frame();

}  // namespace mh3u::game
