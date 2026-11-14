// Key Quests for Cha-Cha (MH3U US v32): tags the quests that unlock the next urgent quest on the Quest Counter's
// list. A Cha-Cha plugin (include/chacha_plugin.h, docs/modding.md).
//
// Where the list comes from (docs/research-log.md, 2026-11-04 "Key quests"): rQuestData +0x8E8 openFlag (u64,
// progress bits a quest needs before it is offered), +0x8F0 openSource (u64, bits its clear sets), +0x8F8 openNum
// (u8, how many clears of those bits are needed). The urgent quests (2xxx Moga Village, 12xxx Port Tanzia) need
// bits that ordinary quests set; following that chain back gives the key quests. Where an urgent needs n of more
// than n sources, those quests are a choice ("2 OF 5"). The game's own key flag (Category bit 0x00800000, on
// village 1-5 star quests) is included. Walk 'n' Roll and The Desert Gourmand (village 5 star) come from the
// community wiki: the bit they lead to is set by game code, not by quest data. Checked against the wiki's 1-7 star
// village key quests. Table generated from the game's quest data (412 quests) - addresses and numbers are for v32.
//
// The list: sLobby::questListStep? FUN_021E3978(sLobby, list): list+1 state (0 = browsing), +0x1AC 0xFF while no
// page turn runs, +0x18 rows on this page, +0x1A u16[8] their quest numbers; rows on the TV image (1920 x 1080) at
// x 1295-1755, centred at y 336 + 54 * row (src/game/menu_mouse.cpp).
#include "chacha_plugin.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace {

constexpr uint32_t kQuestList = 0x021E3978;
constexpr const char* kGameSha1 = "d97aabfc5a45362f88dfacb09f23c6dcae6faac2";  // MH3U US v32

