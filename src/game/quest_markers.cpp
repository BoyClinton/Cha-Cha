// Key Quest Markers (user 2026-11-10; mods.h, off by default): in the quest lists of Moga Village and Port Tanzia, key
// and urgent quests get a star and a golden outline, coloured by whether they are still needed for progression: gold =
// a key quest still needed, red-orange = an urgent quest still needed, grey = already cleared. Quests that are only one
// choice of a group ("any 2 of these 5") get nothing (user).
//
// Which quests (read in Ghidra 2026-11-10, the game's own unlock checks; status: wip):
//   Port Tanzia FUN_0214B3B8 (per unlock stage, FUN_021D0AB4(.., 1)): each urgent quest opens once 3 or 4 fixed quests
//   are cleared (FUN_0219CBF4 sUserData::isCleared?) - those are the key quests below; the urgents 12201, 12202, 12401,
//   12501, 12502, 12701, 12801 (also the hunter rank steps of FUN_0219D2E0).
//   Moga Village FUN_0214BAFC (per story stage, FUN_021D0AB4(.., 0)): stages move on single quests, all-of sets, or
//   "n of m" groups; key = the single quests and all-of sets, urgent = the village's 2xxx quests.
// Cleared: FUN_0219CBF4 (sUserData, quest): a bit at save +0x6E74 by the quest's index in the master list
// 0x102076A4 (FUN_0219C9AC).
//
// The star: the list's rows are text widgets whose titles are set again every frame, in list order, by the widget
// text helper FUN_026B6470 (call site 0x026B6530) through FUN_02AFF91C cGUIObjMessage::setTextLen? (message, text,
// length) - seen with MH3U_QUEST_PROBE in the village and Port Tanzia lists, 2026-11-10: the k-th title set from there
// in a frame is the list's k-th quest (list +0x1A). The title is set again with a coloured star in front: the game's
// font has the star (the list's level column uses it) and <COL RRGGBB> colours text (<COLO n> is a palette index). A title the game shortened
// ("...") loses two more characters to make room.
// The golden outline: the row's own cursor highlight frame (below). MH3U_QUEST_PROBE=1 logs, while a list is open, its
// quest ids, every text set on a GUI message with its caller, and the GUI instance drawn with each row's title.
#include "quest_markers.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "functions.h"
#include "gui_util.h"
#include "menu_mouse.h"
#include "mods.h"

