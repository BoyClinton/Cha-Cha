// Game Options with Cha-Cha's own boxes (game_options.cpp): always on.
#pragma once

#include <functional>
#include <string>

#include "settings_page.h"

namespace mh3u::game {

struct GameOptionsPages {
    SettingsPage display;   // Display Settings (right column): a page of its own (settings_page.cpp)
    SettingsPage graphics;  // Graphics Settings (right column)
    SettingsPage controls;  // Controller Settings: Cha-Cha's controller remapping in place of the game's page
    SettingsPage keyboard;  // Keyboard Settings: keyboard + mouse remapping, in place of the game's Game Settings
};
void register_game_options(GameOptionsPages pages);

// The right column's cursor for the menu pointer: -1 while the cursor is in the game's own column.
int game_options_right_row();
// The menu pointer on Game Options (user 2026-11-06: an instant hover, as on the item pouch): the box under a moved
// pointer selected at once (0-5 the game's column, 6+ ours), with the move sound, in the next Game Options step.
void game_options_hover(int item);

}  // namespace mh3u::game
