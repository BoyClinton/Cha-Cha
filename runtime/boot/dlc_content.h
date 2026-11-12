// What Cha-Cha's local DLC server answers (dlc_content.cpp): the files the game's DLC screen downloads, made from the
// add-on folders for the section the player is in (game/dlc_menu.h):
//   Official  <data>/bonus  - Capcom's event / challenge quests (q_<id>.quest files, or quest1..quest4 copied from a
//             Wii U save), plus the official packs of the old <data>/quests folder;
//   Quests    <data>/custom - custom quests (same files), plus the old folder's custom packs;
//   Mods      <data>/mods   - mods.ini: the switches of game/mods.h, listed as the Mods list's rows.
// Quest lists carry what the quest data says (rQuestData's read order, 0x02076CB0): title, goal, failure, client,
// summary, level, locale, time limit, fee, reward and the monsters, with locale and monster names from the game's own
// text (QuestMap / Monster tables, read from the game files). The game lists only quests 20000-20999 (event) and
// 61000-61999 (challenge): others are skipped (logged; docs/mods.md).
#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "hle.h"
#include "quests.h"

namespace mh3u::fs {
class OverlayFs;
}

namespace mh3u::boot {

class DlcContent {
public:
    DlcContent(std::filesystem::path data, const fs::OverlayFs* vfs);  // creates the three folders
    os::DlcResponse respond(const std::string& url);   // guest thread (curl_easy_perform)
    void load_mods();                                  // <data>/mods/mods.ini -> game::set_mod_enabled
    void save_mods() const;
    // The Mods list's Texture Dump row: start() begins the dump, progress() says how far it is (0..1); its
    // "download" goes out as the dump goes, so the game's download screen shows it.
    void set_texture_dump(std::function<void()> start, std::function<double()> progress, std::function<bool()> cancelled);

private:
    std::vector<quests::Quest> scan(bool official) const;
    std::string quest_list(bool challenge, const std::string& lang);
    const std::vector<std::string>& game_text(const std::string& arc, const std::string& entry);
    std::filesystem::path data_;
    const fs::OverlayFs* vfs_;
    std::map<std::string, std::vector<std::string>> texts_;  // game text tables by arc + entry
    std::mutex m_;
    std::function<void()> dump_start_;
    std::function<double()> dump_progress_;
    std::function<bool()> dump_cancelled_;
    std::vector<quests::Quest> listed_;  // the section's quests at the last list, for the q_<id>.quest that follow
};

}  // namespace mh3u::boot
