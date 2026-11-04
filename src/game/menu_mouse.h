// Menu mouse (menu_mouse.cpp): the mouse is a pointer while one of the game's menus is open. Always on.
#pragma once

#include <cstdint>
#include <functional>

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

}  // namespace mh3u::game
