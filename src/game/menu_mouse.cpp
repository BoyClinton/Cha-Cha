// Menu mouse (always on): tells host input when one of the game's menus is open, so the mouse is a pointer there
// (right click = B, wheel = up / down, left clicks as below) instead of turning the camera.
//
// Replaces: sAppPad::vf6 (0x020E0CFC, the pad system's per-frame update; the original runs, then the menu state is
// read) and sLobby::menuDirections? (0x0221A430, wrapped to see that it was called). Status: wip - read in Ghidra,
// signals checked headless with MH3U_MENU_PROBE (docs/research-log.md, "Menu mouse"); not yet seen in a window.
//
// A menu is open while any of these holds:
// - sGame+0x48 == 0: the start-menu screens (title, Continue / New Game, character creation); 2 = village, 1 = quest;
// - sMenu+0x4C != 0: the field menu (− on the Pro Controller; set from the frame it opens until it closes);
// - a lobby menu read its key-repeat directions this frame: FUN_0221A430 (185 calls in 97 functions, nearly all sLobby
//   menu steps - item box, shops, NPCs) is not called at all in the village or the field menu.
// Closing waits kCloseFrames, so a frame between two menu steps (an animation, a sub-list opening) does not flicker the
// pointer.
//
// Clicks: host input queues left clicks in pointer mode. A menu with hover of its own calls menu_pointer and takes the
// clicks (menu_click) - a click on an item moves its cursor there and confirms, a click beside the items does nothing.
// Hover so far: the start menu's screens, the Options pages, the field menu and its screens, the Guild Card List
// (screens() below), Character Creation's gradients and Skin Tone slider (analog_hover), Yes / No prompts
// (two_choice), the item box options (item_sets.cpp). Every other menu (the other lobby menus) gets A for a click.
//
// MH3U_MENU_PROBE=1 logs the menu state fields when they change and the callers of FUN_0221A430 in each frame, for
// mapping more menus.
#include "menu_mouse.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include "boot_prompts.h"
#include "item_sets.h"
#include "functions.h"
#include "guest_memory.h"

