// Boot notices and save prompts confirmed without input (boot_prompts.cpp).
#pragma once

#include <cstdint>

namespace mh3u::game {

// Registers the replacements; with enabled = false they call the original game code unchanged.
void register_boot_prompts(bool enabled);
// The start menu widget (uIDStartMenu) whose state step ran last, 0 before the first (any thread).
uint32_t start_menu_widget();

}  // namespace mh3u::game
