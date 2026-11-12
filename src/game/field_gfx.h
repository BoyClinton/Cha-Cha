// The field menu's CFG tab (field_gfx.cpp): Cha-Cha's settings (Display, Graphics, Camera, Mods) after the game's last tab.
#pragma once

#include "settings_page.h"

namespace mh3u::game {

// The settings shown: the Graphics Settings page's rows (label, values, help), changed through its change().
void register_field_gfx(SettingsPage page);

}  // namespace mh3u::game