enum Kind : uint8_t { kKey = 1, kPick = 2, kUrgent = 3 };
struct Entry {
    uint16_t quest;
    uint8_t kind, need, of;
};
const Entry kTable[] = {
    {1101, 1, 0, 0},  // Harvest 'Shroom - urgent 2201 No Love for Ludroth
    {1102, 1, 0, 0},  // Farm Aid - urgent 2201 No Love for Ludroth
    {1103, 1, 0, 0},  // Guts: It's What's for Dinner - urgent 2201 No Love for Ludroth
    {1204, 1, 0, 0},  // Bear Trap - urgent 2301 Shakalaka Savior
    {1205, 1, 0, 0},  // Pain in the Plains - urgent 2301 Shakalaka Savior
    {1302, 1, 0, 0},  // Playing with Fire - urgent 2401 Accident Investigation
    {1304, 1, 0, 0},  // Save Our Boat - urgent 2401 Accident Investigation
    {1306, 1, 0, 0},  // Leading the Charge - urgent 2401 Accident Investigation
    {1402, 2, 2, 5},  // Leader of the Icepack - 2 of 5 for urgent 2402 Trial of the Sea Dragon
    {1403, 2, 2, 5},  // Hunter Killer - 2 of 5 for urgent 2402 Trial of the Sea Dragon
    {1404, 2, 2, 5},  // Lost in Blue - 2 of 5 for urgent 2402 Trial of the Sea Dragon
    {1405, 2, 2, 5},  // Cold Stones - 2 of 5 for urgent 2402 Trial of the Sea Dragon
    {1406, 2, 2, 5},  // The Snowbound Slider - 2 of 5 for urgent 2402 Trial of the Sea Dragon
    {1407, 1, 0, 0},  // Dragon Lady - urgent 2501 Fell the Lagiacrus!
    {1502, 2, 1, 2},  // Heat Exhaustion - 1 of 2 for urgent 2503 Shakalaka Savior II
    {1503, 2, 1, 2},  // The Lord of the Seas - 1 of 2 for urgent 2503 Shakalaka Savior II
    {1506, 1, 0, 0},  // Walk 'n' Roll - wiki (unlock set in code)
    {1507, 1, 0, 0},  // The Desert Gourmand - wiki (unlock set in code)
    {1508, 1, 0, 0},  // The Wrath of Rathalos - game key flag
    {1509, 1, 0, 0},  // A Bard's Tale - game key flag
    {1512, 1, 0, 0},  // The Volcano's Fury - urgent 2502 Save Moga Village!
    {1605, 1, 0, 0},  // Bye-Bye, Bloom - urgent 2701 Splash Damage
    {1607, 1, 0, 0},  // Pink Queen of the Peaks - urgent 2701 Splash Damage
    {1611, 1, 0, 0},  // The Jaggia Menace - urgent 2701 Splash Damage
    {1702, 1, 0, 0},  // Snowball Fight - urgent 2801 Masked Robbery
    {1704, 1, 0, 0},  // Shocking Development - urgent 2801 Masked Robbery
    {1711, 1, 0, 0},  // Rockslide - urgent 2801 Masked Robbery
    {1803, 1, 0, 0},  // The Fisherman's Fiend - urgent 2901 Howl at the Moon
    {1805, 1, 0, 0},  // Denizen of the Molten Deep - urgent 2901 Howl at the Moon
    {1807, 1, 0, 0},  // The Moving Mountain - urgent 2901 Howl at the Moon
    {1810, 1, 0, 0},  // "Brachydios" Means Trouble - urgent 2901 Howl at the Moon
    {1901, 1, 0, 0},  // Jumping at Shadows - urgent 2902 Conqueror of Land and Sea
    {1902, 1, 0, 0},  // Desert Storm - urgent 2902 Conqueror of Land and Sea
    {1903, 1, 0, 0},  // Heaven and Earth - urgent 2902 Conqueror of Land and Sea
    {1904, 1, 0, 0},  // Midnight Madness - urgent 2902 Conqueror of Land and Sea
    {1907, 1, 0, 0},  // Ace in the Hole - urgent 2902 Conqueror of Land and Sea
    {1908, 1, 0, 0},  // Echo Location - urgent 2902 Conqueror of Land and Sea
    {2201, 3, 0, 0},  // No Love for Ludroth - urgent
    {2301, 3, 0, 0},  // Shakalaka Savior - urgent
    {2401, 3, 0, 0},  // Accident Investigation - urgent
    {2402, 3, 0, 0},  // Trial of the Sea Dragon - urgent
    {2501, 3, 0, 0},  // Fell the Lagiacrus! - urgent
    {2502, 3, 0, 0},  // Save Moga Village! - urgent
    {2503, 3, 0, 0},  // Shakalaka Savior II - urgent
    {2601, 3, 0, 0},  // Poison Control - urgent
    {2701, 3, 0, 0},  // Splash Damage - urgent
    {2801, 3, 0, 0},  // Masked Robbery - urgent
    {2901, 3, 0, 0},  // Howl at the Moon - urgent
    {2902, 3, 0, 0},  // Conqueror of Land and Sea - urgent
    {2903, 3, 0, 0},  // Into the Fire - urgent
    {2904, 3, 0, 0},  // Mark of a Hero - urgent
    {2905, 3, 0, 0},  // The Brilliant Darkness - urgent
    {2906, 3, 0, 0},  // Conqueror of Land and Sea - urgent
    {11105, 1, 0, 0},  // Playing with Fire - urgent 12201 The Fisherman's Fiend
    {11109, 1, 0, 0},  // Leading the Charge - urgent 12201 The Fisherman's Fiend
    {11110, 1, 0, 0},  // Accident Investigation - urgent 12201 The Fisherman's Fiend
    {11112, 1, 0, 0},  // The Merchant's Mission - urgent 12201 The Fisherman's Fiend
    {11204, 1, 0, 0},  // The Horned Wyvern - urgent 12202 Double Trouble
    {11205, 1, 0, 0},  // The Creeping Venom - urgent 12202 Double Trouble
    {11208, 1, 0, 0},  // Denizen of the Molten Deep - urgent 12202 Double Trouble
    {11210, 1, 0, 0},  // The Desert Gourmand - urgent 12202 Double Trouble
    {11306, 1, 0, 0},  // Bye-Bye, Bloom - urgent 12401 Brachydios Mio!
    {11311, 1, 0, 0},  // Splash Damage - urgent 12401 Brachydios Mio!
    {11312, 1, 0, 0},  // Mud Brothers - urgent 12401 Brachydios Mio!
    {11317, 1, 0, 0},  // Supply Constraints - urgent 12401 Brachydios Mio!
    {11411, 1, 0, 0},  // Tumble on the Tundra - urgent 12501 Ruler of Two Realms
    {11412, 1, 0, 0},  // The Volcano's Fury - urgent 12501 Ruler of Two Realms
    {11413, 1, 0, 0},  // Unwanted Visitor - urgent 12501 Ruler of Two Realms
    {11504, 1, 0, 0},  // Desert Desertion - urgent 12502 Throne of the Abyss
    {11505, 1, 0, 0},  // Azure Attack - urgent 12502 Throne of the Abyss
    {11508, 1, 0, 0},  // Echo Location - urgent 12502 Throne of the Abyss
    {11509, 1, 0, 0},  // Bedevil a Deviljho - urgent 12502 Throne of the Abyss
    {11609, 1, 0, 0},  // Sweltering Showdown - urgent 12701 Sticky Situation
    {11611, 1, 0, 0},  // Playing with Fire - urgent 12701 Sticky Situation
    {11616, 1, 0, 0},  // The Cherry Blossom Queen - urgent 12701 Sticky Situation
    {11618, 1, 0, 0},  // The Green Flash - urgent 12701 Sticky Situation
    {11619, 1, 0, 0},  // Axe to Grind - urgent 12701 Sticky Situation
    {11621, 1, 0, 0},  // Avenge the Fallen Hunter - urgent 12701 Sticky Situation
    {11622, 1, 0, 0},  // Snowball Fight - urgent 12701 Sticky Situation
    {11623, 1, 0, 0},  // The Desert Gourmand - urgent 12701 Sticky Situation
    {11626, 1, 0, 0},  // The Creeping Venom - urgent 12701 Sticky Situation
    {11628, 1, 0, 0},  // Thunder on the Tundra - urgent 12701 Sticky Situation
    {11703, 1, 0, 0},  // The Horned Wyvern - urgent 12801 The Black Flame of Calamity
    {11705, 1, 0, 0},  // Desert Storm - urgent 12801 The Black Flame of Calamity
    {11706, 1, 0, 0},  // The Thrill of the Hunt - urgent 12801 The Black Flame of Calamity
    {11708, 1, 0, 0},  // Rathalos Alert - urgent 12801 The Black Flame of Calamity
    {11710, 1, 0, 0},  // Volcanic Activity - urgent 12801 The Black Flame of Calamity
    {11714, 1, 0, 0},  // Night Stalker - urgent 12801 The Black Flame of Calamity
    {11715, 1, 0, 0},  // Infernal Overlord - urgent 12801 The Black Flame of Calamity
    {11718, 1, 0, 0},  // The Fisherman's Fiend - urgent 12801 The Black Flame of Calamity
    {11720, 1, 0, 0},  // A Sea of Wyverns - urgent 12801 The Black Flame of Calamity
    {11721, 1, 0, 0},  // Denizen of the Molten Deep - urgent 12801 The Black Flame of Calamity
    {11723, 1, 0, 0},  // Deep-six a Deviljho - urgent 12801 The Black Flame of Calamity
    {12201, 3, 0, 0},  // The Fisherman's Fiend - urgent
    {12202, 3, 0, 0},  // Double Trouble - urgent
    {12401, 3, 0, 0},  // Brachydios Mio! - urgent
    {12501, 3, 0, 0},  // Ruler of Two Realms - urgent
    {12502, 3, 0, 0},  // Throne of the Abyss - urgent
    {12701, 3, 0, 0},  // Sticky Situation - urgent
    {12801, 3, 0, 0},  // The Black Flame of Calamity - urgent
};

