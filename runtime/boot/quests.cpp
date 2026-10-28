#include "quests.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>

namespace mh3u::boot::quests {
namespace {

namespace stdfs = std::filesystem;

std::vector<uint8_t> read_file(const stdfs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
std::string read_text(const stdfs::path& p, size_t max = 4000) {
    auto b = read_file(p);
    if (b.size() > max) b.resize(max);
    std::string s(b.begin(), b.end());
    if (s.size() >= 3 && s.compare(0, 3, "\xEF\xBB\xBF") == 0) s.erase(0, 3);  // UTF-8 BOM
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}
std::string utf8(const stdfs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

bool is_quest_data(const uint8_t* p, size_t n) { return n >= 12 && std::memcmp(p, "QTDS", 4) == 0; }

// q_<digits>.quest -> id
bool parse_name(const std::string& n, uint32_t& id) {
    if (n.size() < 9 || n.compare(0, 2, "q_") != 0) return false;
    const size_t dot = n.find('.');
    if (dot == std::string::npos || n.compare(dot, std::string::npos, ".quest") != 0 || dot <= 2 || dot - 2 > 7) return false;
    id = 0;
    for (size_t i = 2; i < dot; ++i) {
        if (n[i] < '0' || n[i] > '9') return false;
        id = id * 10 + static_cast<uint32_t>(n[i] - '0');
    }
    return true;
}

// The quest data starts "QTDS", version (u32 LE), then length-prefixed (u32 LE) UTF-8 titles, one per language.
std::string first_title(const std::vector<uint8_t>& d) {
    if (d.size() < 12) return {};
    uint32_t len = 0;
    std::memcpy(&len, &d[8], 4);
    if (len == 0 || len > 200 || 12 + len > d.size()) return {};
    std::string s(reinterpret_cast<const char*>(&d[12]), len);
    while (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

// "key": "value" from a small JSON object (pack.json); \" \\ \n \t escapes
std::string json_string(const std::string& j, const char* key) {
    const std::string k = std::string("\"") + key + "\"";
    size_t at = j.find(k);
    if (at == std::string::npos) return {};
    at = j.find(':', at + k.size());
    if (at == std::string::npos) return {};
    at = j.find('"', at);
    if (at == std::string::npos) return {};
    std::string out;
    for (size_t i = at + 1; i < j.size() && j[i] != '"'; ++i) {
        if (j[i] == '\\' && i + 1 < j.size()) {
            const char c = j[++i];
            out += c == 'n' ? '\n' : c == 't' ? '\t' : c;
        } else {
            out += j[i];
        }
    }
    return out;
}

stdfs::path find_image(const stdfs::path& dir, const std::string& stem) {
    std::error_code ec;
    for (const char* ext : {".png", ".jpg", ".jpeg", ".webp", ".bmp"})
        if (stdfs::exists(dir / (stem + ext), ec)) return dir / (stem + ext);
    return {};
}

Quest make(uint32_t id, std::string name, std::vector<uint8_t> data, const stdfs::path& source) {
    Quest q;
    q.id = id;
    q.name = std::move(name);
    q.data = std::move(data);
    q.source = source;
    q.title = first_title(q.data);
    if (q.title.empty()) q.title = q.name;
    return q;
}

// Every slot of a save quest file: (offset, file name or "" when empty).
template <class F>
void for_each_slot(const std::vector<uint8_t>& b, F&& f) {
    for (size_t i = 0; i < kSlots; ++i) {
        const size_t at = kFirstSlot + i * kSlotSize;
        if (at + kSlotSize > b.size()) break;
        const char* nm = reinterpret_cast<const char*>(&b[at + kSlotData]);
        f(at, is_quest_data(&b[at], kSlotData) ? std::string(nm, strnlen(nm, 16)) : std::string());
    }
}
std::vector<uint8_t> slot_data(const std::vector<uint8_t>& b, size_t at) {
    std::vector<uint8_t> d(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + kSlotData));
    while (!d.empty() && d.back() == 0) d.pop_back();
    return d;
}

const char* const kFiles[] = {"quest1", "quest2", "quest3", "quest4"};
bool is_save_file(const std::string& fn) { return std::find(std::begin(kFiles), std::end(kFiles), fn) != std::end(kFiles); }

}  // namespace

Library scan_library(const stdfs::path& dir) {
    Library lib;
    std::map<std::string, int> pack_of;  // folder key -> index
    std::error_code ec;
    std::vector<stdfs::path> files;
    for (auto it = stdfs::recursive_directory_iterator(dir, stdfs::directory_options::skip_permission_denied, ec);
         !ec && it != stdfs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec)) files.push_back(it->path());
    std::sort(files.begin(), files.end());
    std::map<std::string, size_t> by_name;  // a quest found twice: the later file wins
    const auto add = [&](Quest q, const stdfs::path& file, bool official) {
        const stdfs::path rel = file.lexically_relative(dir);
        int pack = -1;
        if (std::distance(rel.begin(), rel.end()) > 1) {  // inside a top-level folder: that folder is the pack
            const std::string key = utf8(*rel.begin());
            auto [it, fresh] = pack_of.emplace(key, static_cast<int>(lib.packs.size()));
            if (fresh) {
                Pack p;
                p.key = key;
                const stdfs::path root = dir / *rel.begin();
                const std::string j = read_text(root / "pack.json");
                p.name = json_string(j, "name");
                p.author = json_string(j, "author");
                p.info = json_string(j, "description");
                if (p.name.empty()) p.name = key;
                p.image = find_image(root, "cover");
                if (p.image.empty()) p.image = find_image(root, "pack");
                lib.packs.push_back(std::move(p));
            }
            pack = it->second;
            if (official) lib.packs[static_cast<size_t>(pack)].official = true;
        }
        q.pack = pack;
        auto [it, fresh] = by_name.emplace(q.name, lib.quests.size());
        if (fresh) lib.quests.push_back(std::move(q));
        else lib.quests[it->second] = std::move(q);
    };
    for (const auto& p : files) {
        const std::string fn = utf8(p.filename());
        uint32_t id = 0;
        if (parse_name(fn, id)) {
            auto d = read_file(p);
            if (!is_quest_data(d.data(), d.size()) || d.size() > kSlotData) continue;
            Quest q = make(id, fn, std::move(d), p);
            const std::string stem = fn.substr(0, fn.find('.'));
            q.image = find_image(p.parent_path(), stem);
            const std::string txt = read_text(p.parent_path() / (stem + ".txt"));
            if (!txt.empty()) q.description = txt;
            add(std::move(q), p, false);
        } else if (is_save_file(fn)) {  // from a Wii U save: Capcom's downloads
            const auto b = read_file(p);
            for_each_slot(b, [&](size_t at, const std::string& name) {
                if (!name.empty() && parse_name(name, id)) add(make(id, name, slot_data(b, at), p), p, true);
            });
        }
    }
    // official packs show no pictures (the game's own quests); sort quests by pack, then id; descriptions
    for (auto& pk : lib.packs)
        if (pk.official) pk.image.clear();
    const auto order = [](const Quest& q) { return q.pack < 0 ? INT_MAX : q.pack; };  // loose quests last
    std::stable_sort(lib.quests.begin(), lib.quests.end(),
                     [&](const Quest& a, const Quest& b) { return order(a) != order(b) ? order(a) < order(b) : a.id < b.id; });
    for (size_t i = 0; i < lib.quests.size(); ++i) {
        Quest& q = lib.quests[i];
        const Pack* pk = q.pack >= 0 ? &lib.packs[static_cast<size_t>(q.pack)] : nullptr;
        if (pk) lib.packs[static_cast<size_t>(q.pack)].quests.push_back(i);
        if (pk && pk->official) q.image.clear();
        char head[96];
        std::snprintf(head, sizeof head, "%s quest %u (%s)", q.challenge() ? "Challenge" : "Event", q.id, q.name.c_str());
        q.description = std::string(head) + (pk ? "\nPack: " + pk->name : std::string()) +
                        (q.description.empty() ? std::string() : "\n\n" + q.description);
    }
    for (auto& pk : lib.packs) {
        int ev = 0, ch = 0;
        for (size_t i : pk.quests) ++(lib.quests[i].challenge() ? ch : ev);
        char n[96];
        std::snprintf(n, sizeof n, "%d event and %d challenge quests", ev, ch);
        pk.description = (pk.official ? "Capcom's downloadable quests, from Wii U save files.\n" : std::string()) +
                         (pk.author.empty() ? std::string() : "By " + pk.author + "\n") + n +
                         (pk.info.empty() ? std::string() : "\n\n" + pk.info) +
                         "\n\nAll: every quest of the pack stays installed, new ones too. Choose: pick quests one by one. "
                         "Off: none installed.";
    }
    return lib;
}

std::vector<Quest> installed(const stdfs::path& save_dir) {
    std::vector<Quest> out;
    for (const char* f : kFiles) {
        const auto b = read_file(save_dir / f);
        for_each_slot(b, [&](size_t at, const std::string& name) {
            uint32_t id = 0;
            if (!name.empty() && parse_name(name, id)) {
                Quest q = make(id, name, slot_data(b, at), save_dir / f);
                q.description = std::string(q.challenge() ? "Challenge" : "Event") + " quest " + std::to_string(id) +
                                " (" + name + "), in your save but not in the quest folder.";
                out.push_back(std::move(q));
            }
        });
    }
    std::sort(out.begin(), out.end(), [](const Quest& a, const Quest& b) { return a.id < b.id; });
    return out;
}

bool install(const stdfs::path& save_dir, const Quest& q, std::string* error, SlotChange* change) {
    const auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (q.data.empty() || q.data.size() > kSlotData) return fail("not a quest file");
    // the slot that has this quest already, else the first free one in the files for its kind
    const std::vector<const char*> targets = q.challenge() ? std::vector<const char*>{"quest4"}
                                                           : std::vector<const char*>{"quest1", "quest2", "quest3"};
    const char* file = nullptr;
    size_t slot_at = 0;
    std::vector<uint8_t> bytes;
    for (int pass = 0; pass < 2 && !file; ++pass)
        for (const char* f : targets) {
            auto b = read_file(save_dir / f);
            if (b.size() < kFirstSlot + kSlots * kSlotSize) continue;
            for_each_slot(b, [&](size_t at, const std::string& name) {
                if (!file && ((pass == 0 && name == q.name) || (pass == 1 && name.empty()))) {
                    file = f;
                    slot_at = at;
                }
            });
            if (file) {
                bytes = std::move(b);
                break;
            }
        }
    if (!file) {
        std::error_code ec;
        if (!stdfs::exists(save_dir / targets.front(), ec)) return fail("no save yet: start the game and create a save first");
        return fail(q.challenge() ? "all 20 challenge quest slots are used" : "all 60 event quest slots are used");
    }
    std::vector<uint8_t> slot(kSlotSize, 0);
    std::memcpy(slot.data(), q.data.data(), q.data.size());
    std::memcpy(slot.data() + kSlotData, q.name.c_str(), std::min<size_t>(q.name.size(), 15));
    std::fstream f(save_dir / file, std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(static_cast<std::streamoff>(slot_at));
    f.write(reinterpret_cast<const char*>(slot.data()), static_cast<std::streamsize>(slot.size()));
    if (!f) return fail("can't write " + (save_dir / file).string());
    if (change) {
        change->file = file;
        change->offset = slot_at;
        change->before.assign(bytes.begin() + static_cast<std::ptrdiff_t>(slot_at), bytes.begin() + static_cast<std::ptrdiff_t>(slot_at + kSlotSize));
        change->after = std::move(slot);
    }
    return true;
}

bool uninstall(const stdfs::path& save_dir, const std::string& name, std::string* error, SlotChange* change) {
    for (const char* fn : kFiles) {
        const auto b = read_file(save_dir / fn);
        size_t found = 0;
        for_each_slot(b, [&](size_t at, const std::string& n) {
            if (!found && n == name) found = at;
        });
        if (!found) continue;
        std::fstream f(save_dir / fn, std::ios::binary | std::ios::in | std::ios::out);
        const std::vector<uint8_t> zero(kSlotSize, 0);
        f.seekp(static_cast<std::streamoff>(found));
        f.write(reinterpret_cast<const char*>(zero.data()), static_cast<std::streamsize>(zero.size()));
        if (!f) {
            if (error) *error = "can't write the save";
            return false;
        }
        if (change) {
            change->file = fn;
            change->offset = found;
            change->before.assign(b.begin() + static_cast<std::ptrdiff_t>(found), b.begin() + static_cast<std::ptrdiff_t>(found + kSlotSize));
            change->after = zero;
        }
        return true;
    }
    if (error) *error = "not installed";
    return false;
}

PackMode default_mode(const Pack& p) { return p.official ? PackMode::All : PackMode::Choose; }
const char* mode_name(PackMode m) { return m == PackMode::All ? "all" : m == PackMode::Off ? "off" : "choose"; }
PackMode mode_from_name(const std::string& s, PackMode fallback) {
    return s == "all" ? PackMode::All : s == "off" ? PackMode::Off : s == "choose" ? PackMode::Choose : fallback;
}

}  // namespace mh3u::boot::quests
