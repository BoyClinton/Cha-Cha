#pragma once
// Where the game read its save files to (FSReadFile / FSReadFileWithPos on /vol/save/...): lets a tool that changes a
// save file while the game runs change the copy the game already loaded too (MODS -> QUESTS installing a quest).
#include <cstdint>
#include <string>
#include <vector>

namespace mh3u::os {

struct SaveRead {
    uint32_t dest = 0;      // guest address the bytes went to
    uint64_t file_pos = 0;  // where in the file they came from
    uint32_t bytes = 0;
};
// Reads of the save file with this name ("quest1"), oldest first (the last few dozen per file).
std::vector<SaveRead> save_reads(const std::string& file_name);

}  // namespace mh3u::os