const Entry* find(uint16_t q) {
    const auto* it = std::lower_bound(std::begin(kTable), std::end(kTable), q,
                                      [](const Entry& e, uint16_t v) { return e.quest < v; });
    return it != std::end(kTable) && it->quest == q ? it : nullptr;
}

const ChaChaApi* api = nullptr;
ChaChaPlugin* self = nullptr;

struct Settings {
    int enabled = 1, picks = 1, urgent = 1, side = 1;  // side: 0 inside the row's right end, 1 left of the row (default)
};
Settings g_set;  // main thread
bool g_demo = false;  // CHACHA_KEY_QUESTS_DEMO=1

// what the list showed in its last step (game thread -> HUD)
std::mutex g_m;
struct Snapshot {
    int rows = 0;
    uint16_t quests[8] = {};
    std::chrono::steady_clock::time_point at{};
} g_snap;

bool browsing(const uint8_t* b, uint32_t list) {
    return b[list + 1] == 0 && static_cast<int16_t>(chacha_ld16(b, list + 0x1AC)) == 0xFF;  // no quest picked, no page turn
}
void clear_marks() {
    std::lock_guard g(g_m);
    g_snap.rows = 0;
}

void before_list(void*, ChaChaCall* call) {
    const uint8_t* b = api->guest_base;
    const uint32_t list = call->arg_r[1];
    if (!list || !api->readable(list, 0x1B0)) return;
    if (!browsing(b, list)) {  // a quest picked (its details open), a page turning: no marks
        clear_marks();
        return;
    }
    const int rows = std::clamp(static_cast<int>(static_cast<int16_t>(chacha_ld16(b, list + 0x18))), 0, 8);
    std::lock_guard g(g_m);
    g_snap.rows = rows;
    for (int i = 0; i < rows; ++i) g_snap.quests[i] = chacha_ld16(b, list + 0x1A + i * 2);
    g_snap.at = std::chrono::steady_clock::now();
}