namespace mh3u::game {
namespace {

constexpr uint32_t kSGame = 0x101FDB9C, kSMenu = 0x10314980, kSID = 0x10314578, kSLobby = 0x103147A0, kSAppPad = 0x10312BB0;
constexpr int kCloseFrames = 8;

constexpr int kHoverFrames = 3;  // a hover menu that stopped calling menu_pointer gives clicks back after this

Pointer g_pointer;
bool g_probe = false;
std::atomic<int> g_hover_frames{0};  // menu_pointer called lately
std::atomic<int> g_clicks{0};        // clicks for the hover menu (menu_click)
bool g_lobby_menu = false;  // FUN_0221A430 called since the last pad update
bool g_prompt = false;      // a Yes / No prompt ran since then (two_choice, message_choice): not every one reads the
                            // lobby directions (the village's "Save your progress?" does not)
int g_open_frames = 0;      // frames the pointer stays on without a signal
bool g_open = false;

// ---- probe ----
struct Field {
    const char* name;
    uint32_t singleton, offset, size;
    uint32_t last = 0xFFFFFFFF;
};
Field g_fields[] = {
    {"game+48", kSGame, 0x48, 1}, {"menu+34", kSMenu, 0x34, 1},   {"menu+35", kSMenu, 0x35, 1},
    {"menu+4C", kSMenu, 0x4C, 1}, {"id+354", kSID, 0x354, 4},     {"id+368", kSID, 0x368, 4},
    {"lobby+5450", kSLobby, 0x5450, 1}, {"lobby+6F10", kSLobby, 0x6F10, 1},
    {"menu+59", kSMenu, 0x59, 1}, {"menu+52", kSMenu, 0x52, 1}, {"menu+204", kSMenu, 0x204, 4},
};
uint32_t g_frame = 0;
std::set<uint32_t> g_callers, g_last_callers;
// MH3U_MENU_SNAP=<file>: every 5th pad update, the frame, the start-menu state and the whole uIDStartMenu (0x171E0
// bytes) appended to the file - for finding cursor fields offline (scripted presses, then a diff)
FILE* g_snap = nullptr;
// MH3U_MENU_SNAP_MAKE=1: uIDMakeCharacter (0xD20 bytes, zero-padded) in place of the start menu, while it runs;
// MH3U_MENU_SNAP_SMENU=1: sMenu (0x518 bytes; header state = +0x4C, the field menu open)
bool g_snap_make = false, g_snap_smenu = false;
std::atomic<uint32_t> g_make{0}, g_make_frame{0};  // Character Creation's step ran (make_character_step)
void snapshot(uint8_t* base) {
    const uint32_t w = g_snap_smenu ? ppc::ld32(base, kSMenu) : g_snap_make ? g_make.load() : start_menu_widget();
    if (!g_snap || !w || g_frame % 5) return;
    const uint32_t hdr[2] = {g_frame, g_snap_smenu ? ppc::ld8(base, w + 0x4C) : g_snap_make ? ppc::ld32(base, w + 0x884) : ppc::ld32(base, w + 0x2A0)};
    std::fwrite(hdr, sizeof hdr, 1, g_snap);
    if (g_snap_make || g_snap_smenu) {
        static std::vector<uint8_t> buf(0x171E0);
        std::memcpy(buf.data(), base + w, g_snap_smenu ? 0x518 : 0xD20);
        std::fwrite(buf.data(), buf.size(), 1, g_snap);
    } else {
        std::fwrite(base + w, 0x171E0, 1, g_snap);
    }
    std::fflush(g_snap);
}

// MH3U_MENU_HUNT="f0,f1,f2,f3;..." (headless, with DOWN, DOWN, UP pressed between them): the bytes of the MEM2 heap
// (0x10600000-0x50000000) that went +1 at f1, +1 at f2 and -1 at f3 (values < 64) - a menu's cursor wherever its
// widget lives. Each hit is printed with the vtable-looking words (0x1000xxxx-0x100Cxxxx) in the 0x1800 bytes before
// it, nearest first, to name the object (config/symbols.txt).
struct Hunt {
    uint32_t f[4];
};
std::vector<Hunt> g_hunts;
std::vector<uint8_t> g_hunt_copy;
std::vector<uint32_t> g_hunt_hits;
std::vector<uint8_t> g_hunt_last;
// the committed ranges of [from, to): found once (asking per page is slow on the write-watched heap)
std::vector<std::pair<uint32_t, uint32_t>> g_hunt_ranges;
// MH3U_MENU_HUNT_LIVE=1 (a window): the same hunt over the player's own presses in a menu - forward (down or right)
// twice, then back (up or left) once; the hits are printed half a second after the back press. Presses are the menu
// trigger bits of sAppPad+0x6C (1 up, 2 down, 4 left, 8 right).
bool g_hunt_live = false;
void hunt_live(uint8_t* base) {
    static int stage = 0;
    const uint32_t pad = ppc::ld32(base, kSAppPad);
    const uint32_t trig = pad ? ppc::ld32(base, pad + 0x6C) : 0;
    const bool fwd = trig & 0xA, back = trig & 0x5;
    if (g_hunts.empty()) g_hunts.push_back({});
    Hunt& h = g_hunts[0];
    if (stage && g_frame - h.f[0] > 600) stage = 0;  // too slow: start again on the next press
    if (fwd && stage != 1) {
        h = {{g_frame, 0, 0, 0}}, stage = 1;
        std::printf("menu hunt: frame %u first forward press\n", g_frame);
    } else if (fwd) {
        h.f[1] = g_frame, stage = 2;
    } else if (back && stage == 2) {
        h.f[2] = g_frame, h.f[3] = g_frame + 30, stage = 0;
    }
}

void hunt(uint8_t* base) {
    constexpr uint32_t kFrom = 0x10000000, kTo = 0x50000000;
    core::GuestMemory* gm = core::GuestMemory::current();
    if (!gm) return;
    if (g_hunt_ranges.empty()) {
        for (uint32_t a = 0x10600000; a < kTo;) {  // the MEM2 heap (widgets live there), from its arena base
            const uint32_t step = 0x100000;  // committed in large blocks: probe both ends of each MB, merge
            if (gm->committed(a) && gm->committed(a + step - 0x1000)) {
                if (!g_hunt_ranges.empty() && g_hunt_ranges.back().second == a) g_hunt_ranges.back().second = a + step;
                else g_hunt_ranges.push_back({a, a + step});
            }
            a += step;
        }
    }
    for (const Hunt& h : g_hunts) {
        if (g_frame == h.f[0]) {
            g_hunt_copy.assign(kTo - kFrom, 0);
            for (const auto& [lo, hi] : g_hunt_ranges) std::memcpy(&g_hunt_copy[lo - kFrom], base + lo, hi - lo);
            g_hunt_hits.clear();
        } else if (g_frame == h.f[1] && !g_hunt_copy.empty()) {
            for (const auto& [lo, hi] : g_hunt_ranges)
                for (uint32_t a = lo; a < hi; ++a) {
                    const uint8_t old = g_hunt_copy[a - kFrom], now = base[a];
                    if (old < 64 && now == old + 1) g_hunt_hits.push_back(a);
                }
            g_hunt_last.clear();
            for (uint32_t a : g_hunt_hits) g_hunt_last.push_back(base[a]);
            std::vector<uint8_t>().swap(g_hunt_copy);
        } else if ((g_frame == h.f[2] || g_frame == h.f[3]) && !g_hunt_hits.empty()) {
            const int d = g_frame == h.f[2] ? 1 : -1;
            std::vector<uint32_t> keep;
            std::vector<uint8_t> vals;
            for (size_t i = 0; i < g_hunt_hits.size(); ++i)
                if (base[g_hunt_hits[i]] == static_cast<uint8_t>(g_hunt_last[i] + d)) keep.push_back(g_hunt_hits[i]), vals.push_back(base[g_hunt_hits[i]]);
            g_hunt_hits.swap(keep);
            g_hunt_last.swap(vals);
            if (d < 0) {
                std::printf("menu hunt: frames %u-%u: %zu hits\n", h.f[0], h.f[3], g_hunt_hits.size());
                for (size_t i = 0; i < g_hunt_hits.size() && i < 40; ++i) {
                    const uint32_t a = g_hunt_hits[i];
                    std::printf("  %08X = %u, vtables before:", a, base[a]);
                    int n = 0;
                    for (uint32_t b = (a & ~3u); b + 0x1800 > a && b >= kFrom && n < 3; b -= 4) {
                        const uint32_t w = ppc::ld32(base, b);
                        if (w >= 0x10000000 && w < 0x100C0000 && (w & 3) == 0) std::printf(" %08X@-%X", w, a - b), ++n;
                    }
                    std::printf("\n");
                }
                g_hunt_hits.clear();
            }
        }
    }
}

// MH3U_MENU_LEARN=1 (a window, for mapping menus by playing): when the functions reading the lobby menu directions
// or calling the cursor helpers (learn_cursor_*) change (a menu or a sub-list of it opened), half a second later a
// screenshot (build/frame_<n>.bmp) and a line naming them and the lobby mode (sLobby+0x5450) - their code then gives
// the cursor fields (Ghidra).
bool g_learn = false;
// The cursor helpers run only on a press, so a screenshot is taken when a caller not seen since the menus last
// closed (60 frames with no caller) appears - not on every change.
std::set<uint32_t> g_learn_seen, g_learn_new;
uint32_t g_learn_due = 0, g_learn_quiet = 0;
void learn(uint8_t* base) {
    // the field menu's screens read the pad themselves: a new screen / level (sMenu +0x34, +0x52, +0x35) counts as a
    // caller F0DDSSLL of its own
    static uint32_t last_screen = 0;
    if (const uint32_t menu = ppc::ld32(base, kSMenu); menu && ppc::ld8(base, menu + 0x4C)) {
        const uint32_t key = 0xF0000000u | ppc::ld8(base, menu + 0x34) << 16 | ppc::ld8(base, menu + 0x52) << 8 | ppc::ld8(base, menu + 0x35);
        if (key != last_screen) g_callers.insert(key);
        last_screen = key;
    } else {
        last_screen = 0;
    }
    if (g_callers.empty()) {
        if (++g_learn_quiet > 60) g_learn_seen.clear();
    } else {
        g_learn_quiet = 0;
        for (uint32_t c : g_callers)
            if (g_learn_seen.insert(c).second) g_learn_new = g_callers, g_learn_due = g_frame + 30;
    }
    if (!g_learn_due || g_frame != g_learn_due) return;
    g_learn_due = 0;
    const uint32_t lobby = ppc::ld32(base, kSLobby);
    std::printf("menu learn: frame %u, lobby mode %u, screenshot; directions read by:", g_frame, lobby ? ppc::ld8(base, lobby + 0x5450) : 0);
    for (uint32_t c : g_learn_new) std::printf(" %08X", c);
    std::printf("\n");
    std::fflush(stdout);
    capture_frame();
}

void probe(uint8_t* base) {
    for (Field& f : g_fields) {
        const uint32_t s = ppc::ld32(base, f.singleton);
        if (!s) continue;
        const uint32_t a = s + f.offset;
        const uint32_t v = f.size == 1 ? ppc::ld8(base, a) : f.size == 2 ? ppc::ld16(base, a) : ppc::ld32(base, a);
        if (v == f.last) continue;
        f.last = v;
        std::printf("menu probe: frame %u %s = %X\n", g_frame, f.name, v);
    }
    if (g_callers != g_last_callers) {
        std::printf("menu probe: frame %u directions read by:", g_frame);
        for (uint32_t c : g_callers) std::printf(" %08X", c);
        std::printf("\n");
        g_last_callers = g_callers;
    }
    g_callers.clear();
}

bool menu_signal(uint8_t* base) {
    const uint32_t game = ppc::ld32(base, kSGame);
    if (!game || ppc::ld8(base, game + 0x48) == 0) return true;
    const uint32_t menu = ppc::ld32(base, kSMenu);
    if (menu && ppc::ld8(base, menu + 0x4C)) return true;
    return g_lobby_menu || g_prompt;
}

// The Options pages (FUN_0275BD84(object, page): 8 the list, 18 Sound, 19 Effect, 20 Game, 22 Monitor, 23 Controller,
// 24 Language - the page numbers are the start menu's states): which object and page ran this frame
std::atomic<uint32_t> g_option{0}, g_option_page{0}, g_option_frame{0};
PPC_FUNC(option_page) {
    if (g_probe && (ctx.r[3] != g_option.load() || ctx.r[4] != g_option_page.load()))
        std::printf("menu probe: frame %u options page %u object %08X (start menu %08X)\n", g_frame, static_cast<uint32_t>(ctx.r[4]),
                    static_cast<uint32_t>(ctx.r[3]), start_menu_widget());
    g_option = static_cast<uint32_t>(ctx.r[3]);
    g_option_page = static_cast<uint32_t>(ctx.r[4]);
    g_option_frame = g_frame;
    // Controller Settings (page 23) is disabled for now (user 2026-10-29: it will become a page of our own): it stores
    // the controller choice as it moves, and choosing the GamePad there leaves no working input. Its step sees no
    // left / right (menu bits 4 / 8 of sAppPad +0x6C and +0x80, what FUN_0275A4A8 reads), so the choice cannot change;
    // B still leaves the page.
    const uint32_t pad = ctx.r[4] == 23 ? ppc::ld32(base, kSAppPad) : 0;
    const uint32_t trig = pad ? ppc::ld32(base, pad + 0x6C) : 0, rep = pad ? ppc::ld32(base, pad + 0x80) : 0;
    if (pad) {
        ppc::st32(base, pad + 0x6C, trig & ~0xCu);
        ppc::st32(base, pad + 0x80, rep & ~0xCu);
    }
    sub_0275BD84__orig(ctx, base);
    if (pad) {
        ppc::st32(base, pad + 0x6C, trig);
        ppc::st32(base, pad + 0x80, rep);
    }
}

// MH3U_MENU_LEARN also records the callers of the game's cursor-step helpers (FUN_020431F8, FUN_020432BC,
// FUN_020432C8: cursor, count, direction bits -> the new cursor), which the menus that read the pad themselves use
// too - the quest results (FUN_020E8DD4), the quest's supply box. Only wrapped while learning.
PPC_FUNC(learn_cursor_a) {
    g_callers.insert(ctx.lr);
    sub_020431F8__orig(ctx, base);
}
PPC_FUNC(learn_cursor_b) {
    g_callers.insert(ctx.lr);
    sub_020432BC__orig(ctx, base);
}
PPC_FUNC(learn_cursor_c) {
    g_callers.insert(ctx.lr);
    sub_020432C8__orig(ctx, base);
}

PPC_FUNC(menu_directions) {
    g_lobby_menu = true;
    if (g_probe || g_learn) g_callers.insert(ctx.lr);
    sub_0221A430__orig(ctx, base);
}

// ---- start-menu screens and the Options pages ----
// Each screen is found by a key - the start menu's state (uIDStartMenu+0x2A0; the widget comes from boot_prompts) or
// the Options page that ran last frame (uIDOption via FUN_0275BD84, from the start menu or the game's own menu) - and
// lists its items as boxes in TV pixels with the field holding the game's cursor. Cursors were found by snapshot diffs
// over d-pad presses (MH3U_MENU_SNAP) and in Ghidra (the Options page steps); boxes measured on 1920 x 1080 frame
// dumps. Hover steps the game's own cursor with d-pad taps, one step at a time, so its highlight, sound and help text
// follow as with a pad. A click on an item goes there and then presses A (list screens) or just selects it (pages
// whose selection is the setting). Rows with a value: a click in the value area of the selected row taps left / right
// - or, with a level field (the volume bars), steps the value to the bar clicked.
struct Box {
    float x0, y0, x1, y1;
};
enum class Source { StartMenu, OptionPage, MakeCharacter, MakeSub, MakePrompt, FieldMenu, FieldSub, CardList };
struct Screen {
    const char* name;
    Source source;
    uint32_t key;              // start-menu state or Options page
    uint32_t cursor;           // cursor offset in the object (0: one item, no cursor)
    int cursor_size;           // 2 or 4 bytes
    bool horizontal, confirm;  // left / right steps; A after a click on an item
    std::vector<Box> items;
    float vx0 = 0, vx1 = 0;    // value area of the selected row (left half: left, right half: right)
    uint32_t level = 0;        // value field of row 0 (row i at +4 * i), stepped to the bar clicked
    float bar0 = 0, bar_step = 0;  // x of the first bar's middle (level 1), distance between bars
    int levels = 0;            // highest level
    uint32_t sub = 0, sub_value = 0;  // also needs object+sub == sub_value (a step state inside the screen); field
                                      // screens: sMenu+0x35 == sub_value (a level inside the screen)
    bool cursor_in_menu = false;      // the cursor is a field of sMenu, not of the object
    std::vector<int> values;          // the cursor's value for each item (empty: the item's index)
    std::vector<std::pair<int, int>> grid;  // row, column of each item: steps go up / down to the row, then left / right
    int toggle = -1;                  // this item's click is a right press (it flips a value), not A
    int page = 0;                     // items per page (the cursor counts on across L / R pages): cursor % page
    std::vector<Box> tabs;            // tabs (a click switches with left / right), their cursor at tab_cursor (s16)
    uint32_t tab_cursor = 0;
    uint32_t count = 0;               // a byte beside the cursor: how many of the items are shown now (0: all)
    uint32_t slots = 0, page_cursor = 0;  // a byte per item over all pages, at slots + page * items + item (< 0: an
                                          // empty row, not a target); the page number (s16) at page_cursor
};

// a grid of `rows` x 4 cells (the creation sub-screens' type grids and colour swatches), cursor = row * 4 + column
Screen make_grid(const char* name, uint32_t sub, int rows, float top) {
    Screen g{name, Source::MakeSub, sub, 0x890, 4, false, true, {}};
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < 4; ++c) {
            g.items.push_back({186.0f + c * 225.0f, top + r * 76.0f, 394.0f + c * 225.0f, top + r * 76.0f + 64.0f});
            g.grid.push_back({r, c});
        }
    g.page = rows * 4;
    return g;
}

std::vector<Box> rows(int n, float x0, float x1, float top, float height, float step) {
    std::vector<Box> v;
    for (int i = 0; i < n; ++i) v.push_back({x0, top + i * step, x1, top + i * step + height});
    return v;
}

const std::vector<Screen>& screens() {
    static const std::vector<Screen> s = [] {
        std::vector<Screen> v;
        // Main Menu: New Game, Continue, Options, Data Transfer, DLC
        v.push_back({"main menu", Source::StartMenu, 7, 0x2AC, 4, false, true, rows(5, 588, 1336, 201, 110, 126)});
        // Character Select's "Use this character?" (sub-state +0x290 == 6; an empty slot asks nothing): Yes above No in
        // the help box, cursor the byte sMenu+0x59 (0 Yes, 1 No; up / down step it, wrapping), from FUN_02764E60
        Screen use{"character prompt", Source::StartMenu, 14, 0x59, 1, false, true, {{1250, 870, 1468, 922}, {1250, 936, 1468, 992}}};
        use.sub = 0x290, use.sub_value = 6, use.cursor_in_menu = true;
        v.push_back(use);
        // Character Select (Continue): three save slots
        v.push_back({"character select", Source::StartMenu, 14, 0x318, 4, false, true, rows(3, 240, 930, 205, 190, 208)});
        // New Game after Character Creation: Touch Panel Settings (state 12; Standard / Dynamic at the bottom right,
        // cursor +0x324), then the slot to save to (state 13; the same three slots, cursor +0x318), then "Welcome to the
        // world of Monster Hunter!" (state 15, a message: a click is A)
        v.push_back({"touch panel", Source::StartMenu, 12, 0x324, 4, false, true, {{1290, 873, 1770, 933}, {1290, 951, 1770, 1014}}});
        v.push_back({"save slot", Source::StartMenu, 13, 0x318, 4, false, true, rows(3, 240, 930, 205, 190, 208)});
        // Game Options: Monitor, Sound, Effect, Controller, Game, Language
        v.push_back({"game options", Source::OptionPage, 8, 0x330, 2, false, true, rows(6, 175, 790, 190, 92, 106)});
        // Sound Settings: Music / SFX volume, 7 bars (levels 0-7 at +0x360 / +0x364)
        Screen sound{"sound", Source::OptionPage, 18, 0x368, 4, false, false, {{354, 366, 1590, 495}, {354, 600, 1590, 729}}};
        sound.vx0 = 750, sound.vx1 = 1545, sound.level = 0x360, sound.bar0 = 921, sound.bar_step = 68, sound.levels = 7;
        v.push_back(sound);
        // Effect Settings: Normal / Mild
        v.push_back({"effect", Source::OptionPage, 19, 0x37C, 4, true, false, {{315, 411, 900, 729}, {1020, 411, 1545, 729}}});
        // Game Settings: Quick Load, Wii U GamePad (values Off / On, Type 1 / 2)
        Screen game{"game", Source::OptionPage, 20, 0x3BC, 4, false, false, {{354, 366, 1590, 429}, {354, 468, 1590, 531}}};
        game.vx0 = 1000, game.vx1 = 1520;
        v.push_back(game);
        // Monitor Settings: the brightness slider
        Screen monitor{"monitor", Source::OptionPage, 22, 0, 4, false, false, {{400, 420, 1530, 540}}};
        monitor.vx0 = 400, monitor.vx1 = 1530;
        v.push_back(monitor);
        // Controller Settings (page 23, cursor +0x398): disabled (option_page), so no items here
        // The field menu (- in the village / a quest; sMenu+0x4C open, +0x34 == 0 while its list is up, not a screen
        // opened from it): tabs Hunter / Info (sMenu+0x84), six rows sharing the cursor sMenu+0x5C (s16); a greyed row
        // (Quest Info in the village) is skipped by the game's cursor. Rows measured on frame dumps of both tabs.
        Screen field{"field menu", Source::FieldMenu, 0, 0x5C, 2, false, true, rows(6, 1412, 1826, 286, 44, 52)};
        field.cursor_in_menu = true;
        field.tabs = {{1424, 204, 1556, 248}, {1564, 204, 1684, 248}};
        field.tab_cursor = 0x84;
        v.push_back(field);
        // Screens opened from it (sMenu+0x34 == 1; which one: sMenu+0x52), their cursors bytes of sMenu (found with
        // MH3U_MENU_HUNT, confirmed with sMenu snapshots); rows from frame dumps. The lists turn pages with L / R and
        // count the cursor within the page.
        const auto field_sub = [&](const char* name, uint32_t id, uint32_t cursor, std::vector<Box> items) {
            Screen f{name, Source::FieldSub, id, cursor, 1, false, true, std::move(items)};
            f.cursor_in_menu = true;
            return f;
        };
        for (auto [name, id] : {std::pair<const char*, uint32_t>{"item pouch", 0}, {"combine", 1}, {"combo list", 2}, {"gestures", 11}}) {
            Screen list = field_sub(name, id, 0x21F, rows(8, 1415, 1815, 282, 46, 52));
            list.page = 8;
            v.push_back(list);
        }
        v.push_back(field_sub("hunter's notes", 3, 0x5F, rows(2, 1082, 1326, 436, 49, 52)));   // Monster List, Hunter Info
        v.push_back(field_sub("guild card", 12, 0x36, rows(4, 1082, 1326, 438, 49, 52)));      // Check, Edit, Send, Card List
        v.push_back(field_sub("equipment", 8, 0x5F,                                            // Weapon; 5 armour; Charm
                              {{1415, 285, 1780, 335}, {1415, 365, 1780, 412}, {1415, 417, 1780, 464}, {1415, 469, 1780, 516},
                               {1415, 521, 1780, 568}, {1415, 573, 1780, 620}, {1415, 653, 1780, 700}}));
        // Options (in game): 7 rows a page (3 pages; left / right turn the page here, so a click on a row only goes there
        // and presses A, as with a pad)
        Screen opts = field_sub("field options", 4, 0x225, rows(7, 1410, 1830, 282, 46, 52));
        opts.page = 7;
        v.push_back(opts);
        // One level deeper (sMenu+0x35 != 0, the same +0x34 == 1; sMenu snapshots and frame dumps of each): an item's
        // Discard / Swap (cursor +0x5A) and Swap's "into which position?" (+0x35 == 2, the pouch list again); Hunter
        // Info (id 38): its three categories, their topic lists and the topics shown beside them all share the cursor
        // +0x225 and the rows, 8 a page, +0x229 the rows shown (3, 6, 8 seen); an Options row being edited (A on a row;
        // left / right change the value: a click on the value box steps it by its half, elsewhere on the row it is A,
        // which ends the edit). Nothing to point at: Guild Card Check (+0x35 13, pages only); Combine's second
        // material and Equipment's details stay on their screens' lists.
        Screen item_action = field_sub("item action", 0, 0x5A, {{1124, 592, 1344, 644}, {1124, 648, 1344, 696}});
        item_action.sub_value = 1;
        v.push_back(item_action);
        Screen swap = field_sub("swap to", 0, 0x21F, rows(8, 1415, 1815, 282, 46, 52));
        swap.sub_value = 2, swap.page = 8;
        v.push_back(swap);
        Screen hunter_info = field_sub("hunter info", 38, 0x225, rows(8, 1415, 1825, 284, 46, 52));
        hunter_info.sub_value = 1, hunter_info.page = 8, hunter_info.count = 0x229;
        v.push_back(hunter_info);
        Screen edit = field_sub("option value", 4, 0x225, rows(7, 1410, 1830, 282, 46, 52));
        edit.sub_value = 1, edit.page = 7, edit.vx0 = 1660, edit.vx1 = 1830;
        v.push_back(edit);
        // Monster List (id 37): the same list as Hunter Info (+0x225, +0x229 the rows shown: 1 with "(No Data)" in a
        // save with no monster hunted; its row on the same place)
        Screen monsters = field_sub("monster list", 37, 0x225, rows(8, 1415, 1825, 284, 46, 52));
        monsters.sub_value = 1, monsters.page = 8, monsters.count = 0x229;
        v.push_back(monsters);
        // Guild Card List (+0x35 15): its own widget, uIDLobbyCardList (step FUN_0268E7DC, wrapped: card_list_step),
        // 8 rows a page: row +0x3D4 (s8; up / down, wrapping), page +0x3D8 (s16; left / right), the cards' slots s8 at
        // +0x3DC + page * 8 + row (< 0: empty); A views the card there. Rows measured on a frame dump.
        Screen cards{"guild card list", Source::CardList, 0, 0x3D4, 1, false, true, rows(8, 162, 1756, 240, 58, 66.6f)};
        cards.page = 8, cards.slots = 0x3DC, cards.page_cursor = 0x3D8;
        v.push_back(cards);
        // Character Creation (uIDMakeCharacter, cursor +0x884: 1 Name, 2 gender, 4 Clothing / 5 Clothing Color, 6 Face /
        // 7 Features, 8 Hairstyle / 9 Hair Color, 10 Voice / 3 Skin Tone, 11 OK; 0 while a sub-screen is open; read by
        // FUN_027512A0, numbering from d-pad walks). Left / right are only pressed between the two columns, never on the
        // gender row (they change the gender there).
        Screen make{"character creation", Source::MakeCharacter, 0, 0x884, 4, false, true,
                    {{135, 267, 570, 325}, {570, 285, 800, 380},                        // Name, gender
                     {144, 408, 600, 474}, {654, 408, 1110, 474},                       // Clothing, Clothing Color
                     {144, 495, 600, 561}, {654, 495, 1110, 561},                       // Face, Features
                     {144, 585, 600, 651}, {654, 585, 1110, 651},                       // Hairstyle, Hair Color
                     {144, 675, 600, 738}, {654, 675, 1110, 738},                       // Voice, Skin Tone
                     {264, 765, 984, 822}}};                                            // OK
        make.values = {1, 2, 4, 5, 6, 7, 8, 9, 10, 3, 11};
        make.grid = {{0, 0}, {1, 0}, {2, 0}, {2, 1}, {3, 0}, {3, 1}, {4, 0}, {4, 1}, {5, 0}, {5, 1}, {6, 0}};
        make.toggle = 1;  // the gender symbol: a click flips it
        v.push_back(make);
        // Its sub-screens (+0x868 = the item opened, numbered as the cursor; 0 = none), cursor +0x890: type grids of
        // up to 5 x 4 per page (L / R turn pages), Clothing's one row lower down; the colour swatches 3 x 4 (the
        // gradient under them and Skin Tone's slider: analog_hover); Features: Feature 1 Off (0), Feature 2 Off (2)
        // and its Color (3). Rows measured on frame dumps of each.
        v.push_back(make_grid("clothing", 4, 1, 504));
        v.push_back(make_grid("face", 6, 5, 360));
        v.push_back(make_grid("hairstyle", 8, 5, 360));
        v.push_back(make_grid("voice", 10, 5, 360));
        // the colour screens: 3 x 4 swatches, then the gradient under them (cursor 12, a row of its own; A there lets
        // the pointer pick the colour: analog_hover)
        for (auto [name, sub] : {std::pair<const char*, uint32_t>{"clothing color", 5}, {"hair color", 9}}) {
            Screen colors = make_grid(name, sub, 3, 360);
            colors.page = 0;
            colors.items.push_back({200, 606, 602, 728});
            colors.grid.push_back({3, 0});
            v.push_back(colors);
        }
        // Its modes (+0x874): 2 the grid; 3 "Cancel character creation and return to the Main Menu?" - Yes above No in
        // a box under the grid, cursor +0x89C (0 Yes, 1 No, No first; up / down); 4 the name keyboard is open, 5 the
        // name was entered (no items: clicks there do nothing)
        v.push_back({"creation prompt", Source::MakePrompt, 3, 0x89C, 4, false, true, {{855, 900, 1155, 954}, {855, 966, 1155, 1020}}});
        for (uint32_t mode : {4u, 5u}) v.push_back({"name entry", Source::MakePrompt, mode, 0, 4, false, false, {}});
        Screen features{"features", Source::MakeSub, 7, 0x890, 4, false, true, {{128, 290, 270, 344}, {128, 416, 270, 466}, {386, 416, 530, 466}}};
        features.values = {0, 2, 3};
        features.grid = {{0, 0}, {1, 0}, {1, 1}};
        v.push_back(features);
        // Language Settings: one row
        Screen language{"language", Source::OptionPage, 24, 0, 4, false, false, {{354, 366, 1590, 429}}};
        language.vx0 = 1000, language.vx1 = 1520;
        v.push_back(language);
        return v;
    }();
    return s;
}

constexpr int kStepFrames = 5;  // pad updates between two taps (a tap holds its buttons for 3 reads)
constexpr uint32_t kVpadA = 0x8000, kVpadDown = 0x100, kVpadUp = 0x200, kVpadRight = 0x400, kVpadLeft = 0x800;

const Screen* g_screen = nullptr;  // the screen up this frame
uint32_t g_object = 0;             // its object (uIDStartMenu or uIDOption)
float g_last_x = -1, g_last_y = -1;
int g_target = -1, g_cooldown = 0;
bool g_confirm = false;    // A once the cursor reaches g_target
int g_value_steps = 0;     // left (< 0) / right (> 0) taps still to give on the selected row
int g_step_from = -1;      // the cursor before the last step tap (-1: none)
int g_steps = 0;           // taps in the current walk
int g_tab_target = -1;     // a tab clicked
int g_level_target = -1;   // a bar clicked: step the selected row's level there

std::atomic<uint32_t> g_card{0}, g_card_frame{0};  // the Guild Card List's step ran then (card_list_step), its object
std::atomic<uint32_t> g_two_choice_frame{0};  // a Yes / No prompt ran then (two_choice): it takes the pointer
std::atomic<uint32_t> g_message_frame{0}, g_message_value{0};  // the message box's Yes / No drawn then, its value
const Screen kMessageBox{"message box", Source::StartMenu, 0, 1, 4, true, true, {{690, 595, 925, 668}, {1000, 595, 1230, 668}}};

// ---- Character Creation's analog pickers ----
// Skin Tone's slider (sub-screen 3, step FUN_02754C04: left / right step the int +0x8B4, 0-99; FUN_027518E8 moves the
// knob and recolours the skin) and the gradients under Clothing Color / Hair Color's swatches (sub-screens 5 / 9, mode
// 3 after A on the gradient: the d-pad steps floats x 0-175, y 0-50 - FUN_0275274C / FUN_027527C0, FUN_02752120 /
// FUN_02752194 - and FUN_02752470 / FUN_02751E5C redraw). Both steps take A as "done" (back to the swatches / out of
// Skin Tone). The pointer over a picker sets its value as the pad would, so the knob and the model follow it; a click
// on it confirms (A), one beside it does nothing. The knob sits at the area's x0 / y0 + value / max * its size
// (FUN_027518E8: 618 px for 100; the gradients 400 x 116 px), measured on frame dumps.
struct Analog {
    const char* name;
    uint32_t open, mode;  // uIDMakeCharacter +0x868 (the sub-screen) and +0x874 (its mode)
    uint32_t fx, fy;      // the value(s): int at fx (a slider, fy 0) or floats x, y
    float max_x, max_y, x0, y0, width, height;
    void (*refresh)(ppc::Context&, uint8_t*);
};
const Analog kAnalogs[] = {
    {"skin tone", 3, 2, 0x8B4, 0, 99, 0, 288, 528, 618 * 0.99f, 0, sub_027518E8},
    {"clothing gradient", 5, 3, 0x8AC, 0x8B0, 175, 50, 200, 610, 400, 116, sub_02752470},
    {"hair gradient", 9, 3, 0x8A4, 0x8A8, 175, 50, 200, 610, 400, 116, sub_02751E5C},
};
const Analog* g_analog = nullptr;  // the picker up this frame (find_screen)
float g_analog_x = -1, g_analog_y = -1;

void analog_hover(ppc::Context& ctx, uint8_t* base, uint32_t m) {
    const Analog& a = *g_analog;
    float x = 0, y = 0;
    if (!menu_pointer(x, y)) return;
    constexpr float kMargin = 24;  // around the area: the ends are easy to reach
    const float slider_half = 28;  // the slider's bar, above and below its knob
    const float top = a.fy ? a.y0 - kMargin : a.y0 - slider_half, bottom = a.fy ? a.y0 + a.height + kMargin : a.y0 + slider_half;
    const bool in = x >= a.x0 - kMargin && x <= a.x0 + a.width + kMargin && y >= top && y <= bottom;
    const bool moved = x != g_analog_x || y != g_analog_y;
    g_analog_x = x, g_analog_y = y;
    bool click = false;
    while (menu_click()) click = click || in;
    if (!in || (!moved && !click)) return;
    const float u = std::clamp((x - a.x0) / a.width, 0.0f, 1.0f), v = a.fy ? std::clamp((y - a.y0) / a.height, 0.0f, 1.0f) : 0;
    bool changed = false;
    if (a.fy) {
        const float vx = std::round(u * a.max_x), vy = std::round(v * a.max_y);
        changed = std::bit_cast<float>(ppc::ld32(base, m + a.fx)) != vx || std::bit_cast<float>(ppc::ld32(base, m + a.fy)) != vy;
        ppc::st32(base, m + a.fx, std::bit_cast<uint32_t>(vx));
        ppc::st32(base, m + a.fy, std::bit_cast<uint32_t>(vy));
    } else {
        const uint32_t value = static_cast<uint32_t>(std::lround(u * a.max_x));
        changed = ppc::ld32(base, m + a.fx) != value;
        ppc::st32(base, m + a.fx, value);
    }
    if (changed) {
        if (g_probe) std::printf("menu probe: frame %u analog %s %.0f, %.0f\n", g_frame, a.name, u * a.max_x, v * a.max_y);
        const ppc::Context saved = ctx;
        ctx.r[1] -= 0x40;
        ctx.r[3] = m;
        a.refresh(ctx, base);
        ctx = saved;
    }
    if (click && g_pointer.tap) g_pointer.tap(kVpadA);
}

const Screen* find_screen(uint8_t* base, uint32_t& object) {
    // Character Creation: its step ran lately, or the start menu is in its creation state (10; the step does not run
    // while a sub-screen is open)
    const uint32_t sm = start_menu_widget();
    if (g_frame - g_make_frame.load() <= 2 || (g_make.load() && sm && ppc::ld32(base, sm + 0x2A0) == 10)) {
        const uint32_t m = g_make.load();
        const uint32_t open = m ? ppc::ld32(base, m + 0x868) : 0, mode = m ? ppc::ld32(base, m + 0x874) : 0;
        for (const Analog& a : kAnalogs)
            if (m && a.open == open && a.mode == mode) return object = m, g_analog = &a, nullptr;
        if (m && mode != 2 && !(mode == 3 && open))  // mode 3 in a sub-screen is its own (the gradients)
            for (const Screen& s : screens())
                if (s.source == Source::MakePrompt && s.key == mode) return object = m, &s;
        if (m && mode == 2 && open == 0 && ppc::ld32(base, m + 0x884) - 1 < 11)
            for (const Screen& s : screens())
                if (s.source == Source::MakeCharacter) return object = m, &s;
        if (m && mode == 2 && open)
            for (const Screen& s : screens())
                if (s.source == Source::MakeSub && s.key == open) return object = m, &s;
        if (m) return nullptr;  // another creation screen (the name keyboard)
    }
    const uint32_t game = ppc::ld32(base, kSGame), w = start_menu_widget();
    const bool start_menu = game && ppc::ld8(base, game + 0x48) == 0 && w;
    // a start-menu step state of its own (a prompt inside a screen) first: it also shows the message box
    if (start_menu)
        for (const Screen& s : screens())
            if (s.source == Source::StartMenu && s.sub && s.key == ppc::ld32(base, w + 0x2A0) && ppc::ld32(base, w + s.sub) == s.sub_value)
                return object = w, &s;
    if (g_frame - g_two_choice_frame.load() <= 2) return nullptr;
    if (g_frame - g_message_frame.load() <= 2) return &kMessageBox;
    if (const uint32_t c = g_card.load(); c && g_frame - g_card_frame.load() <= 2 && ppc::ld8(base, c + 0x3D2) == 0)
        for (const Screen& s : screens())  // its list (state +0x3D2 0; others: a card shown, moving one, a prompt)
            if (s.source == Source::CardList) return object = c, &s;
    if (const uint32_t menu = ppc::ld32(base, kSMenu); menu && game && ppc::ld8(base, game + 0x48) != 0 && ppc::ld8(base, menu + 0x4C)) {
        const uint8_t depth = ppc::ld8(base, menu + 0x34);
        for (const Screen& s : screens())
            if ((depth == 0 && s.source == Source::FieldMenu) ||
                (depth == 1 && s.source == Source::FieldSub && s.key == ppc::ld8(base, menu + 0x52) &&
                 s.sub_value == ppc::ld8(base, menu + 0x35)))
                return object = menu, &s;
        return nullptr;  // not mapped (see screens()): a click is A
    }
    if (g_frame - g_option_frame.load() <= 2)
        for (const Screen& s : screens())
            if (s.source == Source::OptionPage && s.key == g_option_page.load()) return object = g_option.load(), &s;
    if (!start_menu) return nullptr;
    const uint32_t state = ppc::ld32(base, w + 0x2A0);
    for (const Screen& s : screens())
        if (s.source == Source::StartMenu && s.key == state && (!s.sub || ppc::ld32(base, w + s.sub) == s.sub_value))
            return object = w, &s;
    return nullptr;
}

int item_at(const Screen& s, float x, float y) {
    for (size_t i = 0; i < s.items.size(); ++i)
        if (x >= s.items[i].x0 && x <= s.items[i].x1 && y >= s.items[i].y0 && y <= s.items[i].y1) return static_cast<int>(i);
    return -1;
}

void screen_hover(uint8_t* base) {
    const Screen* s = g_screen;
    if (!s) {
        g_target = -1, g_confirm = false, g_value_steps = 0, g_level_target = -1, g_tab_target = -1;
        return;
    }
    const uint32_t holder = s->cursor_in_menu ? ppc::ld32(base, kSMenu) : g_object;
    const int count = s->count && holder ? std::min(static_cast<int>(s->items.size()), static_cast<int>(ppc::ld8(base, holder + s->count)))
                                         : static_cast<int>(s->items.size());
    const int cursor = s == &kMessageBox ? static_cast<int>(g_message_value.load())
                       : !s->cursor || !holder ? 0
                       : s->cursor_size == 1 ? static_cast<int>(ppc::ld8(base, holder + s->cursor))
                       : s->cursor_size == 2 ? static_cast<int16_t>(ppc::ld16(base, holder + s->cursor))
                                             : static_cast<int>(ppc::ld32(base, holder + s->cursor));
    const int raw = s->page ? cursor % s->page : cursor;
    int at = raw;  // the item the cursor is on
    if (!s->values.empty()) {
        at = -1;
        for (size_t i = 0; i < s->values.size(); ++i)
            if (s->values[i] == raw) at = static_cast<int>(i);
    }
    float x = 0, y = 0;
    const bool on = menu_pointer(x, y);
    int item = on ? item_at(*s, x, y) : -1;
    if (item >= 0 && s->slots) {  // an empty row is no target
        const uint32_t pg = static_cast<uint32_t>(static_cast<int16_t>(ppc::ld16(base, holder + s->page_cursor)));
        if (static_cast<int8_t>(ppc::ld8(base, holder + s->slots + pg * s->page + static_cast<uint32_t>(item))) < 0) item = -1;
    }
    static const Screen* logged = nullptr;
    static int logged_cursor = -2;
    if (g_probe && (s != logged || cursor != logged_cursor))
        std::printf("menu probe: frame %u screen %s cursor %d sub %u mode %u\n", g_frame, s->name, cursor,
                    g_object ? ppc::ld32(base, g_object + 0x290) : 0, g_make.load() ? ppc::ld32(base, g_make.load() + 0x874) : 0);
    logged = s, logged_cursor = cursor;
    if (on && (x != g_last_x || y != g_last_y) && item >= 0) g_target = item;  // a moved pointer picks the item
    if (on) g_last_x = x, g_last_y = y;
    while (menu_click()) {
        for (size_t i = 0; i < s->tabs.size(); ++i)
            if (on && x >= s->tabs[i].x0 && x <= s->tabs[i].x1 && y >= s->tabs[i].y0 && y <= s->tabs[i].y1)
                g_tab_target = static_cast<int>(i), g_target = -1, g_confirm = false;
        if (item < 0) continue;  // beside the items: nothing
        g_target = item;
        if (s->vx1 > s->vx0 && x >= s->vx0 && x <= s->vx1) {  // the value area: change the value
            if (s->levels && x >= s->bar0 - s->bar_step) {
                const int lv = static_cast<int>((x - s->bar0) / s->bar_step + 1.5f);
                g_level_target = lv < 0 ? 0 : lv > s->levels ? s->levels : lv;
            } else if (s->levels) {
                g_level_target = 0;  // left of the bars ("Min")
            } else {
                g_value_steps += x < (s->vx0 + s->vx1) * 0.5f ? -1 : 1;
            }
        } else if (item == s->toggle) {
            g_value_steps += 1;
        } else if (s->confirm) {
            g_confirm = true;
        }
    }
    if (g_cooldown > 0) {
        --g_cooldown;
        return;
    }
    if (g_tab_target >= 0) {  // a tab clicked: left / right until it is the open one
        const int tab = static_cast<int16_t>(ppc::ld16(base, holder + s->tab_cursor));
        if (tab == g_tab_target || tab < 0 || tab >= static_cast<int>(s->tabs.size()) || ++g_steps > 4) {
            g_tab_target = -1, g_steps = 0;
        } else {
            g_pointer.tap(tab < g_tab_target ? kVpadRight : kVpadLeft);
            g_cooldown = kStepFrames;
            return;
        }
    }
    if (g_target >= count || at < 0 || at >= count) {
        g_target = -1, g_confirm = false, g_value_steps = 0, g_level_target = -1;
        return;
    }
    const auto tap = [&](uint32_t b) {
        if (g_probe) std::printf("menu probe: frame %u %s tap %X (cursor %d, target %d)\n", g_frame, s->name, b, raw, g_target);
        g_pointer.tap(b);
        g_cooldown = kStepFrames;
    };
    // a step that left the cursor where it was (an empty cell, the end of a row): the walk ends, the click is dropped
    // a walk that goes on too long (a greyed item the cursor skips: it would circle) ends the same way
    if (g_step_from >= 0 && ((g_step_from == raw && at != g_target) || ++g_steps > 2 * count + 2)) g_target = -1, g_confirm = false;
    g_step_from = -1;
    if (g_target < 0) g_steps = 0;
    if (g_target >= 0 && at != g_target && s->cursor) {
        g_step_from = raw;
        if (!s->grid.empty()) {
            const auto [row, col] = s->grid[at];
            const auto [trow, tcol] = s->grid[g_target];
            if (row != trow) tap(row < trow ? kVpadDown : kVpadUp);
            else if (col != tcol) tap(col < tcol ? kVpadRight : kVpadLeft);
            else g_target = -1;
            return;
        }
        const bool forward = at < g_target;
        tap(s->horizontal ? (forward ? kVpadRight : kVpadLeft) : (forward ? kVpadDown : kVpadUp));
        return;
    }
    if (g_level_target >= 0) {
        const int level = static_cast<int>(ppc::ld32(base, g_object + s->level + 4 * static_cast<uint32_t>(at)));
        if (level == g_level_target || level < 0 || level > s->levels) g_level_target = -1;
        else return tap(level < g_level_target ? kVpadRight : kVpadLeft);
    }
    if (g_value_steps) {
        tap(g_value_steps < 0 ? kVpadLeft : kVpadRight);
        g_value_steps += g_value_steps < 0 ? 1 : -1;
        return;
    }
    if (g_confirm) tap(kVpadA);
    g_target = -1, g_confirm = false;
}

// ---- Yes / No prompts ----
// FUN_020435B4(&cursor, buttons, left mask, right mask, ...) is the game's two-choice step (11 callers; the system
// message's Yes / No via FUN_020E28A0, the DLC prompt, ...): cursor 0 = Yes (left), 1 = No (right), no wrap; buttons
// 0x10 decides, 0x20 cancels; sound 3 on a move. Hover sets the cursor (with that sound); a click on a button sets it
// and adds "decide" to this step's buttons. Two layouts, told apart by the masks: left / right (4 / 8) - the message
// box with Yes and No side by side (measured on the DLC prompt "Connect to get downloadable content?"); otherwise Yes
// above No at the right of the help box (measured on Character Select's "Use this character?").
constexpr Box kYesSide{690, 595, 925, 668}, kNoSide{1000, 595, 1230, 668};
constexpr Box kYesStacked{1250, 870, 1468, 922}, kNoStacked{1250, 936, 1468, 992};
float g_yn_x = -1, g_yn_y = -1;
uint32_t g_two_choice_logged = 0;

// Hover / click on a Yes / No pair whose cursor (u32: 0 Yes, 1 No) is at `cursor`. Returns true for a click on a
// button (the caller confirms it).
bool yes_no(ppc::Context& ctx, uint8_t* base, uint32_t cursor, bool side) {
    float x = 0, y = 0;
    if (!cursor || !g_pointer.tap || !menu_pointer(x, y)) return false;
    const auto in = [&](const Box& b) { return x >= b.x0 && x <= b.x1 && y >= b.y0 && y <= b.y1; };
    const int item = in(side ? kYesSide : kYesStacked) ? 0 : in(side ? kNoSide : kNoStacked) ? 1 : -1;
    const bool moved = x != g_yn_x || y != g_yn_y;
    g_yn_x = x, g_yn_y = y;
    const auto select = [&] {
        if (static_cast<int>(ppc::ld32(base, cursor)) == item) return;
        ppc::st32(base, cursor, static_cast<uint32_t>(item));
        const ppc::Context saved = ctx;
        ctx.r[1] -= 0x40;
        ctx.r[3] = ctx.r[1] + 0x20;
        ctx.r[4] = 3;  // the cursor sound
        sub_020BFC74(ctx, base);
        ctx = saved;
    };
    if (item >= 0 && moved) select();
    bool decide = false;
    while (menu_click())
        if (item >= 0) select(), decide = true;  // beside the buttons: nothing
    return decide;
}

// ---- the lobby's option scrolls ----
// FUN_0221C614(sLobby, list, buttons) steps the option list of nearly every facility (Market / Guild Store Buy / Sell,
// Armory and Smithy categories, Farm, Stockpile, Canteen, Arena...; found with MH3U_MENU_LEARN): list +4 cursor (s16),
// +0xE count, +8 a bit per greyed option; up / down step the cursor (FUN_020432BC), buttons 0x10 decide (returns 1),
// 0x20 cancel. Hover sets the cursor (with the cursor sound), a click sets it and adds "decide" to this step's
// buttons. The scroll is drawn in the same place everywhere: rows x 1280-1740, centred at y 272 + 54 * row (window
// session screenshots, 2026-11-03); the Arena (lobby mode 29) has plaques 127 apart instead. Lists longer than
// kOptionRows are taken as pages of that many. Lobby mode 0: the step also runs under a button prompt (the Canteen's
// "Eat a Meal"): no rows there, a click stays A.
constexpr int kOptionRows = 6;
float g_opt_x = -1, g_opt_y = -1;

PPC_FUNC(lobby_options) {
    const uint32_t list = static_cast<uint32_t>(ctx.r[4]), lobby = ppc::ld32(base, kSLobby);
    const uint8_t mode = lobby ? ppc::ld8(base, lobby + 0x5450) : 0;
    float x = 0, y = 0;
    if (list && mode && g_pointer.tap && menu_pointer(x, y)) {
        // where the scroll is drawn depends on the facility: told apart by the caller (MH3U_MENU_PROBE lines)
        const bool arena = mode == 29;
        float x0 = arena ? 1290.0f : 1280.0f, x1 = arena ? 1720.0f : 1740.0f, top = arena ? 277.0f : 272.0f,
              step = arena ? 127.0f : 54.0f, half = arena ? 40.0f : 25.0f;
        if (ctx.lr == 0x021C4B48) x0 = 1416, x1 = 1830, top = 306, step = 52;  // house: Furnishings / Gallery / DLC
        if (ctx.lr == 0x021C4834) x0 = 202, x1 = 634, top = 274, step = 53;   // house Furnishings: Place / Remove (left)
        if (ctx.lr == 0x021C436C || ctx.lr == 0x021C4424) x0 = 202, x1 = 634, top = 274, step = 53;  // Remove's menus
        const int count = static_cast<int16_t>(ppc::ld16(base, list + 0xE)), cursor = static_cast<int16_t>(ppc::ld16(base, list + 4));
        int item = -1;
        if (x >= x0 && x <= x1)
            for (int r = 0; r < kOptionRows; ++r)
                if (y >= top + r * step - half && y <= top + r * step + half) item = cursor / kOptionRows * kOptionRows + r;
        if (item >= count) item = -1;
        const bool moved = x != g_opt_x || y != g_opt_y;
        g_opt_x = x, g_opt_y = y;
        const auto select = [&] {
            if (cursor == item) return;
            ppc::st16(base, list + 4, static_cast<uint16_t>(item));
            const ppc::Context saved = ctx;
            ctx.r[1] -= 0x40;
            ctx.r[3] = ctx.r[1] + 0x20;
            ctx.r[4] = 3;  // the cursor sound
            sub_020BFC74(ctx, base);
            ctx = saved;
        };
        static uint32_t last_list = 0;
        static int last_set = -1;
        if (g_probe && list == last_list && last_set >= 0)  // did the value set last call stick?
            std::printf("menu probe: frame %u options list %08X (from %08X) set %d, now %d\n", g_frame, list, ctx.lr, last_set, cursor);
        last_list = list, last_set = -1;
        if (g_probe && moved)
            std::printf("menu probe: frame %u options list %08X (from %08X) mode %u pointer %.0f,%.0f -> row %d (cursor %d, count %d)\n",
                        g_frame, list, ctx.lr, mode, x, y, item, cursor, count);
        if (item >= 0 && moved) select(), last_set = item;
        while (menu_click())
            if (item >= 0) select(), ctx.r[5] |= 0x10;  // beside the rows: nothing
    }
    sub_0221C614__orig(ctx, base);
}

// ---- lobby lists and grids with their own cursor ----
// Each keeps its cursor in guest memory and draws its highlight from it every frame, so hover writes the cursor (with
// the cursor sound) and a click on an item writes it and confirms (these steps take no buttons argument: the confirm
// is one A press, menu_confirm). Cells are centred at (x0 + col * dx, y0 + row * dy) in 1920x1080 (window session
// screenshots, 2026-11-03). Pages stay on the d-pad / shoulder buttons (the wheel turns them).
struct Cells {
    float x0, y0, dx, dy, hw, hh;
    int cols, rows;
};

struct Hover {
    int item = -1;
    bool moved = false, click = false;
};

Hover hover_cells(const Cells& c, int count, float& last_x, float& last_y) {
    Hover h;
    g_lobby_menu = true;  // some of these lists read the lobby directions only on a press: keep the pointer on
    float x = 0, y = 0;
    if (!g_pointer.tap || !menu_pointer(x, y)) return h;
    for (int r = 0; r < c.rows; ++r)
        for (int k = 0; k < c.cols; ++k)
            if (std::fabs(x - (c.x0 + k * c.dx)) <= c.hw && std::fabs(y - (c.y0 + r * c.dy)) <= c.hh) h.item = r * c.cols + k;
    if (h.item >= count) h.item = -1;
    h.moved = x != last_x || y != last_y;
    last_x = x, last_y = y;
    while (menu_click()) h.click = h.click || h.item >= 0;  // beside the items: nothing
    return h;
}

void cursor_sound(ppc::Context& ctx, uint8_t* base) {
    const ppc::Context saved = ctx;
    ctx.r[1] -= 0x40;
    ctx.r[3] = ctx.r[1] + 0x20;
    ctx.r[4] = 3;
    sub_020BFC74(ctx, base);
    ctx = saved;
}

// Writes item into a 16-bit cursor (or a column / row pair for grids) when the pointer moved onto it or clicked it.
void hover_apply(ppc::Context& ctx, uint8_t* base, const Hover& h, int cursor, uint32_t at, uint32_t row_at = 0, int cols = 1) {
    if (h.item < 0 || !(h.moved || h.click)) return;
    if (h.item != cursor) {
        if (row_at) {
            ppc::st16(base, at, static_cast<uint16_t>(h.item % cols));
            ppc::st16(base, row_at, static_cast<uint16_t>(h.item / cols));
        } else {
            ppc::st16(base, at, static_cast<uint16_t>(h.item));
        }
        cursor_sound(ctx, base);
    }
    if (h.click) menu_confirm();
}

// Market / Guild Store buy list, FUN_02217F10(sLobby): in state sLobby+0x5551 == 0, row +0x5558 of +0x5564 on the
// page +0x555A; 8 rows at x 1190-1750, centred at y 273 + 54 * row.
float g_shop_x = -1, g_shop_y = -1;

PPC_FUNC(shop_list) {
    const uint32_t lobby = static_cast<uint32_t>(ctx.r[3]);
    if (lobby && ppc::ld8(base, lobby + 0x5551) == 0) {
        const int count = static_cast<int16_t>(ppc::ld16(base, lobby + 0x5564));
        const int cursor = static_cast<int16_t>(ppc::ld16(base, lobby + 0x5558));
        hover_apply(ctx, base, hover_cells({1470, 273, 0, 54, 280, 25, 1, 8}, count, g_shop_x, g_shop_y), cursor,
                    lobby + 0x5558);
    }
    sub_02217F10__orig(ctx, base);
}

// Item box grid, FUN_021F1140(sLobby, box): 10 x 10 cells, column box+0xC, row box+0xE (page box+2 on L / R);
// cells centred at x 1236 + 52.2 * col, y 222 + 51.9 * row.
float g_box_x = -1, g_box_y = -1;

PPC_FUNC(box_grid) {
    const uint32_t box = static_cast<uint32_t>(ctx.r[4]);
    if (box) {
        const int cursor = static_cast<int16_t>(ppc::ld16(base, box + 0xE)) * 10 + static_cast<int16_t>(ppc::ld16(base, box + 0xC));
        hover_apply(ctx, base, hover_cells({1236, 222, 52.2f, 51.9f, 25, 25, 10, 10}, 100, g_box_x, g_box_y), cursor,
                    box + 0xC, box + 0xE, 10);
    }
    sub_021F1140__orig(ctx, base);
}

// Quest Counter quest list, FUN_021E3978(sLobby, list, ...): in state list+1 == 0 with no page turn running
// (list+0x1AC == 0xFF), row list+0x16 of list+0x18; rows at x 1295-1755, centred at y 336 + 54 * row. Also the
// Arena's list.
float g_quest_x = -1, g_quest_y = -1;

PPC_FUNC(quest_list) {
    const uint32_t list = static_cast<uint32_t>(ctx.r[4]);
    if (list && ppc::ld8(base, list + 1) == 0 && static_cast<int16_t>(ppc::ld16(base, list + 0x1AC)) == 0xFF) {
        const int count = static_cast<int16_t>(ppc::ld16(base, list + 0x18));
        const int cursor = static_cast<int16_t>(ppc::ld16(base, list + 0x16));
        hover_apply(ctx, base, hover_cells({1525, 336, 0, 54, 230, 25, 1, 8}, count, g_quest_x, g_quest_y), cursor,
                    list + 0x16);
    }
    sub_021E3978__orig(ctx, base);
}

// Item box Store Items (pouch list), FUN_021F1B00(sLobby, list, ...): in state list+1 == 0, row list+0x14 of 8 (page
// list+0x16 on left / right); rows at x 1416-1824, centred at y 308 + 51.84 * row.
float g_store_x = -1, g_store_y = -1;

PPC_FUNC(store_list) {
    const uint32_t list = static_cast<uint32_t>(ctx.r[4]);
    if (list && ppc::ld8(base, list + 1) == 0) {
        const int cursor = static_cast<int16_t>(ppc::ld16(base, list + 0x14));
        hover_apply(ctx, base, hover_cells({1620, 308, 0, 51.84f, 204, 25, 1, 8}, 8, g_store_x, g_store_y), cursor, list + 0x14);
    }
    sub_021F1B00__orig(ctx, base);
}

// Smithy / Armory equipment list, FUN_021CE834(sLobby): in state sLobby+0x5829 == 0 outside the skills view
// (+0x5C60 != 4), row +0x5944 of +0x5950 (page +0x594A on left / right). After a row change the game refreshes the
// comparison and the model (FUN_021CCDD4(sLobby), FUN_021CDCF0(sLobby, sLobby+0x5828)) - so does the hover. Rows at
// x 1190-1752, centred at y 274 + 53.8 * row.
float g_smithy_x = -1, g_smithy_y = -1;

PPC_FUNC(smithy_list) {
    const uint32_t lobby = static_cast<uint32_t>(ctx.r[3]);
    if (lobby && ppc::ld8(base, lobby + 0x5829) == 0 && ppc::ld8(base, lobby + 0x5C60) != 4 &&
        static_cast<int16_t>(ppc::ld16(base, lobby + 0x5DC8)) <= 0) {
        const int count = static_cast<int16_t>(ppc::ld16(base, lobby + 0x5950));
        const int cursor = static_cast<int16_t>(ppc::ld16(base, lobby + 0x5944));
        const Hover h = hover_cells({1471, 274, 0, 53.8f, 281, 25, 1, 8}, count, g_smithy_x, g_smithy_y);
        if (h.item >= 0 && (h.moved || h.click) && h.item != cursor) {
            ppc::st16(base, lobby + 0x5944, static_cast<uint16_t>(h.item));
            cursor_sound(ctx, base);
            const ppc::Context saved = ctx;
            ctx.r[3] = lobby;
            sub_021CCDD4(ctx, base);
            ctx.r[3] = lobby;
            ctx.r[4] = lobby + 0x5828;
            sub_021CDCF0(ctx, base);
            ctx = saved;
        }
        if (h.click) menu_confirm();
    }
    sub_021CE834__orig(ctx, base);
}

// Quest Counter level plaques, FUN_021E55A8(sLobby, list): row list+0x38 of list+0x3A; plaques at x 1291-1757,
// centred at y 274 + 129.6 * row (Lv 1, Lv 2, ..., Event Quests).
float g_plaque_x = -1, g_plaque_y = -1;

PPC_FUNC(quest_plaques) {
    const uint32_t list = static_cast<uint32_t>(ctx.r[4]);
    if (list) {
        const int count = static_cast<int16_t>(ppc::ld16(base, list + 0x3A));
        const int cursor = static_cast<int16_t>(ppc::ld16(base, list + 0x38));
        hover_apply(ctx, base, hover_cells({1524, 274, 0, 129.6f, 233, 45, 1, 6}, count, g_plaque_x, g_plaque_y), cursor,
                    list + 0x38);
    }
    sub_021E55A8__orig(ctx, base);
}

// House furnishing list (Place / Remove), FUN_021EF1A8(sLobby, buttons, browse only): in state sLobby+0x6EB8 == 0,
// row (u8) +0x6EB9 of +0x6EC4; drawn like the item box's Store Items list (x 1416-1824, y 308 + 51.84 * row). Its
// buttons argument takes "decide" (0x10) for a click.
float g_furn_x = -1, g_furn_y = -1;

PPC_FUNC(furnishing_list) {
    const uint32_t lobby = static_cast<uint32_t>(ctx.r[3]);
    if (lobby && ctx.r[5] == 0 && ppc::ld8(base, lobby + 0x6EB8) == 0) {
        const int count = static_cast<int16_t>(ppc::ld16(base, lobby + 0x6EC4));
        const int cursor = ppc::ld8(base, lobby + 0x6EB9);
        const Hover h = hover_cells({1620, 308, 0, 51.84f, 204, 25, 1, 8}, count, g_furn_x, g_furn_y);
        if (h.item >= 0 && (h.moved || h.click) && h.item != cursor) {
            base[lobby + 0x6EB9] = static_cast<uint8_t>(h.item);
            cursor_sound(ctx, base);
        }
        if (h.click) ctx.r[4] |= 0x10;
    }
    sub_021EF1A8__orig(ctx, base);
}

// Canteen ingredient list, FUN_021D8C8C(sLobby, list): row list+0x1E of list+0x28 (page list+0x2A); rows at
// x 1186-1757, centred at y 276.5 + 54 * row (found with MH3U_MENU_HUNT_LIVE: sLobby+0x83CB = list+0x1F).
float g_canteen_x = -1, g_canteen_y = -1;

PPC_FUNC(canteen_list) {
    const uint32_t list = static_cast<uint32_t>(ctx.r[4]);
    if (list) {
        const int count = static_cast<int16_t>(ppc::ld16(base, list + 0x28));
        const int cursor = static_cast<int16_t>(ppc::ld16(base, list + 0x1E));
        hover_apply(ctx, base, hover_cells({1471, 276.5f, 0, 54, 285, 25, 1, 6}, count, g_canteen_x, g_canteen_y), cursor,
                    list + 0x1E);
    }
    sub_021D8C8C__orig(ctx, base);
}

// Quest Supply Box, FUN_02863258(player?, state): open while state+0xBAE != 0; the cursor is the byte +0x6BF of the
// quest's supply data (FUN_02166A14(sQuest), items at +0x6C0, 4 bytes each): 8 x 5 cells, left / right 1, up / down 8
// (FUN_025F5318 shows the item at it). Cells centred at x 1288 + 51.9 * col, y 237 + 52 * row.
constexpr uint32_t kSQuest = 0x1031537C;
float g_supply_x = -1, g_supply_y = -1;

PPC_FUNC(supply_box) {
    const uint32_t state = static_cast<uint32_t>(ctx.r[4]), quest = ppc::ld32(base, kSQuest);
    if (state && quest && ppc::ld8(base, state + 0xBAE)) {
        const ppc::Context saved = ctx;
        ctx.r[3] = quest;
        sub_02166A14(ctx, base);
        const uint32_t box = static_cast<uint32_t>(ctx.r[3]);
        ctx = saved;
        if (box) {
            const int cursor = static_cast<int8_t>(ppc::ld8(base, box + 0x6BF));
            const Hover h = hover_cells({1288, 237, 51.9f, 52, 25, 25, 8, 5}, 40, g_supply_x, g_supply_y);
            if (h.item >= 0 && (h.moved || h.click) && h.item != cursor) {
                base[box + 0x6BF] = static_cast<uint8_t>(h.item);
                cursor_sound(ctx, base);
            }
            if (h.click) menu_confirm();
        }
    }
    sub_02863258__orig(ctx, base);
}

// uIDLobbyOnline's step FUN_026ACF64(widget): three choices, cursor +0x408 (copied to +0x404, drawn change at +0x40C),
// locks by +0x42C (MH3U_MENU_HUNT_LIVE found +0x404..+0x40C moving on the Ferry). Which rows of the Ferry's
// Destination list they are is not known yet: logged with MH3U_MENU_PROBE, the pointer kept on.
PPC_FUNC(online_choice) {
    const uint32_t w = static_cast<uint32_t>(ctx.r[3]);
    g_lobby_menu = true;
    static uint32_t last = 0xFFFFFFFF;
    float x = 0, y = 0;
    if (w && g_probe && menu_pointer(x, y) && ppc::ld32(base, w + 0x408) != last) {
        last = ppc::ld32(base, w + 0x408);
        std::printf("menu probe: frame %u online choice %u (+404 %u, lock %u) pointer %.0f,%.0f\n", g_frame, last,
                    ppc::ld32(base, w + 0x404), ppc::ld32(base, w + 0x42C), x, y);
    }
    sub_026ACF64__orig(ctx, base);
}

// Farm Name List (lobby mode 22), FUN_022101F4(sLobby): in state sLobby+0x7FC0 == 1, row +0x7FC2 of +0x7FCA (page
// +0x7FC6 on left / right); drawn like the option scrolls (x 1280-1740, y 272 + 54 * row, 5 rows).
float g_names_x = -1, g_names_y = -1;

PPC_FUNC(farm_names) {
    const uint32_t lobby = static_cast<uint32_t>(ctx.r[3]);
    if (lobby && ppc::ld8(base, lobby + 0x7FC0) == 1) {
        const int count = static_cast<int16_t>(ppc::ld16(base, lobby + 0x7FCA));
        const int cursor = static_cast<int16_t>(ppc::ld16(base, lobby + 0x7FC2));
        hover_apply(ctx, base, hover_cells({1510, 272, 0, 54, 230, 25, 1, 5}, count, g_names_x, g_names_y), cursor,
                    lobby + 0x7FC2);
    }
    sub_022101F4__orig(ctx, base);
}

// Farm Felyne tabs (lobby mode 8: View Farm Status, and Cultivate's "Assign which Felyne"), FUN_021D431C(sLobby,
// view = sLobby+0x7FEC): the Felyne shown, view+0x38 of sUserData+0x5E78 (left / right, touch tabs 0x35-0x37); after
// a change the game redraws the status panel (FUN_021D3E1C(sLobby, view)) - so does the hover. The tabs sit above the
// panel: the first centred at 1157,217 (window session screenshot, 2026-11-03), the next ones taken as 250 apart.
constexpr uint32_t kSUserData = 0x10315F50;
float g_farm_x = -1, g_farm_y = -1;

PPC_FUNC(farm_stats) {
    const uint32_t lobby = static_cast<uint32_t>(ctx.r[3]), view = static_cast<uint32_t>(ctx.r[4]);
    const uint32_t user = ppc::ld32(base, kSUserData);
    if (lobby && view && user) {
        const int count = ppc::ld8(base, user + 0x5E78);
        const int cursor = static_cast<int16_t>(ppc::ld16(base, view + 0x38));
        const Hover h = hover_cells({1157, 217, 250, 0, 120, 35, 3, 1}, count, g_farm_x, g_farm_y);
        if (h.item >= 0 && (h.moved || h.click) && h.item != cursor) {
            ppc::st16(base, view + 0x38, static_cast<uint16_t>(h.item));
            cursor_sound(ctx, base);
            const ppc::Context saved = ctx;
            ctx.r[3] = lobby;
            ctx.r[4] = view;
            sub_021D3E1C(ctx, base);
            ctx = saved;
        }
    }
    sub_021D431C__orig(ctx, base);
}

// Farm Cultivate settings, FUN_021D4420(sLobby, view): in state view+1 == 1, row view+0x3A (0 Source, 1 Enrich,
// 2 Cycles; up / down), drawn in the status panel: Source centred at y 420, Enrich 551, Cycles 629, x 1090-1718.
// Source and Enrich open a list on A; Cycles changes with left / right: a click on its left arrow (x < 1400) or right
// arrow (x > 1660) is that press. Clicks elsewhere on a row decide.
float g_cult_x = -1, g_cult_y = -1;

PPC_FUNC(farm_cultivate) {
    const uint32_t view = static_cast<uint32_t>(ctx.r[4]);
    if (view && ppc::ld8(base, view + 1) == 1 && g_pointer.tap) {
        static constexpr float kRowY[3] = {420, 551, 629};
        float x = 0, y = 0;
        g_lobby_menu = true;
        if (menu_pointer(x, y)) {
            int item = -1;
            for (int r = 0; r < 3; ++r)
                if (x >= 1090 && x <= 1718 && std::fabs(y - kRowY[r]) <= 28) item = r;
            const bool moved = x != g_cult_x || y != g_cult_y;
            g_cult_x = x, g_cult_y = y;
            bool click = false;
            while (menu_click()) click = click || item >= 0;
            const int cursor = static_cast<int16_t>(ppc::ld16(base, view + 0x3A));
            if (item >= 0 && (moved || click) && item != cursor) {
                ppc::st16(base, view + 0x3A, static_cast<uint16_t>(item));
                cursor_sound(ctx, base);
            }
            if (click) {
                if (item == 2 && x < 1400) g_pointer.tap(kVpadLeft);
                else if (item == 2 && x > 1660) g_pointer.tap(kVpadRight);
                else if (item != 2) menu_confirm();
            }
        }
    }
    sub_021D4420__orig(ctx, base);
}

// Crop Box (Farm) and Stockpile, FUN_021EFA70(sLobby): state sLobby+0x693C. State 0: the menu at the left (Take Crops,
// Send to Item Box, Exit), cursor (u8) +0x693E; with an empty box (+0x6958 == 0) only Exit. Rows at x 202-643, centred
// at y 360 + 54 * row. States 1 / 2: the box grid, column (u8) +0x693D of +0x6974, row +0x694E of +0x6976, the cell
// +0x694C = row * columns + column; drawn like the item box grid (x 1236 + 52.2 * col, y 222 + 52 * row).
float g_crop_x = -1, g_crop_y = -1;

PPC_FUNC(crop_box) {
    const uint32_t lobby = static_cast<uint32_t>(ctx.r[3]);
    if (lobby) {
        const uint8_t state = ppc::ld8(base, lobby + 0x693C);
        if (state == 0) {
            const int cursor = ppc::ld8(base, lobby + 0x693E);
            Hover h = hover_cells({422, 360, 0, 54, 220, 25, 1, 3}, 3, g_crop_x, g_crop_y);
            if (h.item >= 0 && h.item != 2 && ppc::ld32(base, lobby + 0x6958) == 0) h.item = -1, h.click = false;
            if (h.item >= 0 && (h.moved || h.click) && h.item != cursor) {
                base[lobby + 0x693E] = static_cast<uint8_t>(h.item);
                cursor_sound(ctx, base);
            }
            if (h.click) menu_confirm();
        } else if (state == 1 || state == 2) {
            const int cols = static_cast<int16_t>(ppc::ld16(base, lobby + 0x6974));
            const int rows = static_cast<int16_t>(ppc::ld16(base, lobby + 0x6976));
            if (cols > 0 && rows > 0) {
                const int col = ppc::ld8(base, lobby + 0x693D), row = static_cast<int16_t>(ppc::ld16(base, lobby + 0x694E));
                const Hover h = hover_cells({1236, 222, 52.2f, 52, 25, 25, cols, rows}, cols * rows, g_crop_x, g_crop_y);
                if (h.item >= 0 && (h.moved || h.click) && h.item != row * cols + col) {
                    base[lobby + 0x693D] = static_cast<uint8_t>(h.item % cols);
                    ppc::st16(base, lobby + 0x694E, static_cast<uint16_t>(h.item / cols));
                    ppc::st16(base, lobby + 0x694C, static_cast<uint16_t>(h.item));
                    cursor_sound(ctx, base);
                }
                if (h.click) menu_confirm();
            }
        }
    }
    sub_021EFA70__orig(ctx, base);
}

// Ferry Destination list, uIDLobbyOnline state 9 = FUN_026AD984(widget): six entries (0 Moga Village, 1 / 2 Port
// Tanzia Solo Marina / Tavern, 3 / 4 Multi Marina / Tavern, 5 Network Mode); cursor +0x408, copied to +0x404, the
// entry last drawn as selected +0x40C. Greyed: +0x400 (where you are) 1 -> 0, 2 -> 1 and 2, 3 -> 3 and 4; without the
// unlock flag 0x27F (FUN_0219C830(sUserData, 0x27F)) 2 and 4. The step redraws only on up / down, so the hover redraws
// like it: FUN_026ACBB8(widget, entry +0x2E4[i], state) - the new one 1 (0x24 greyed), the old one 0 (3 greyed).
// A greyed entry can be pointed at (as with the d-pad); deciding on it only plays the buzzer. Rows at x 1416-1795,
// centred at y 269 (Moga Village), 429, 489, 612, 671, 758 (window session screenshot, 2026-11-03).
float g_ferry_x = -1, g_ferry_y = -1;

PPC_FUNC(ferry_list) {
    const uint32_t w = static_cast<uint32_t>(ctx.r[3]);
    const uint32_t user = ppc::ld32(base, kSUserData);
    float x = 0, y = 0;
    g_lobby_menu = true;
    if (w && user && g_pointer.tap && menu_pointer(x, y)) {
        static constexpr float kRowY[6] = {269, 429, 489, 612, 671, 758};
        int item = -1;
        for (int r = 0; r < 6; ++r)
            if (x >= 1416 && x <= 1795 && std::fabs(y - kRowY[r]) <= 26) item = r;
        const bool moved = x != g_ferry_x || y != g_ferry_y;
        g_ferry_x = x, g_ferry_y = y;
        bool click = false;
        while (menu_click()) click = click || item >= 0;
        const int cursor = static_cast<int>(ppc::ld32(base, w + 0x404));
        if (item >= 0 && (moved || click) && item != cursor) {
            const ppc::Context saved = ctx;
            ctx.r[3] = user;
            ctx.r[4] = 0x27F;
            sub_0219C830(ctx, base);
            const bool unlocked = static_cast<uint32_t>(ctx.r[3]) != 0;
            ctx = saved;
            const uint32_t where = ppc::ld32(base, w + 0x400);
            const auto greyed = [&](int i) {
                return (where == 1 && i == 0) || (where == 2 && (i == 1 || i == 2)) || (where == 3 && (i == 3 || i == 4)) ||
                       (!unlocked && (i == 2 || i == 4));
            };
            const auto draw = [&](int i, uint32_t state) {
                const ppc::Context keep = ctx;
                ctx.r[3] = w;
                ctx.r[4] = ppc::ld32(base, w + 0x2E4 + 4 * static_cast<uint32_t>(i));
                ctx.r[5] = state;
                sub_026ACBB8(ctx, base);
                ctx = keep;
            };
            const int old = static_cast<int>(ppc::ld32(base, w + 0x40C));
            ppc::st32(base, w + 0x408, static_cast<uint32_t>(item));
            ppc::st32(base, w + 0x404, static_cast<uint32_t>(item));
            draw(item, greyed(item) ? 0x24 : 1);
            if (old >= 0 && old < 6 && old != item) draw(old, greyed(old) ? 3 : 0);
            ppc::st32(base, w + 0x40C, static_cast<uint32_t>(item));
            cursor_sound(ctx, base);
        }
        if (click) menu_confirm();
    }
    sub_026AD984__orig(ctx, base);
}

// House furnishing position picker ("Select a position"), FUN_021C3DCC(sLobby, pick): in state pick+2 == 1, the
// cursor pick+0xCA of pick+0xC4 free spots, pick+0xB3[i] the spots' numbers (left / right). After a change the game
// moves the camera (FUN_021C3C5C(sLobby, pick)) and the orange marker (FUN_0222B34C(sLobbyTarget, spot)) - so does
// the hover. The spots are markers standing in the room, drawn from fixed camera angles, so each spot shows at one
// place on screen: kPickerSpots, measured with MH3U_MENU_PICKER_CAL=1 (the picker steps through its free spots by
// itself, a screenshot of each). The pointer within reach of a listed spot selects it; a click decides.
constexpr uint32_t kSLobbyTarget = 0x103A02D0;
struct PickerSpot {
    uint8_t area, room, spot;
    float x, y;
};
// The marker's centre on screen; the Moga Village house (area 26, room 1) has spots 0-2 (FUN_021C32C4: first 0,
// count 3), measured 2026-11-03 (two passes, within 8 px).
constexpr PickerSpot kPickerSpots[] = {
    {26, 1, 0, 475, 658},
    {26, 1, 1, 827, 515},
    {26, 1, 2, 1415, 738},
};
bool g_picker_cal = false;
float g_pick_x = -1, g_pick_y = -1;

void picker_select(ppc::Context& ctx, uint8_t* base, uint32_t lobby, uint32_t pick, int item) {
    ppc::st16(base, pick + 0xCA, static_cast<uint16_t>(item));
    const ppc::Context saved = ctx;
    ctx.r[3] = lobby;
    ctx.r[4] = pick;
    sub_021C3C5C(ctx, base);
    ctx.r[3] = ppc::ld32(base, kSLobbyTarget);
    ctx.r[4] = ppc::ld8(base, pick + 0xB3 + static_cast<uint32_t>(item));
    if (ctx.r[3]) sub_0222B34C(ctx, base);
    ctx = saved;
}

// Place (FUN_021C3DCC) and Remove (FUN_021C4304, the occupied spots; a spot can go when pick+0xBB[i] has bit 2) step
// the same struct in state 1.
void picker_hover(ppc::Context& ctx, uint8_t* base) {
    const uint32_t lobby = static_cast<uint32_t>(ctx.r[3]), pick = static_cast<uint32_t>(ctx.r[4]);
    if (lobby && pick && ppc::ld8(base, pick + 2) == 1) {
        const int count = static_cast<int16_t>(ppc::ld16(base, pick + 0xC4));
        const int cursor = static_cast<int16_t>(ppc::ld16(base, pick + 0xCA));
        const uint8_t area = ppc::ld8(base, lobby + 0x5451), room = ppc::ld8(base, lobby + 0x5452);
        g_lobby_menu = true;
        if (g_picker_cal) {  // one spot every 60 frames, the screenshot 45 frames in (the camera settled)
            static uint32_t start = 0, last_pick = 0;
            if (pick != last_pick || g_frame - start > 60u * static_cast<uint32_t>(count) + 120) start = g_frame, last_pick = pick;
            const uint32_t t = g_frame - start;
            const int item = static_cast<int>(t / 60);
            if (item < count) {
                if (t % 60 == 0) picker_select(ctx, base, lobby, pick, item);
                if (t % 60 == 45) {
                    std::printf("picker cal: area %u room %u spot %u (%d of %d) frame %u\n", area, room,
                                ppc::ld8(base, pick + 0xB3 + static_cast<uint32_t>(item)), item, count, g_frame);
                    std::fflush(stdout);
                    capture_frame();
                }
            }
        } else if (g_pointer.tap) {
            float x = 0, y = 0;
            if (menu_pointer(x, y)) {
                int item = -1;
                float best = 70.0f * 70.0f;  // within 70 px of a marker
                for (int i = 0; i < count && i < 8; ++i) {
                    const uint8_t spot = ppc::ld8(base, pick + 0xB3 + static_cast<uint32_t>(i));
                    for (const PickerSpot& s : kPickerSpots)
                        if (s.x >= 0 && s.area == area && s.room == room && s.spot == spot) {
                            const float d = (x - s.x) * (x - s.x) + (y - s.y) * (y - s.y);
                            if (d < best) best = d, item = i;
                        }
                }
                const bool moved = x != g_pick_x || y != g_pick_y;
                g_pick_x = x, g_pick_y = y;
                bool click = false;
                while (menu_click()) click = click || item >= 0;
                if (item >= 0 && (moved || click) && item != cursor) {
                    picker_select(ctx, base, lobby, pick, item);
                    cursor_sound(ctx, base);
                }
                if (click) menu_confirm();
            }
        }
    }
}

PPC_FUNC(furnishing_picker) {
    picker_hover(ctx, base);
    sub_021C3DCC__orig(ctx, base);
}

PPC_FUNC(furnishing_remove) {
    picker_hover(ctx, base);
    sub_021C4304__orig(ctx, base);
}

// The lobby's stacked Yes / No, FUN_02043308(&cursor, buttons, up mask, down mask, sound): cursor u32 0 Yes, 1 No;
// buttons 0x10 decides. Drawn right of the help box (the Farm's "Change names?"): Yes centred at 926,940, No at
// 926,996. Hover sets the cursor, a click sets it and decides.
float g_stack_x = -1, g_stack_y = -1;

PPC_FUNC(stacked_yes_no) {
    const uint32_t cursor = static_cast<uint32_t>(ctx.r[3]);
    g_prompt = true;
    if (g_learn) g_callers.insert(ctx.lr);
    if (cursor) {
        const Hover h = hover_cells({926, 940, 0, 56, 115, 25, 1, 2}, 2, g_stack_x, g_stack_y);
        if (h.item >= 0 && (h.moved || h.click) && static_cast<int>(ppc::ld32(base, cursor)) != h.item) {
            ppc::st32(base, cursor, static_cast<uint32_t>(h.item));
            cursor_sound(ctx, base);
        }
        if (h.click) ctx.r[4] |= 0x10;
    }
    sub_02043308__orig(ctx, base);
}

// Abandon Quest's "Quit and return to the village?" (field menu sMenu+0x34 1, +0x52 5, +0x35 0): Yes and No side by
// side, cursor sMenu+0x204 (u32: 0 Yes, 1 No; opens on No - seen with MH3U_MENU_PROBE). Yes centred at 810,666 and No
// at 1111,666 (window session screenshot, 2026-11-03).
float g_abandon_x = -1, g_abandon_y = -1;

void abandon_prompt(ppc::Context& ctx, uint8_t* base) {
    const uint32_t menu = ppc::ld32(base, kSMenu), game = ppc::ld32(base, kSGame);
    if (!menu || !game || !ppc::ld8(base, game + 0x48) || !ppc::ld8(base, menu + 0x4C) || ppc::ld8(base, menu + 0x34) != 1 ||
        ppc::ld8(base, menu + 0x52) != 5 || ppc::ld8(base, menu + 0x35) != 0)
        return;
    const Hover h = hover_cells({810, 666, 301, 0, 102, 22, 2, 1}, 2, g_abandon_x, g_abandon_y);
    if (h.item < 0 || !(h.moved || h.click)) return;
    if (static_cast<int>(ppc::ld32(base, menu + 0x204)) != h.item) {
        ppc::st32(base, menu + 0x204, static_cast<uint32_t>(h.item));
        cursor_sound(ctx, base);
    }
    if (h.click) menu_confirm();
}

PPC_FUNC(two_choice) {
    g_two_choice_frame = g_frame;
    g_prompt = true;
    if (g_learn) g_callers.insert(ctx.lr);
    const bool side = ctx.r[5] == 4 && ctx.r[6] == 8;
    if (g_probe && g_frame - g_two_choice_logged > 60)
        std::printf("menu probe: frame %u yes/no from %08X masks %X %X\n", g_frame, ctx.lr, static_cast<uint32_t>(ctx.r[5]),
                    static_cast<uint32_t>(ctx.r[6]));
    g_two_choice_logged = g_frame;
    if (yes_no(ctx, base, static_cast<uint32_t>(ctx.r[3]), side)) ctx.r[4] |= 0x10;  // decide on the button clicked
    sub_020435B4__orig(ctx, base);
}

// The message box's choice (FUN_0261C0FC(box, text, "Yes", "No", cursor value, ...), drawn from the message box
// FUN_0261CB4C - the DLC prompt and other sMenu messages): Yes and No side by side, cursor sMenu+0x204 (the one
// FUN_020E28A0 steps). Its input does not always go through FUN_020435B4 (the DLC prompt does not), so the pair is
// handled from here: hover sets the cursor, a click sets it and presses A.
// The message box's Yes / No (FUN_0261C0FC(box, text, "Yes", "No", cursor value, ...), drawn every frame from the
// message box FUN_0261CB4C - the DLC prompt and other sMenu messages; Yes and No side by side). The value drawn is
// sMenu+0x204, but the screen steps its own copy and stores it there each frame, so writing it does not stick (seen
// on the DLC prompt): this one is driven like the lists - left / right taps until the drawn value is the button under
// the pointer, then A for a click (screen_hover with g_message_value as the cursor).
PPC_FUNC(message_choice) {
    if (g_probe && ctx.r[7] != g_message_value.load())
        std::printf("menu probe: frame %u message choice %u (from %08X)\n", g_frame, static_cast<uint32_t>(ctx.r[7]), ctx.lr);
    g_message_value = static_cast<uint32_t>(ctx.r[7]);
    if (g_learn) g_callers.insert(ctx.lr);
    g_message_frame = g_frame;
    g_prompt = true;
    sub_0261C0FC__orig(ctx, base);
}

PPC_FUNC(card_list_step) {
    g_card = static_cast<uint32_t>(ctx.r[3]);
    g_card_frame = g_frame;
    sub_0268E7DC__orig(ctx, base);
}

PPC_FUNC(make_character_step) {
    g_make = static_cast<uint32_t>(ctx.r[3]);
    g_make_frame = g_frame;
    sub_0275412C__orig(ctx, base);
}

PPC_FUNC(pad_update) {
    sub_020E0CFC__orig(ctx, base);
    ++g_frame;
    if (g_learn) learn(base);
    if (g_probe) probe(base);
    else g_callers.clear();
    if (g_hunt_live) hunt_live(base);
    if (!g_hunts.empty()) hunt(base);
    snapshot(base);
    g_analog = nullptr;
    g_screen = g_pointer.tap ? find_screen(base, g_object) : nullptr;
    if (g_screen || g_analog) g_hover_frames = kHoverFrames;  // its clicks are taken below (screen_hover, analog_hover)
    // the lobby menus run after this update in the frame: their flag counts for the next frame's decision
    if (menu_signal(base)) g_open_frames = kCloseFrames;
    else if (g_open_frames > 0) --g_open_frames;
    g_lobby_menu = false, g_prompt = false;
    const bool open = g_open_frames > 0;
    if (open != g_open && g_probe) std::printf("menu probe: frame %u pointer %s\n", g_frame, open ? "on" : "off");
    g_open = open;
    if (g_pointer.set_menu_open) g_pointer.set_menu_open(open);
    // left clicks: to the hover menu if one is up, else A
    const int clicks = g_pointer.take_clicks ? g_pointer.take_clicks() : 0;
    if (g_hover_frames.load() > 0) {
        g_clicks += clicks;
        --g_hover_frames;
    } else {
        g_clicks = 0;
        if (clicks && g_pointer.tap) g_pointer.tap(kVpadA);
    }
    screen_hover(base);
    if (g_analog) analog_hover(ctx, base, g_object);
    abandon_prompt(ctx, base);
}

}  // namespace