namespace mh3u::game {
namespace {

constexpr uint32_t kSUserData = 0x10315F50, kSID = 0x10314578;

// Port Tanzia: the quests each urgent waits for (FUN_0214B3B8), and single-quest unlocks
constexpr uint16_t kTanziaKey[] = {
    11105, 11109, 11110, 11112,  // -> 12201
    11204, 11205, 11208, 11210,  // -> 12202
    11306, 11311, 11312, 11317,  // -> 12401
    11411, 11412, 11413,         // -> 12501
    11504, 11505, 11508, 11509,  // -> 12502
    11609, 11611, 11616, 11618,  // stage 0x3C
    11619, 11622, 11623, 11628,  // -> 12701
    11703, 11705, 11710, 11714,  // stage 0x3F
    11715, 11720, 11723,         // -> 12801
    11824, 11825,                // stages 0x43 / 0x45
};
constexpr uint16_t kTanziaUrgent[] = {12201, 12202, 12401, 12501, 12502, 12701, 12801};
// Moga Village: single quests and all-of sets that move the story on (FUN_0214BAFC); "n of m" groups left out
constexpr uint16_t kVillageKey[] = {
    1101, 1102, 1103,          // stages 6, 7
    1204, 1205,                // 9, 10
    1302, 1304, 1306,          // 0xC - 0xE
    1407,                      // 0x12
    1508, 1509, 1512,          // 0x18, 0x1A
    1605, 1607, 1611,          // 0x1F: all three
    1702, 1704, 1711,          // 0x22: all three
    1803, 1805, 1807, 1810,    // 0x24: all four
    1901, 1902,                // 0x26: both
    1903, 1904, 1907, 1908,    // 0x28 - 0x2A: all four
};

enum class Kind { None, Key, Urgent };
Kind kind_of(uint16_t quest) {
    if (std::find(std::begin(kTanziaUrgent), std::end(kTanziaUrgent), quest) != std::end(kTanziaUrgent)) return Kind::Urgent;
    if (quest >= 2000 && quest < 3000) return Kind::Urgent;  // the village's urgent quests
    if (std::find(std::begin(kTanziaKey), std::end(kTanziaKey), quest) != std::end(kTanziaKey)) return Kind::Key;
    if (std::find(std::begin(kVillageKey), std::end(kVillageKey), quest) != std::end(kVillageKey)) return Kind::Key;
    return Kind::None;
}

bool cleared(ppc::Context& ctx, uint8_t* base, uint16_t quest) {
    const uint32_t user = ppc::ld32(base, kSUserData);
    if (!user) return false;
    const ppc::Context saved = ctx;
    ctx.r[3] = user, ctx.r[4] = quest;
    sub_0219CBF4(ctx, base);
    const bool done = ctx.r[3] != 0;
    ctx = saved;
    return done;
}

constexpr uint32_t kRowTextCall = 0x026B6530;  // FUN_026B6470's call of setTextLen: a row's title
constexpr int kRows = 8;
constexpr uint32_t kBufSize = 512;
uint32_t g_buf[kRows] = {};      // guest buffers for the marked titles (the message keeps the pointer)
uint32_t g_row_obj[kRows] = {};  // each row's title message, as last set
int g_row = 0;                   // titles set so far this frame
struct Mark {
    Kind kind = Kind::None;
    bool done = false;
};
Mark g_mark[kRows];  // each row's marker, as its title was last set

const char* colour(Kind k, bool done) { return done ? "9A9A9A" : k == Kind::Urgent ? "FF6A2A" : "FFC832"; }

// the title with the star in front; a shortened title ("...") gives up two more characters
std::string marked(const std::string& title, Kind k, bool done) {
    std::string t = title;
    static const std::string kEllipsis = "\xE2\x80\xA6";
    if (t.size() >= kEllipsis.size() && t.compare(t.size() - kEllipsis.size(), kEllipsis.size(), kEllipsis) == 0) {
        t.resize(t.size() - kEllipsis.size());
        for (int n = 0; n < 2 && !t.empty(); ++n) {
            while (!t.empty() && (static_cast<unsigned char>(t.back()) & 0xC0) == 0x80) t.pop_back();  // continuation bytes
            if (!t.empty()) t.pop_back();                                                                // the lead byte
        }
        while (!t.empty() && t.back() == ' ') t.pop_back();
        t += kEllipsis;
    }
    return std::string("<COL ") + colour(k, done) + ">\xE2\x98\x85</COL> " + t;
}

// ---- MH3U_QUEST_PROBE ----
bool g_probe = false;
bool g_was_open = false;
std::set<uint32_t> g_dumped;  // instances logged since the list opened
int g_texts = 0;  // texts logged since the list opened (bounded)

std::string guest_text(const uint8_t* base, uint32_t str, uint32_t len) {
    std::string s(reinterpret_cast<const char*>(base + str), std::min<uint32_t>(len ? len : 200, 200));
    if (const size_t z = s.find('\0'); z != std::string::npos) s.resize(z);
    for (char& c : s)
        if (c == '\r' || c == '\n') c = '|';
    return s;
}

void dump_tree(uint8_t* base, uint32_t uid) {
    std::printf("quest probe: lobby screen sID[6] = %08X (vt %08X)\n", uid, uid ? ppc::ld32(base, uid) : 0);
    if (!uid) return;
    std::vector<std::pair<uint32_t, int>> stack;
    if (const uint32_t root = ppc::ld32(base, uid + 0xF4)) stack.push_back({root, 0});
    for (int n = 0; !stack.empty() && n < 800; ++n) {
        const auto [e, depth] = stack.back();
        stack.pop_back();
        std::printf("  %*s%08X vt %08X id %u flags %08X x %.1f y %.1f\n", depth * 2, "", e, ppc::ld32(base, e), ppc::ld32(base, e + 4),
                    ppc::ld32(base, e + 0x54), ppc::ldf32(base, e + 0xA0), ppc::ldf32(base, e + 0xA4));
        std::vector<std::pair<uint32_t, int>> objs;
        if (const uint32_t o = ppc::ld32(base, e + 0xFC)) objs.push_back({o, depth + 2});
        for (int k = 0; !objs.empty() && k < 120; ++k) {
            const auto [o, d] = objs.back();
            objs.pop_back();
            std::printf("  %*sobject %08X vt %08X x %.1f y %.1f\n", d * 2, "", o, ppc::ld32(base, o), ppc::ldf32(base, o + 0x80),
                        ppc::ldf32(base, o + 0x84));
            if (const uint32_t next = ppc::ld32(base, o + 0x6C)) objs.push_back({next, d});
            if (const uint32_t child = ppc::ld32(base, o + 0x68)) objs.push_back({child, d + 1});
        }
        if (const uint32_t next = ppc::ld32(base, e + 0x64)) stack.push_back({next, depth});
        if (const uint32_t child = ppc::ld32(base, e + 0x60)) stack.push_back({child, depth + 1});
    }
}

void frame(ppc::Context& ctx, uint8_t* base, bool) {
    g_row = 0;
    if (!g_probe) return;
    uint32_t list = 0;
    const bool open = quest_list_open(&list);
    if (open && !g_was_open) {
        g_texts = 0;
        const int rows = static_cast<int16_t>(ppc::ld16(base, list + 0x18));
        std::printf("quest probe: list %08X opened, %d rows, cursor %d:", list, rows, static_cast<int16_t>(ppc::ld16(base, list + 0x16)));
        for (int i = 0; i < std::clamp(rows, 0, 8); ++i) {
            const uint16_t q = ppc::ld16(base, list + 0x1A + static_cast<uint32_t>(i) * 2);
            const Kind k = kind_of(q);
            std::printf(" %u%s%s", q, k == Kind::Key ? " key" : k == Kind::Urgent ? " urgent" : "", k != Kind::None && cleared(ctx, base, q) ? " cleared" : "");
        }
        std::printf("\n");
        const ppc::Context saved = ctx;
        ctx.r[3] = ppc::ld32(base, kSID), ctx.r[4] = 6;
        uint32_t uid = 0;
        if (ctx.r[3]) {
            sub_0213F040(ctx, base);
            uid = ctx.r[3];
        }
        ctx = saved;
        dump_tree(base, uid);
        g_dumped.clear();
    }
    if (!open && g_was_open) std::printf("quest probe: list closed\n");
    g_was_open = open;
}

PPC_FUNC(set_message_text) {  // FUN_02AFF91C (message, text, length)
    if (g_probe && g_was_open && g_texts < 400) {
        ++g_texts;
        std::printf("quest probe: text obj %08X lr %08X \"%s\"\n", ctx.r[3], ctx.lr, guest_text(base, ctx.r[4], ctx.r[5]).c_str());
    }
    uint32_t list = 0;
    if (ctx.lr == kRowTextCall && g_row < kRows && quest_list_open(&list)) {
        const int row = g_row++;
        g_row_obj[row] = ctx.r[3];
        g_mark[row] = {};
        if (row < static_cast<int16_t>(ppc::ld16(base, list + 0x18)) && mod_enabled(Mod::KeyQuestMarkers) && ctx.r[4]) {
            const uint16_t quest = ppc::ld16(base, list + 0x1A + static_cast<uint32_t>(row) * 2);
            const Kind k = kind_of(quest);
            if (k != Kind::None) {
                g_mark[row] = {k, cleared(ctx, base, quest)};
                const std::string text = marked(guest_text(base, ctx.r[4], ctx.r[5]), k, g_mark[row].done);
                if (!g_buf[row]) g_buf[row] = gui::guest_string(std::string(kBufSize - 1, ' '));
                if (g_buf[row] && text.size() < kBufSize) {
                    std::memcpy(base + g_buf[row], text.c_str(), text.size() + 1);
                    ctx.r[4] = g_buf[row];
                    ctx.r[5] = static_cast<uint32_t>(text.size());
                }
            }
        }
    }
    sub_02AFF91C__orig(ctx, base);
}

// ---- the outline ----
// Each row is a cGUIInstAnimControl whose tree holds three cGUIObjTexture pieces at its origin - the plate, then the
// two pieces of the cursor's highlight (the gold frame fitting the plate's shape) - the title message and an icon to
// the left (the cleared tick). Seen with MH3U_QUEST_PROBE 2026-11-10: the highlight pieces have flag 0x02000000
// (+0x54) only on the cursor's row (the plate always; the icon shows with bit 0). The outline is the row's own
// highlight frame shown on a marked row: the flag set (and the vertex colours +0xB0..+0xBC, AABBGGRR, greyed for a
// cleared quest), every frame before the row draws - draws are deferred, so the state stays until the frame ends; rows
// no longer marked get the game's own state back. MH3U_QUEST_FRAME picks the pieces shown (2, 3 or 23; default 3).
constexpr uint32_t kTextureVtable = 0x100A7F30, kShown = 0x02000000;
// AABBGGRR. The frame's texture is white: the cursor's gold (about E8A818, from a screenshot) is a tint, and off the
// cursor the game keeps the frame see-through (alpha 0 in its own colours) - seen 2026-11-10.
constexpr uint32_t kGold = 0xBF18A8E8, kGrey = 0xBF9A9A9A;  // at 75 % (user 2026-11-10)
std::string g_frame_pieces = "3";
struct Saved {
    uint32_t colour[4];
};
std::map<uint32_t, Saved> g_colours;  // each highlight piece's own vertex colours, from before we tinted it
std::set<uint32_t> g_shown;           // highlight pieces we showed (to give back to the game)

int row_of(uint8_t* base, uint32_t root) {
    int row = -1;
    gui::each_object(base, root, [&](uint32_t o) {
        for (int i = 0; i < kRows; ++i)
            if (g_row_obj[i] && o == g_row_obj[i]) row = i;
    });
    return row;
}

bool draw_outline(ppc::Context&, uint8_t* base, uint32_t inst) {
    const uint32_t root = ppc::ld32(base, inst + 0xFC);
    uint32_t list = 0;
    if (!root || !quest_list_open(&list)) return false;
    const int row = row_of(base, root);
    if (row < 0) return false;
    std::vector<uint32_t> pieces;  // the row's textures at its origin: plate, highlight, highlight
    gui::each_object(base, root, [&](uint32_t o) {
        if (ppc::ld32(base, o) == kTextureVtable && ppc::ldf32(base, o + 0x80) == 0.0f) pieces.push_back(o);
    });
    if (pieces.size() != 3) return false;
    const bool cursor = row == static_cast<int16_t>(ppc::ld16(base, list + 0x16));
    const bool marked = mod_enabled(Mod::KeyQuestMarkers) && g_mark[row].kind != Kind::None;
    for (int i = 1; i <= 2; ++i) {
        const uint32_t o = pieces[static_cast<size_t>(i)];
        const bool chosen = g_frame_pieces.find(static_cast<char>('1' + i)) != std::string::npos;
        if (!g_colours.count(o)) {
            Saved sv;
            for (uint32_t v = 0; v < 4; ++v) sv.colour[v] = ppc::ld32(base, o + 0xB0 + v * 4);
            g_colours[o] = sv;
        }
        const Saved& own = g_colours[o];
        if (marked && chosen && !cursor) {
            ppc::st32(base, o + 0x54, ppc::ld32(base, o + 0x54) | kShown);
            for (uint32_t v = 0; v < 4; ++v) ppc::st32(base, o + 0xB0 + v * 4, g_mark[row].done ? kGrey : kGold);
            g_shown.insert(o);
        } else if (g_shown.count(o)) {  // ours no longer: the game's own state back
            if (!cursor) ppc::st32(base, o + 0x54, ppc::ld32(base, o + 0x54) & ~kShown);
            for (uint32_t v = 0; v < 4; ++v) ppc::st32(base, o + 0xB0 + v * 4, own.colour[v]);
            g_shown.erase(o);
        }
    }
    return false;  // the game draws the row
}

// MH3U_QUEST_PROBE: the GUI instance drawn with a row's title in its tree (once each while the list is open)
bool probe_draw(ppc::Context&, uint8_t* base, uint32_t inst) {
    if (!g_was_open) return false;
    const uint32_t root = ppc::ld32(base, inst + 0xFC);
    if (!root || g_dumped.count(inst)) return false;
    int row = -1;
    gui::each_object(base, root, [&](uint32_t o) {
        for (int i = 0; i < kRows; ++i)
            if (g_row_obj[i] && o == g_row_obj[i]) row = i;
    });
    if (row < 0) return false;
    g_dumped.insert(inst);
    std::printf("quest probe: row %d instance %08X vt %08X id %u flags %08X x %.1f y %.1f\n", row, inst, ppc::ld32(base, inst),
                ppc::ld32(base, inst + 4), ppc::ld32(base, inst + 0x54), ppc::ldf32(base, inst + 0xA0), ppc::ldf32(base, inst + 0xA4));
    std::vector<std::pair<uint32_t, int>> objs{{root, 1}};
    for (int k = 0; !objs.empty() && k < 120; ++k) {
        const auto [o, d] = objs.back();
        objs.pop_back();
        std::printf("  %*sobject %08X vt %08X flags %08X x %.1f y %.1f w %.1f h %.1f col %08X%s\n", d * 2, "", o, ppc::ld32(base, o),
                    ppc::ld32(base, o + 0x54), ppc::ldf32(base, o + 0x80), ppc::ldf32(base, o + 0x84), ppc::ldf32(base, o + 0x88),
                    ppc::ldf32(base, o + 0x8C), ppc::ld32(base, o + 0x5C), o == g_row_obj[row] ? " (title)" : "");
        if (const uint32_t next = ppc::ld32(base, o + 0x6C)) objs.push_back({next, d});
        if (const uint32_t child = ppc::ld32(base, o + 0x68)) objs.push_back({child, d + 1});
    }
    return false;
}

}  // namespace

void register_quest_markers() {
    char* e = nullptr;
    size_t len = 0;
    g_probe = _dupenv_s(&e, &len, "MH3U_QUEST_PROBE") == 0 && e && std::string(e) == "1";
    std::free(e);
    e = nullptr;
    if (_dupenv_s(&e, &len, "MH3U_QUEST_FRAME") == 0 && e) g_frame_pieces = e;
    std::free(e);
    add_frame_hook(&frame);
    ppc::set_override(0x02AFF91C, &set_message_text);
    if (g_probe) gui::add_draw_hook(&probe_draw);  // first: it only logs
    gui::add_draw_hook(&draw_outline);
}

}  // namespace mh3u::game
