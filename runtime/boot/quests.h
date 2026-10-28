#pragma once
// Event and challenge quests from loose files (MODS -> QUESTS). MH3U keeps downloaded quests in its save, in the files
// quest1-quest3 (event quests, q_2xxxx) and quest4 (challenge quests, q_61xxx): 20 slots each, a slot being the
// quest's .quest data (SDTQ, at most 0x2000 bytes) followed by its 16-byte file name. Capcom's download server is
// gone, so Cha-Cha installs quests into those slots from <data>/quests:
//   - q_<id>.quest files (one quest each: official DLC or custom), with an optional q_<id>.png/.jpg (picture) and
//     q_<id>.txt (description) next to them;
//   - quest1..quest4 files copied from a Wii U save: every quest in them is offered, as official DLC.
// Each top-level subfolder is a pack (optional pack.json: name, author, description; cover.png / pack.png). A pack can
// be installed as a whole and stays complete (new quests in it install themselves). See docs/mods.md.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mh3u::boot::quests {

struct Quest {
    uint32_t id = 0;               // from the file name q_<id>.quest
    std::string name;              // q_20102.quest
    std::string title;             // first title in the quest data (English in western quests)
    std::string description;       // for the launcher's description pane
    std::vector<uint8_t> data;     // SDTQ data
    std::filesystem::path source;  // where it was found
    std::filesystem::path image;   // picture shown with it (custom quests only)
    int pack = -1;                 // index into Library::packs, -1 = loose file in the quest folder itself
    bool challenge() const { return id >= 60000 && id < 70000; }
};

struct Pack {
    std::string key;    // its folder name under the quest folder (the settings key)
    std::string name, author, info;
    std::string description;      // for the launcher (info, author, quest count)
    std::filesystem::path image;  // cover (custom packs only)
    bool official = false;        // quests taken from Wii U save files: Capcom's DLC
    std::vector<size_t> quests;   // indices into Library::quests
};

struct Library {
    std::vector<Quest> quests;  // sorted by pack, then id
    std::vector<Pack> packs;
};

// A slot written in the save: the bytes before and after (to bring the game's loaded copy in step).
struct SlotChange {
    std::string file;  // "quest1"
    size_t offset = 0;
    std::vector<uint8_t> before, after;
};

constexpr size_t kSlotData = 0x2000, kSlotSize = 0x2010, kSlots = 20, kFirstSlot = 0x28;

Library scan_library(const std::filesystem::path& dir);
// Quests installed in the save (<save_dir>/quest1..4).
std::vector<Quest> installed(const std::filesystem::path& save_dir);
// Puts the quest in its file's first free slot (or the slot that has it already). The save's quest files must exist:
// the game creates them with the save.
bool install(const std::filesystem::path& save_dir, const Quest& q, std::string* error, SlotChange* change = nullptr);
bool uninstall(const std::filesystem::path& save_dir, const std::string& name, std::string* error, SlotChange* change = nullptr);

// How a pack is used: every quest kept installed, the user's own choice, or none.
enum class PackMode { All, Choose, Off };
PackMode default_mode(const Pack& p);  // official DLC: All; custom packs: Choose
const char* mode_name(PackMode m);     // "all" / "choose" / "off" (mods.ini)
PackMode mode_from_name(const std::string& s, PackMode fallback);

}  // namespace mh3u::boot::quests