bool menu_pointer(float& x, float& y) {
    g_hover_frames = kHoverFrames;
    float u = 0, v = 0;
    if (!g_pointer.position || !g_pointer.position(u, v)) return false;
    x = u * 1920.0f;
    y = v * 1080.0f;
    return true;
}

bool menu_click() {
    for (int n = g_clicks.load(); n > 0;)
        if (g_clicks.compare_exchange_weak(n, n - 1)) return true;
    return false;
}

void menu_confirm() {
    if (g_pointer.tap) g_pointer.tap(0x8000);
}

void register_menu_mouse(Pointer pointer) {
    g_pointer = std::move(pointer);
    // MH3U_POINTER_SCRIPT="frame:x,y[,click];..." (headless tests): from pad update `frame` on, the pointer is at TV
    // pixel x, y, with a left click at that frame if ",click" is given - in place of the mouse
    if (char* v = nullptr; _dupenv_s(&v, nullptr, "MH3U_POINTER_SCRIPT") == 0 && v) {
        struct Step {
            uint32_t frame;
            float x, y;
            bool click;
        };
        auto steps = std::make_shared<std::vector<Step>>();
        for (char* p = v; *p;) {
            Step st{};
            char* end = nullptr;
            st.frame = static_cast<uint32_t>(std::strtoul(p, &end, 10));
            if (end == p || *end != ':') break;
            st.x = std::strtof(end + 1, &end);
            st.y = std::strtof(end + 1, &end);
            if (std::strncmp(end, ",click", 6) == 0) st.click = true, end += 6;
            steps->push_back(st);
            p = *end ? end + 1 : end;
        }
        std::free(v);
        g_pointer.position = [steps](float& u, float& v2) {
            const Step* cur = nullptr;
            for (const Step& st : *steps)
                if (st.frame <= g_frame) cur = &st;
            if (!cur) return false;
            u = cur->x / 1920.0f, v2 = cur->y / 1080.0f;
            return true;
        };
        g_pointer.take_clicks = [steps] {
            int n = 0;
            for (const Step& st : *steps) n += st.click && st.frame == g_frame;
            return n;
        };
    }
    if (char* v = nullptr; _dupenv_s(&v, nullptr, "MH3U_MENU_PROBE") == 0 && v) {
        g_probe = true;
        std::free(v);
    }
    if (char* v = nullptr; _dupenv_s(&v, nullptr, "MH3U_MENU_LEARN") == 0 && v) {
        g_learn = true;
        std::free(v);
    }
    if (char* v = nullptr; _dupenv_s(&v, nullptr, "MH3U_MENU_SNAP_MAKE") == 0 && v) {
        g_snap_make = true;
        std::free(v);
    }
    if (char* v = nullptr; _dupenv_s(&v, nullptr, "MH3U_MENU_HUNT") == 0 && v) {
        for (char* p = v; *p;) {
            Hunt h{};
            for (int k = 0; k < 4; ++k) {
                h.f[k] = static_cast<uint32_t>(std::strtoul(p, &p, 10));
                if (*p == ',') ++p;
            }
            g_hunts.push_back(h);
            if (*p == ';') ++p;
            else break;
        }
        std::free(v);
    }
    if (char* v = nullptr; _dupenv_s(&v, nullptr, "MH3U_MENU_PICKER_CAL") == 0 && v) {
        g_picker_cal = true;
        std::free(v);
    }
    if (char* v = nullptr; _dupenv_s(&v, nullptr, "MH3U_MENU_HUNT_LIVE") == 0 && v) {
        g_hunt_live = true;
        std::free(v);
    }
    if (char* v = nullptr; _dupenv_s(&v, nullptr, "MH3U_MENU_SNAP_SMENU") == 0 && v) {
        g_snap_smenu = true;
        std::free(v);
    }
    if (char* v = nullptr; _dupenv_s(&v, nullptr, "MH3U_MENU_SNAP") == 0 && v) {
        if (fopen_s(&g_snap, v, "wb") != 0) g_snap = nullptr;
        std::free(v);
    }
    ppc::set_override(0x020E0CFC, &pad_update);
    ppc::set_override(0x0221A430, &menu_directions);
    ppc::set_override(0x0275BD84, &option_page);
    ppc::set_override(0x020435B4, &two_choice);
    ppc::set_override(0x0261C0FC, &message_choice);
    ppc::set_override(0x0275412C, &make_character_step);
    ppc::set_override(0x0268E7DC, &card_list_step);
    ppc::set_override(0x0221C614, &lobby_options);
    ppc::set_override(0x02217F10, &shop_list);
    ppc::set_override(0x021F1140, &box_grid);
    ppc::set_override(0x021E3978, &quest_list);
    ppc::set_override(0x021F1B00, &store_list);
    ppc::set_override(0x021CE834, &smithy_list);
    ppc::set_override(0x021E55A8, &quest_plaques);
    ppc::set_override(0x021EF1A8, &furnishing_list);
    ppc::set_override(0x021D8C8C, &canteen_list);
    ppc::set_override(0x02863258, &supply_box);
    ppc::set_override(0x026ACF64, &online_choice);
    ppc::set_override(0x022101F4, &farm_names);
    ppc::set_override(0x021D431C, &farm_stats);
    ppc::set_override(0x02043308, &stacked_yes_no);
    ppc::set_override(0x021D4420, &farm_cultivate);
    ppc::set_override(0x021EFA70, &crop_box);
    ppc::set_override(0x026AD984, &ferry_list);
    ppc::set_override(0x021C3DCC, &furnishing_picker);
    ppc::set_override(0x021C4304, &furnishing_remove);
    if (g_learn) {
        ppc::set_override(0x020431F8, &learn_cursor_a);
        ppc::set_override(0x020432BC, &learn_cursor_b);
        ppc::set_override(0x020432C8, &learn_cursor_c);
    }
}

}  // namespace mh3u::game
