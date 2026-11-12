#include "dlc_content.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <utility>

#include "dlc_menu.h"
#include "inflate.h"
#include "overlay_fs.h"
#include "mods.h"

namespace mh3u::boot {
namespace stdfs = std::filesystem;
namespace {

using game::DlcSection;
constexpr size_t kField = 54;          // %54[^|] in the game's list parser
constexpr uint32_t kFirstModQuest = 20990;  // game/dlc_menu.cpp: the Mods rows; Texture Dump's "download" after them
constexpr uint32_t kTextureDumpQuest = kFirstModQuest + static_cast<uint32_t>(game::Mod::Count);

// The quest data (QTDS version 5, little-endian) in rQuestData::vf10's read order (0x02076CB0): text groups are five
// u32-length-prefixed UTF-8 strings (one per language; the first is kept). Fields named here were matched against the
// 38 event / challenge quests of a save (research log, "Quest data fields").
struct QuestInfo {
    std::string title, goal, sub_goal, failure, client, summary;
    uint32_t level = 0, locale = 0, minutes = 0, fee = 0, reward = 0;
    std::vector<uint32_t> monsters;  // GUIont\Monster ids, in the quest's order
    uint32_t target = 0;             // the main objective's monster (hunt / slay objectives), 0 for deliveries
    bool ok = false;
};

struct Reader {
    const std::vector<uint8_t>& d;
    size_t o = 0;
    bool bad = false;
    uint32_t le(int n) {
        if (o + n > d.size()) {
            bad = true;
            return 0;
        }
        uint32_t v = 0;
        for (int i = 0; i < n && i < 4; ++i) v |= uint32_t{d[o + i]} << 8 * i;
        o += n;
        return v;
    }
    uint32_t u8() { return le(1); }
    uint32_t u16() { return le(2); }
    uint32_t u32() { return le(4); }
    void skip(size_t n) { o + n <= d.size() ? o += n : (bad = true, o); }
    std::string group() {
        std::string first;
        for (int i = 0; i < 5 && !bad; ++i) {
            const uint32_t n = u32();
            if (n > 0x1000 || o + n > d.size()) {
                bad = true;
                break;
            }
            if (i == 0) first.assign(reinterpret_cast<const char*>(&d[o]), n);
            o += n;
        }
        return first;
    }
};

QuestInfo read_quest(const std::vector<uint8_t>& d) {
    QuestInfo q;
    Reader r{d};
    if (r.u32() != 0x53445451 || r.u32() != 5) return q;
    q.title = r.group();
    r.u16();  // quest id
    q.goal = r.group();
    q.level = r.u8();
    q.locale = r.u8();  // GUIont\questdata\QuestMap index
    q.sub_goal = r.group();
    q.minutes = r.u16();
    q.failure = r.group();
    r.skip(6);
    q.client = r.group();
    q.summary = r.group();
    r.skip(2 + 8);
    for (int i = 0; i < 5; ++i) {  // 5 monsters of 11 bytes: id first
        const uint32_t id = r.u8();
        r.skip(10);
        if (id) q.monsters.push_back(id);
    }
    r.skip(6 + 32 * 8);  // then the two reward tables
    q.fee = r.u32();
    q.reward = r.u32();
    r.skip(3 * 4 + 4 + 8 * 2 + 4 * 4 + 6 * 2 + 4 + 4 + 2);  // sub reward, points, ...
    const uint32_t kind = r.u32(), what = r.u16();          // main objective: kind (odd = a monster), id, count
    if (kind & 1) q.target = what;
    q.ok = !r.bad;
    return q;
}

// GUIont message data (gmd, big-endian): string count at +0x18, the strings' bytes at the end, NUL-separated
std::vector<std::string> gmd_strings(const std::vector<uint8_t>& g) {
    std::vector<std::string> out;
    if (g.size() < 0x24) return out;
    const auto be = [&](size_t o) { return uint32_t{g[o]} << 24 | uint32_t{g[o + 1]} << 16 | uint32_t{g[o + 2]} << 8 | g[o + 3]; };
    const uint32_t count = be(0x18), bytes = be(0x20);
    if (bytes > g.size()) return out;
    std::string cur;
    for (size_t i = g.size() - bytes; i < g.size() && out.size() < count; ++i) {
        if (g[i]) cur += static_cast<char>(g[i]);
        else out.push_back(std::exchange(cur, {}));
    }
    return out;
}

// a field for the game's '|' lists: no '|', no CR, cut to the parser's room on a UTF-8 boundary
std::string field(std::string s) {
    std::replace(s.begin(), s.end(), '|', '/');
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    if (s.size() > kField) {
        size_t n = kField;
        while (n > 0 && (static_cast<uint8_t>(s[n]) & 0xC0) == 0x80) --n;
        s.resize(n);
    }
    return s.empty() ? " " : s;  // the game's %[^|] fails on an empty field and stops reading the line
}

std::vector<std::string> lines(const std::string& s, size_t n) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    for (std::string l; out.size() < n && std::getline(ss, l);) out.push_back(field(l));
    out.resize(n);
    return out;
}

uint32_t today_code() {  // YYMMDD00, as the game shows dates
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    return static_cast<uint32_t>((tm.tm_year % 100) * 1000000 + (tm.tm_mon + 1) * 10000 + tm.tm_mday * 100);
}

// One row of DLC_EventQuestInfo / DLC_ChallengeQuestInfo (34 fields, FUN_0201478C; what each shows: research log
// "DLC download screen found").
struct Row {
    uint32_t id = 0;
    std::string title, locale, client;
    std::string level = "0", minutes = "0", reward = "0", fee = "0";
    std::vector<std::string> icons = std::vector<std::string>(5, "0");  // monster ids (0 = none)
    std::vector<std::string> goal = std::vector<std::string>(2), terms = std::vector<std::string>(2),
                             failure = std::vector<std::string>(2), monsters = std::vector<std::string>(2),
                             summary = std::vector<std::string>(7);
};

std::string row_line(const Row& r) {
    std::vector<std::string> f;
    f.push_back(std::to_string(today_code()));
    f.push_back(std::to_string(r.id));
    f.push_back(field(r.title));
    f.push_back("0");                                        // 4 not mapped (no visible effect)
    f.push_back(r.level);                                    // 5: quest level (stars; 0 shows none)
    f.push_back("0");                                        // 6 not mapped
    f.push_back(r.goal[0]);                                  // 7 / 8: success
    f.push_back(r.goal[1]);
    f.push_back(field(r.locale));                            // 9
    f.push_back(r.minutes);                                  // 10-12 time limit, reward, fee
    f.push_back(r.reward);
    f.push_back(r.fee);
    for (const std::string& i : r.icons) f.push_back(i);     // 13-17 monster icons
    f.push_back(r.terms[0]);                                 // 18 / 19
    f.push_back(r.terms[1]);
    f.push_back("");                                         // 20 / 21 not seen
    f.push_back("");
    f.push_back(r.failure[0]);                               // 22 / 23
    f.push_back(r.failure[1]);
    f.push_back(r.monsters[0]);                              // 24 / 25
    f.push_back(r.monsters[1]);
    f.push_back(field(r.client));                            // 26
    for (const std::string& s : r.summary) f.push_back(s);   // 27-33
    f.push_back("");                                         // 34
    std::string out;
    for (const std::string& s : f) out += (s.empty() ? std::string(" ") : s) + "|";  // no empty field (see field())
    return out + "\n";
}

os::DlcResponse text(const std::string& s) {
    os::DlcResponse r;
    r.status = 200;
    r.body.assign(s.begin(), s.end());
    return r;
}

bool starts(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

}  // namespace

DlcContent::DlcContent(stdfs::path data, const fs::OverlayFs* vfs) : data_(std::move(data)), vfs_(vfs) {
    std::error_code ec;
    for (const char* d : {"bonus", "custom", "mods"}) stdfs::create_directories(data_ / d, ec);
}

void DlcContent::set_texture_dump(std::function<void()> start, std::function<double()> progress, std::function<bool()> cancelled) {
    dump_start_ = std::move(start);
    dump_progress_ = std::move(progress);
    dump_cancelled_ = std::move(cancelled);
}

void DlcContent::load_mods() {
    std::ifstream in(data_ / "mods" / "mods.ini");
    for (std::string l; std::getline(in, l);) {
        const size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        for (int m = 0; m < static_cast<int>(game::Mod::Count); ++m)
            if (l.substr(0, eq) == game::mod_info(static_cast<game::Mod>(m)).key)
                game::set_mod_enabled(static_cast<game::Mod>(m), l.substr(eq + 1, 1) != "0");
    }
}

void DlcContent::save_mods() const {
    std::ofstream out(data_ / "mods" / "mods.ini", std::ios::trunc);
    out << "# Cha-Cha mods (main menu > DLC > Mods): 1 on, 0 off\n";
    for (int m = 0; m < static_cast<int>(game::Mod::Count); ++m)
        out << game::mod_info(static_cast<game::Mod>(m)).key << "=" << (game::mod_enabled(static_cast<game::Mod>(m)) ? 1 : 0) << "\n";
}

std::vector<quests::Quest> DlcContent::scan(bool official) const {
    std::vector<quests::Quest> out;
    quests::Library lib = quests::scan_library(data_ / (official ? "bonus" : "custom"));
    for (quests::Quest& q : lib.quests) out.push_back(std::move(q));
    // the old quest folder (before 2026-11-05): its official packs count as bonus, the rest as custom
    quests::Library old = quests::scan_library(data_ / "quests");
    for (quests::Quest& q : old.quests) {
        const bool off = q.pack >= 0 && old.packs[static_cast<size_t>(q.pack)].official;
        if (off == official && std::none_of(out.begin(), out.end(), [&](const quests::Quest& o) { return o.name == q.name; }))
            out.push_back(std::move(q));
    }
    return out;
}

// A text table of the game (GUI\font\...) out of one of its archives, read once
const std::vector<std::string>& DlcContent::game_text(const std::string& arc, const std::string& entry) {
    const std::string key = arc + "|" + entry;
    if (auto it = texts_.find(key); it != texts_.end()) return it->second;
    std::vector<std::string>& out = texts_[key];
    if (!vfs_) return out;
    std::vector<uint8_t> d;
    if (auto f = vfs_->open("/vol/content/nativeCafe/arc/ID/" + arc)) {
        d.resize(static_cast<size_t>(f->size()));
        d.resize(f->read(0, d.data(), d.size()));
    }
    const auto be = [&](size_t o) { return uint32_t{d[o]} << 24 | uint32_t{d[o + 1]} << 16 | uint32_t{d[o + 2]} << 8 | d[o + 3]; };
    constexpr size_t kEntry = 0x50;  // name[64], type, compressed size, size << 3 | flags, offset
    const size_t n = d.size() >= 12 ? (d[6] << 8 | d[7]) : 0;
    for (size_t i = 0; i < n && 12 + (i + 1) * kEntry <= d.size(); ++i) {
        const uint8_t* e = d.data() + 12 + i * kEntry;
        if (std::string(reinterpret_cast<const char*>(e), strnlen(reinterpret_cast<const char*>(e), 64)) != entry) continue;
        const uint32_t csize = be(12 + i * kEntry + 68), off = be(12 + i * kEntry + 76);
        if (size_t{off} + csize > d.size()) break;
        std::vector<uint8_t> g;
        if (!recomp::zlib_inflate(d.data() + off, csize, g)) g.assign(d.data() + off, d.data() + off + csize);
        out = gmd_strings(g);
        break;
    }
    std::printf("dlc: game text %s %s: %zu strings\n", arc.c_str(), entry.c_str(), out.size());
    return out;
}

std::string DlcContent::quest_list(bool challenge, const std::string& lang) {
    const DlcSection s = game::dlc_section();
    std::string out = "# Cha-Cha\n";
    if (s == DlcSection::Mods) {
        if (challenge) return out;
        for (int m = 0; m < static_cast<int>(game::Mod::Count); ++m) {
            const game::ModInfo& info = game::mod_info(static_cast<game::Mod>(m));
            Row r;
            r.id = kFirstModQuest + static_cast<uint32_t>(m);
            r.title = info.name;
            r.goal = {field(info.goal[0]), field(info.goal[1])};
            r.locale = game::mod_enabled(static_cast<game::Mod>(m)) ? "On" : "Off";
            // the page's three numbers (relabelled Solo / Duo / Trio HP, game/dlc_menu.cpp): monster HP for 1-3 hunters
            const bool scaled = game::mod_enabled(game::Mod::QuestScaling);
            r.minutes = scaled ? "65" : "100";
            r.reward = scaled ? "80" : "100";
            r.fee = scaled ? "90" : "100";
            r.client = "Cha-Cha";
            r.summary = lines(info.summary, 7);
            out += row_line(r);
        }
        return out;
    }
    std::vector<quests::Quest> qs = scan(s != DlcSection::Custom);
    std::lock_guard g(m_);
    listed_.clear();
    for (quests::Quest& q : qs) {
        const uint32_t kind = q.id / 1000;
        if (kind != (challenge ? 61u : 20u)) {
            if (challenge == (q.id >= 60000)) std::printf("dlc: %s skipped: the DLC lists show only q_20xxx / q_61xxx\n", q.name.c_str());
            continue;
        }
        if (q.id >= kFirstModQuest && q.id < kFirstModQuest + 10) {
            std::printf("dlc: %s skipped: q_20990-q_20999 are Cha-Cha's (the Mods list)\n", q.name.c_str());
            continue;
        }
        if (listed_.size() >= 48) break;
        const QuestInfo t = read_quest(q.data);
        Row r;
        r.id = q.id;
        r.title = t.title.empty() ? q.title : t.title;
        r.goal = lines(t.goal, 2);
        r.failure = lines(t.failure, 2);
        r.client = t.client;
        r.summary = lines(t.summary, 7);
        if (t.ok) {
            const auto& maps = game_text("ID_lb_" + lang + ".arc", "GUI\\font\\questdata\\QuestMap_" + lang);
            const auto& info = game_text("ID_lb_" + lang + ".arc", "GUI\\font\\questdata\\QuestInfo_" + lang);
            const auto& names = game_text("ID_dl_" + lang + ".arc", "GUI\\font\\Monster_" + lang);
            r.level = std::to_string(t.level);
            r.locale = t.locale < maps.size() ? maps[t.locale] : "";
            r.minutes = std::to_string(t.minutes);
            r.reward = std::to_string(t.reward);
            r.fee = std::to_string(t.fee);
            std::vector<uint32_t> big;  // each monster once, for the icons and the names
            for (uint32_t m : t.monsters)
                if (std::find(big.begin(), big.end(), m) == big.end()) big.push_back(m);
            if (big.empty() && t.target && t.target < names.size()) big.push_back(t.target);  // small-monster quests
            for (size_t i = 0; i < big.size() && i < 5; ++i) r.icons[i] = std::to_string(big[i]);
            std::string who;
            for (uint32_t m : big)
                if (m < names.size()) who += (who.empty() ? "" : ", ") + names[m];
            r.monsters = lines(who.size() > kField ? who.substr(0, who.rfind(", ", kField)) + "\n" + who.substr(who.rfind(", ", kField) + 2) : who, 2);
            const std::string none = info.size() > 5 ? info[5] : "None";
            r.terms = lines(t.sub_goal.empty() ? none : t.sub_goal, 2);
        }
        out += row_line(r);
        listed_.push_back(std::move(q));
    }
    std::printf("dlc: %zu %s quests listed (%s)\n", listed_.size(), challenge ? "challenge" : "event",
                s == DlcSection::Custom ? "custom" : "bonus");
    return out;
}

os::DlcResponse DlcContent::respond(const std::string& url) {
    const std::string name = url.substr(url.find_last_of('/') + 1);
    // the game's language, from the file it asks for (DLC_..._eng.txt)
    const size_t us = name.rfind('_'), dot = name.rfind('.');
    const std::string lang = us != std::string::npos && dot != std::string::npos && dot > us ? name.substr(us + 1, dot - us - 1) : "eng";
    const std::string date = std::to_string(today_code());
    // the DLC Menu's notice board: one line each (Content Preview / Quests / Extras headings, here Official / Quests /
    // Mods at the top)
    if (starts(name, "DLC_Info_Notice_")) return text(date + "|1|0|Official|Capcom's event quests and extras\n");
    if (starts(name, "DLC_Info_Quest_")) return text(date + "|2|0|Quests|Custom quests from your quest folder\n");
    if (starts(name, "DLC_Info_Special_")) {
        std::string t;
        for (int m = 0; m < static_cast<int>(game::Mod::Count); ++m)
            t += std::string(t.empty() ? "" : ", ") + game::mod_info(static_cast<game::Mod>(m)).name + (game::mod_enabled(static_cast<game::Mod>(m)) ? " on" : " off");
        return text(date + "|3|0|Mods|" + field(t) + "\n");
    }
    if (starts(name, "DLC_Info_List_") || starts(name, "DLC_Special_")) return text("# Cha-Cha: none\n");
    if (starts(name, "DLC_EventQuestInfo_")) return text(quest_list(false, lang));
    if (starts(name, "DLC_ChallengeQuestInfo_")) return text(quest_list(true, lang));
    if (name == "q_" + std::to_string(kTextureDumpQuest) + ".quest" && dump_start_ && dump_progress_) {
        dump_start_();
        os::DlcResponse r;
        r.status = 200;
        r.body.assign(0x1F00, 0);  // never kept: game/dlc_menu.cpp clears its save slot again
        r.pieces = 100;
        r.progress = dump_progress_;
        r.cancelled = dump_cancelled_;
        return r;
    }
    if (starts(name, "q_")) {
        std::lock_guard g(m_);
        for (const quests::Quest& q : listed_)
            if (q.name == name) {
                os::DlcResponse r;
                r.status = 200;
                r.body = q.data;
                r.delay_ms = 40;  // a moment per piece, so the download screen's bar is seen
                return r;
            }
    }
    return {};
}

}  // namespace mh3u::boot
