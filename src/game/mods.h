// Cha-Cha's mods, switched on and off in the main menu's DLC > Mods (dlc_menu.cpp; user decision 2026-11-05) and in
// the field menu's CFG tab > Mods (field_gfx.cpp; user 2026-11-09, with Texture Packs moved here from Display
// Settings). Rule (user 2026-11-09): every mod is in both lists - both are built from this table, so a new Mod entry
// shows in both by itself. The features ask mod_enabled() each time they act; boot code loads and saves the switches
// (<data>/mods/mods.ini). Item sets are not a mod: always on.
#pragma once

#include <functional>

namespace mh3u::game {

enum class Mod { QuestScaling, AutoCarve, TexturePacks, InstantGathering, InstantCarve, AutoDelivery, KeyQuestMarkers, Count };

struct ModInfo {
    const char* key;      // mods.ini key
    const char* name;     // list row and title
    const char* goal[2];  // what it does, two short lines
    const char* summary;  // longer text ('\n' between lines, up to 7 of at most ~32 characters: the DLC
                          // detail box overflows at ~38)
    bool default_on;      // until mods.ini says otherwise
};

const ModInfo& mod_info(Mod m);
bool mod_enabled(Mod m);  // any thread
void set_mod_enabled(Mod m, bool on);
// Told of every switch (from set_mod_enabled, any thread): mods that act from outside the game code (Texture Packs).
void set_mod_listener(std::function<void(Mod, bool)> listener);

}  // namespace mh3u::game
