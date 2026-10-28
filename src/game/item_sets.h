// Item sets in the item box menu (item_sets.cpp), MODS -> GAMEPLAY -> ITEM SETS.
#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace mh3u::game {

// save_dir: the folder of the game's save (sets are kept beside it); notify: messages for the player (any thread).
// Always on (no MODS switch, user 2026-10-28).
void register_item_sets(const std::filesystem::path& save_dir, std::function<void(const std::string&)> notify);
// MH3U_GUI_TEST: how a screenshot of the next frame is taken (gfx::dump_next_frame), for the box menu layout test.
void set_frame_capture(std::function<void()> capture);

}  // namespace mh3u::game