// the step that picks a quest leaves browsing in the same call: the marks go in that frame, not one later
void after_list(void*, ChaChaCall* call) {
    const uint32_t list = call->arg_r[1];
    if (list && api->readable(list, 0x1B0) && !browsing(api->guest_base, list)) clear_marks();
}

void draw_tag(float x, float y, float h, bool right_aligned, const char* text, uint32_t fill, uint32_t ink) {
    const float size = h * 0.62f;
    float tw = 0, th = 0;
    api->text_size(nullptr, size, text, &tw, &th);
    const float pad = h * 0.35f, w = tw + pad * 2;
    const float x0 = right_aligned ? x - w : x, x1 = x0 + w;
    api->draw_rect(x0, y - h / 2, x1, y + h / 2, fill, h * 0.25f);
    ChaChaText t{};
    t.x = (x0 + x1) / 2;
    t.y = y;
    t.size = size;
    t.color = ink;
    t.outline_color = 0;
    t.outline = 0;
    t.align = CHACHA_ALIGN_CENTER;
    api->draw_text(&t, text);
}

// A key, drawn: a ring (bow) with a hole, a shaft and two teeth, in one colour. cx / cy = its centre, h = its height;
// the ring on the left, the teeth at the right end. hole = the colour behind it (the ring's hole).
void draw_key(float cx, float cy, float h, uint32_t ink, uint32_t hole) {
    const float r = h * 0.34f, w = h * 1.5f, x0 = cx - w / 2, x1 = cx + w / 2;
    const float rx = x0 + r, sh = h * 0.12f;  // ring centre, shaft half-height
    api->draw_circle(rx, cy, r, ink);
    api->draw_rect(rx, cy - sh, x1, cy + sh, ink, 0);
    api->draw_rect(x1 - h * 0.20f, cy, x1, cy + h * 0.34f, ink, 0);
    api->draw_rect(x1 - h * 0.48f, cy, x1 - h * 0.30f, cy + h * 0.26f, ink, 0);
    api->draw_circle(rx, cy, r * 0.45f, hole);
}

// The key badge: a gold disc with a dark rim and a dark key on it, a little smaller than the other tags (h), its right
// edge at x.
void draw_key_badge(float x, float y, float h) {
    const uint32_t gold = 0xF2C24BFF, rim = 0x2A1A08FF;
    const float r = h * 0.56f, rim_w = h * 0.072f, cx = x - r - rim_w;  // 10% under the tags' height (user)
    api->draw_circle(cx, y, r + rim_w, rim);
    api->draw_circle(cx, y, r, gold);
    draw_key(cx, y, r * 1.05f, rim, gold);
}

