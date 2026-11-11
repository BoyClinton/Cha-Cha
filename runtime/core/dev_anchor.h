// Development: set once the title's main menu has opened (start menu state 7, src/game/boot_prompts.cpp), so headless
// scripts can be timed from it instead of from boot, whose length varies run to run (the attract movie, loading):
// MH3U_INPUT_SCRIPT "@N:BUTTON" and MH3U_DUMP_FRAME "@N" count N swaps from the first swap that sees it set.
#pragma once

#include <atomic>

namespace mh3u::core {

inline std::atomic<bool> g_main_menu_opened{false};

}  // namespace mh3u::core
