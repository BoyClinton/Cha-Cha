// Boot notices and save prompts confirmed without input (boot_prompts.cpp).
#pragma once

namespace mh3u::game {

// Registers the replacements; with enabled = false they call the original game code unchanged.
void register_boot_prompts(bool enabled);

}  // namespace mh3u::game