void on_hud(ChaChaPlugin*) {
    if (!g_set.enabled) return;
    Snapshot s;
    {
        std::lock_guard g(g_m);
        s = g_snap;
    }
    if (g_demo) {  // CHACHA_KEY_QUESTS_DEMO=1: the marks on the first rows of any screen, to check how they look
        s.rows = 3;
        s.quests[0] = 1101, s.quests[1] = 1402, s.quests[2] = 2201;
        s.at = std::chrono::steady_clock::now();
    }
    if (s.rows == 0 || std::chrono::steady_clock::now() - s.at > std::chrono::milliseconds(50)) return;  // list closed
    float gx, gy, gw, gh;
    api->game_rect(&gx, &gy, &gw, &gh);
    const float sx = gw / 1920.0f, sy = gh / 1080.0f;
    const float h = 30.0f * sy;
    // left of the quest's row (default; right edge at x 1280, just over the list's edge - user's screenshot
    // 2026-11-04), or inside the name's box at its right end (right edge 1712)
    const float right = g_set.side == 0 ? 1712.0f : 1280.0f;
    for (int i = 0; i < s.rows; ++i) {
        const Entry* e = find(s.quests[i]);
        if (!e || (e->kind == kPick && !g_set.picks) || (e->kind == kUrgent && !g_set.urgent)) continue;
        const float y = gy + (336.0f + 54.0f * static_cast<float>(i)) * sy;
        if (e->kind == kKey) {
            draw_key_badge(gx + right * sx, y, h);
            continue;
        }
        char text[24];
        uint32_t fill, ink = 0x1A1408FF;
        if (e->kind == kPick) std::snprintf(text, sizeof text, "%d OF %d", e->need, e->of), fill = 0x9CC9E8F2;
        else std::snprintf(text, sizeof text, "URGENT"), fill = 0xE0533DF2, ink = 0xFFFFFFFF;
        draw_tag(gx + right * sx, y, h, true, text, fill, ink);
    }
}

void load_settings() {
    const auto flag = [](const char* v) { return std::strcmp(v, "false") != 0 && std::strcmp(v, "0") != 0; };
    g_set.enabled = flag(api->config_get(self, "enabled", "true"));
    g_set.picks = flag(api->config_get(self, "choices", "true"));
    g_set.urgent = flag(api->config_get(self, "urgent", "true"));
    g_set.side = std::strcmp(api->config_get(self, "side", "left"), "right") != 0;
}
void save_settings() {
    api->config_set(self, "enabled", g_set.enabled ? "true" : "false");
    api->config_set(self, "choices", g_set.picks ? "true" : "false");
    api->config_set(self, "urgent", g_set.urgent ? "true" : "false");
    api->config_set(self, "side", g_set.side ? "left" : "right");
    api->config_save(self);
}

void on_save(ChaChaPlugin*) { save_settings(); }
void on_defaults(ChaChaPlugin*) {
    g_set = Settings{};
    save_settings();
}

const char* const kSides[] = {"In the quest's row", "Left of the quest's row"};

void on_settings(ChaChaPlugin*) {
    api->ui_group("Settings");
    bool changed = api->ui_switch("Mark key quests", &g_set.enabled,
                                  "A gold key on the Quest Counter's list for the quests that unlock the next urgent quest.") != 0;
    changed |= api->ui_switch("Mark choices", &g_set.picks,
                              "Where any few of a group will do (\"2 OF 5\": clear any two of the five marked).") != 0;
    changed |= api->ui_switch("Mark urgent quests", &g_set.urgent, "URGENT on the urgent quests themselves.") != 0;
    changed |= api->ui_choice("Tag position", &g_set.side, kSides, 2,
                              "Where the mark sits: just left of the quest's row (on the list's edge), or inside the name's box at its right end.") != 0;
    if (changed) save_settings();
}

}  // namespace

extern "C" CHACHA_EXPORT int chacha_plugin_init(const ChaChaApi* a, ChaChaPlugin* p) {
    api = a;
    self = p;
    p->name = "Key Quests";
    p->version = "1.0";
    if (a->version < 1 || std::strcmp(a->game_sha1, kGameSha1) != 0) {
        a->log(p, "made for MH3U US v32 - this game's executable differs, staying off");
        return 0;
    }
    load_settings();
    if (const char* d = std::getenv("CHACHA_KEY_QUESTS_DEMO"); d && *d == '1') g_demo = true;
    p->on_hud = &on_hud;
    p->on_settings = &on_settings;
    if (chacha_has_save_hooks(a)) p->on_save = &on_save, p->on_defaults = &on_defaults;
    if (!a->hook(p, kQuestList, &before_list, &after_list, nullptr)) {
        a->log(p, "this Cha-Cha has no hook point for the quest list (needs Cha-Cha with mod support)");
        return 0;
    }
    a->log(p, "ready");
    return CHACHA_API_VERSION;
}
