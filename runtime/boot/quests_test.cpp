// quests.h: library scan (loose q_<id>.quest files and save-style quest1..4 files), install into free slots of the
// save's quest files, reinstall in place, removal. Synthetic quest data only (no game files).
#include "quests.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace {

namespace stdfs = std::filesystem;
using namespace mh3u::boot;
int g_failed = 0;

#define CHECK(x)                                                         \
    do {                                                                 \
        if (!(x)) {                                                      \
            std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); \
            ++g_failed;                                                  \
        }                                                                \
    } while (0)

std::vector<uint8_t> quest_data(const char* title) {
    std::vector<uint8_t> d = {'Q', 'T', 'D', 'S', 5, 0, 0, 0};
    const uint32_t n = static_cast<uint32_t>(std::strlen(title));
    for (int i = 0; i < 4; ++i) d.push_back(static_cast<uint8_t>(n >> (8 * i)));
    d.insert(d.end(), title, title + n);
    d.resize(0x300, 0x11);
    return d;
}

void write(const stdfs::path& p, const std::vector<uint8_t>& b) {
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

// an empty save quest file as the game creates it: header, 20 empty slots, padding
std::vector<uint8_t> empty_save_file() { return std::vector<uint8_t>(0x29024, 0); }

}  // namespace

int main() {
    const stdfs::path root = stdfs::temp_directory_path() / "cha-cha-quests-test";
    std::error_code ec;
    stdfs::remove_all(root, ec);
    const stdfs::path lib = root / "quests", save = root / "save";
    stdfs::create_directories(lib / "dlc", ec);
    stdfs::create_directories(save, ec);

    // library: an official pack (a Wii U save file), a custom pack (pack.json, a picture), a loose quest, a wrong name
    stdfs::create_directories(lib / "mypack", ec);
    write(lib / "q_20102.quest", quest_data("Streaking"));
    write(lib / "mypack" / "q_61000.quest", quest_data("Challenge 1"));
    write(lib / "mypack" / "q_61000.png", {0x89, 'P', 'N', 'G'});
    {
        std::ofstream j(lib / "mypack" / "pack.json");
        j << "{ \"name\": \"My Pack\", \"author\": \"Me\", \"description\": \"Two \\\"hard\\\" quests\" }";
    }
    write(lib / "notaquest.quest", quest_data("ignored"));
    {
        auto f = empty_save_file();
        const auto d = quest_data("From a save");
        std::memcpy(&f[quests::kFirstSlot], d.data(), d.size());
        std::memcpy(&f[quests::kFirstSlot + quests::kSlotData], "q_20317.quest", 13);
        write(lib / "dlc" / "quest2", f);
    }
    const quests::Library library = quests::scan_library(lib);
    const auto& qs = library.quests;
    CHECK(qs.size() == 3 && library.packs.size() == 2);
    if (qs.size() == 3 && library.packs.size() == 2) {
        CHECK(library.packs[0].key == "dlc" && library.packs[0].official && library.packs[0].name == "dlc");
        CHECK(library.packs[1].name == "My Pack" && library.packs[1].author == "Me" && library.packs[1].info == "Two \"hard\" quests");
        CHECK(!library.packs[1].official);
        CHECK(qs[0].id == 20317 && qs[0].title == "From a save" && qs[0].pack == 0 && qs[0].image.empty() && !qs[0].challenge());
        CHECK(qs[1].id == 61000 && qs[1].challenge() && qs[1].pack == 1 && qs[1].image.filename() == "q_61000.png");
        CHECK(qs[2].id == 20102 && qs[2].title == "Streaking" && qs[2].pack == -1);
        CHECK(quests::default_mode(library.packs[0]) == quests::PackMode::All);
        CHECK(quests::default_mode(library.packs[1]) == quests::PackMode::Choose);
    }
    CHECK(quests::mode_from_name(quests::mode_name(quests::PackMode::Off), quests::PackMode::All) == quests::PackMode::Off);

    // no save yet
    std::string err;
    CHECK(!quests::install(save, qs[0], &err) && err.find("no save") != std::string::npos);

    for (const char* f : {"quest1", "quest2", "quest3", "quest4"}) write(save / f, empty_save_file());
    quests::SlotChange change;
    for (const auto& q : qs) CHECK(quests::install(save, q, &err, &change));
    CHECK(quests::install(save, qs[0], &err));  // again: same slot, not a second copy
    // the last change: the loose event quest into quest1's second slot (the official one took the first)
    CHECK(change.file == "quest1" && change.offset == quests::kFirstSlot + quests::kSlotSize);
    CHECK(change.before == std::vector<uint8_t>(quests::kSlotSize, 0) && change.after.size() == quests::kSlotSize);
    CHECK(std::memcmp(change.after.data(), "QTDS", 4) == 0);
    auto inst = quests::installed(save);
    CHECK(inst.size() == 3);
    int ev = 0, ch = 0;
    for (const auto& q : inst) ++(q.challenge() ? ch : ev);
    CHECK(ev == 2 && ch == 1);
    if (inst.size() == 3) CHECK(inst[0].data == qs[2].data);

    // challenge quests go to quest4, event quests to quest1 first
    {
        std::ifstream f(save / "quest4", std::ios::binary);
        std::vector<char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK(std::memcmp(&b[quests::kFirstSlot + quests::kSlotData], "q_61000.quest", 13) == 0);
    }
    CHECK(quests::uninstall(save, "q_20102.quest", &err, &change));
    CHECK(change.after == std::vector<uint8_t>(quests::kSlotSize, 0) && std::memcmp(change.before.data(), "QTDS", 4) == 0);
    CHECK(!quests::uninstall(save, "q_20102.quest", &err));
    CHECK(quests::installed(save).size() == 2);

    stdfs::remove_all(root, ec);
    if (g_failed) std::fprintf(stderr, "%d check(s) failed\n", g_failed);
    else std::printf("quests: all checks passed\n");
    return g_failed ? 1 : 0;
}
