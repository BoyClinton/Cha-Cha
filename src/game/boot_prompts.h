// Boot notices and save prompts confirmed without input (boot_prompts.cpp).
#pragma once

#include <cstdint>

namespace mh3u::game {

// Registers the replacements; with enabled = false they call the original game code unchanged.
void register_boot_prompts(bool enabled);
// The start menu widget (uIDStartMenu) whose state step ran last, 0 before the first (any thread).
uint32_t start_menu_widget();
// Counts start menus: goes up when a new one starts (another widget, or its state back to 0, as after quitting the game
// to the title). The title's layouts are made again then, possibly at the same addresses, so anything kept from them
// (objects, copies of them) is stale once this changes (any thread).
uint32_t start_menu_generation();

}  // namespace mh3u::game
