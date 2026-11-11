// The main menu's DLC screen as Cha-Cha's add-on menu (dlc_menu.cpp; user decision 2026-11-05): its three buttons
// become Official (the game's own DLC menus, content from <data>/bonus), Quests (custom quests from <data>/custom,
// straight to the Quests screen) and Mods (the event quest list as a list of on / off switches, mods.h). The content
// itself comes from the local DLC server (runtime/os/dlc_server.cpp, answered by runtime/boot/dlc_content.cpp), which
// asks dlc_section() what the game is looking at.
#pragma once

#include <functional>

namespace mh3u::game {

enum class DlcSection { Top, Official, Custom, Mods };

DlcSection dlc_section();  // any thread

// on_mods_saved: the Mods list was saved (switches changed through set_mod_enabled), on the guest thread.
// on_dump_cancel: Cancel was chosen on the download screen while Texture Dump ran (guest thread).
void register_dlc_menu(std::function<void()> on_mods_saved, std::function<void()> on_dump_cancel);

}  // namespace mh3u::game
