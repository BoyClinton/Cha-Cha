// The field menu's CFG tab (field_gfx.cpp; "GFX" until the user renamed it 2026-11-09): Cha-Cha's settings as a tab
// after the game's last one: Display Settings, Graphics Settings, Camera Settings and Mods (user 2026-11-10: Camera
// Settings above Mods).
// Replaces: FUN_026615A8 (uIDCommonPauch, the field menu list's step: tab plates, rows, help) and FUN_02664F74 (the
// same widget's Options screen step). Status: wip; checked headless with frame dumps (research log 2026-11-06, "Field
// menu GFX tab").
//
// The field menu (MINUS in the village / a quest) is uIDCommonPauch; it reads the view sMenu+0x34 (widget+0x2C0):
//   list: +0x58 the number of tabs (2 in the village: Hunter / Info; 3 with Online), +0x59 the tab (an s16 copy at
//     sMenu+0x84), +0x54 rows shown (s16), +0x5C the row cursor (s16), rows from +0xE4 (0x18 each: +1 the cursor's
//     row, +2 enabled, +8 the text). The widget has three tab plates (labels +0x578 / +0x57C / +0x580; the third
//     hidden while there are two tabs).
//   a screen opened from it: sMenu+0x34 1, +0x52 which (4 Options). Options: view+1 the level (1 = a value being
//     changed), rows from view+0xB0 (0x18 each: +0 the option, +1 the cursor's row, +0xC its colour), +0x1F1 the
//     cursor's option, +0x1F2 rows on the page, +0x1F4 / +0x1F6 page / pages; the widget's label texts at its list
//     +0x584 (0x161), values at +0x618 (0x186 + row), title +0x570, page plate +0x5A8; help FUN_02144960(sID, text).
//
// The CFG tab is ours: the game keeps its tab (the one we came from) while we take up / down / A in the pad update
// before the menu reads them (menu_mouse's field menu hook); left / right out of it walk the game's tab to the
// neighbour with right presses (the game's tabs wrap). Its rows, Display Settings and Graphics Settings, open the
// game's Options screen (walked to: Info, its Options row, A) with Cha-Cha's settings shown in it - one page of
// the Graphics Settings page each - and its input ours too; B leaves it back to the CFG tab.
#include "field_gfx.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "functions.h"
#include "guest_memory.h"
#include "gui_util.h"
#include "menu_mouse.h"

namespace mh3u::game {
namespace {

constexpr uint32_t kSMenu = 0x10314980;
constexpr uint32_t kView = 0x34;
constexpr uint32_t kOpen = 0x4C, kScreen = 0x52, kTabs = 0x58, kTab = 0x59, kTab16 = 0x84, kRowCount = 0x54, kRowCursor = 0x5C,
                   kRows = 0xE4;
constexpr uint32_t kRowSize = 0x18;
constexpr uint32_t kOptionsScreen = 4;
// the field / lobby menus' sounds (FUN_020BFC74(handle, id), as item_sets.cpp plays them): 0 open, 1 back, 3 the
// cursor moved, 5 a choice made, 6 a page / tab turned (0 open: the game's own A on Options plays it)
constexpr uint32_t kMoveSound = 3, kDecideSound = 5, kTabSound = 6;
// pad: +0x6C pressed this frame, +0x80 pressed or repeating (1 up 2 down 4 left 8 right 0x10 A 0x20 B), +0x11C
// another layout of this frame's presses (0x20 A, 0x40 B)
constexpr uint32_t kPressed = 0x6C, kRepeat = 0x80, kTrigger = 0x11C;
// TV pixels (frame dumps): list rows x 1412-1826 from y 286, 44 high, 52 apart; tab plates y 204-248; Options rows
// x 1415-1880 from y 284, 46 high, 52 apart, its value box from x 1690
constexpr float kRowX0 = 1412, kRowX1 = 1826, kRowY = 286, kRowH = 44, kRowStep = 52;
constexpr float kTabY0 = 204, kTabY1 = 248;
constexpr float kTabX[4][2] = {{1424, 1556}, {1564, 1684}, {1692, 1812}, {1820, 1940}};
constexpr float kOptX0 = 1415, kOptX1 = 1880, kOptY = 284, kOptH = 46, kOptStep = 52, kOptValueX = 1690;

struct TabRow {
    const char* label;
    const char* help;
};
constexpr TabRow kTabRows[] = {
    {"Display Settings", "How the game is shown: window, resolution,\r\nscaling, frame rate and VSync."},
    {"Graphics Settings", "How the game looks: Tri's look, bloom,\r\ndepth of field, lighting and reflections."},
    {"Camera Settings", "The quest camera: the Modern Camera and\r\nhow far back the camera sits."},
    {"Mods", "Cha-Cha's mods: gathering, delivery, quest\r\nmarkers, scaling, textures: on or off."},
};
constexpr int kTabRowCount = 4;

SettingsPage g_page;
std::vector<SettingRow> g_rows;  // the whole page; a set is one of its pages
bool g_gfx = false;              // the CFG tab is open
int g_tab_row = 0;               // its cursor
int g_walk = -1;                 // the game's tab to reach (leaving CFG), by right presses
// the Options screen with our settings
int g_open = -1;         // a set being opened (walking to Info's Options)
int g_open_frames = 0;   // frames spent opening (given up after a while)
int g_set = -1;          // the set shown in the Options screen (-1: the game's own Options)
int g_cur = 0;           // its cursor (row of the set)
bool g_edit = false;     // a value being changed
HoverWalk g_tab_walk, g_opt_walk;  // the pointer's hover on the tab's rows / the settings screen's rows

uint32_t text(const std::string& s) {  // guest copies, kept (text objects keep the pointer)
    static std::map<std::string, uint32_t> cache;
    auto it = cache.find(s);
    if (it == cache.end()) it = cache.emplace(s, gui::guest_string(s)).first;
    return it->second;
}

std::string help_text(const std::string& s) {  // the game's line breaks
    std::string out;
    for (char c : s) {
        if (c == '\n') out += '\r';
        out += c;
    }
    return out;
}

void sound(ppc::Context& ctx, uint8_t* base, uint32_t id) {
    const ppc::Context saved = ctx;
    ctx.r[1] -= 0x40;
    ctx.r[3] = ctx.r[1] + 0x20;  // the handle: 8 bytes on the guest stack
    ctx.r[4] = id;
    sub_020BFC74(ctx, base);
    ctx = saved;
}

int set_first(int set) {
    if (set < 0 || set >= static_cast<int>(g_page.page_starts.size())) return 0;
    return g_page.page_starts[static_cast<size_t>(set)];
}
int set_size(int set) {
    const int end = set + 1 < static_cast<int>(g_page.page_starts.size()) ? g_page.page_starts[static_cast<size_t>(set) + 1]
                                                                           : static_cast<int>(g_rows.size());
    return std::clamp(end - set_first(set), 0, 7);
}

std::string value_text(const SettingRow& r) {
    if (r.values.empty()) return "";
    return r.values[static_cast<size_t>(std::clamp(r.value, 0, static_cast<int>(r.values.size()) - 1))];
}

void step_value(ppc::Context& ctx, uint8_t* base, int step) {
    const int row = set_first(g_set) + g_cur;
    if (row < 0 || row >= static_cast<int>(g_rows.size())) return;
    SettingRow& r = g_rows[static_cast<size_t>(row)];
    const int n = static_cast<int>(r.values.size());
    if (n < 2) return;
    r.value = ((r.value + step) % n + n) % n;
    if (g_page.change) g_page.change(row, r.value);
    if (g_page.rows) g_rows = g_page.rows();
    sound(ctx, base, kMoveSound);
}

void clear_pad(uint8_t* base, uint32_t pad, uint32_t keep) {  // the menu sees only `keep` (+0x6C bits) this frame
    ppc::st32(base, pad + kPressed, ppc::ld32(base, pad + kPressed) & keep);
    ppc::st32(base, pad + kRepeat, ppc::ld32(base, pad + kRepeat) & keep);
    ppc::st32(base, pad + kTrigger, ppc::ld32(base, pad + kTrigger) & ((keep & gui::kCancel) ? 0x40u : 0u));
}

// The Options screen showing a set: our input.
bool options_input(ppc::Context& ctx, uint8_t* base, uint32_t pad) {
    const uint32_t pressed = ppc::ld32(base, pad + kPressed), dirs = pressed | ppc::ld32(base, pad + kRepeat);
    const int n = set_size(g_set);
    clear_pad(base, pad, g_edit ? 0 : gui::kCancel);  // B leaves the screen (the game's own close), unless editing
    if (g_edit) {
        if (pressed & (gui::kDecide | gui::kCancel)) {
            g_edit = false;
            sound(ctx, base, kDecideSound);
        } else if (dirs & gui::kLeft) step_value(ctx, base, -1);
        else if (dirs & gui::kRight) step_value(ctx, base, +1);
    } else if (n > 0) {
        if (dirs & gui::kUp) g_cur = (g_cur + n - 1) % n, sound(ctx, base, kMoveSound);
        else if (dirs & gui::kDown) g_cur = (g_cur + 1) % n, sound(ctx, base, kMoveSound);
        else if (pressed & gui::kDecide) g_edit = true, sound(ctx, base, kDecideSound);
    }
    // the pointer, as on the item pouch (HoverWalk): moving onto a row selects it; a click on the selected row's
    // value box steps it (left half back, right half on), elsewhere on it is A; a click on another row selects it
    if (float x = 0, y = 0; menu_pointer(x, y)) {
        int hit = -1;
        for (int i = 0; i < n; ++i)
            if (x >= kOptX0 && x <= kOptX1 && y >= kOptY + kOptStep * i && y < kOptY + kOptStep * i + kOptH) hit = i;
        if (const int d = g_edit ? 0 : g_opt_walk.step(x, y, hit, g_cur)) g_cur = (g_cur + d + n) % n, sound(ctx, base, kMoveSound);
        g_opt_walk.arrived(g_cur);
        while (menu_click()) {
            if (hit < 0) continue;
            if (hit == g_cur && x >= kOptValueX) step_value(ctx, base, x < (kOptValueX + kOptX1) / 2 ? -1 : +1);
            else if (hit == g_cur) g_edit = !g_edit, sound(ctx, base, kDecideSound);
            else if (!g_edit) g_opt_walk.click(hit);
        }
    }
    return true;
}

// Once a frame after the pad update: our tab's input, taken from the pad before the menu reads it.
bool pad_hook(ppc::Context& ctx, uint8_t* base) {
    const uint32_t menu = ppc::ld32(base, kSMenu), pad = ppc::ld32(base, gui::kSAppPad);
    if (!menu || !pad || !base[menu + kOpen]) {
        g_gfx = false, g_walk = -1, g_open = -1, g_set = -1, g_edit = false;
        return false;
    }
    const bool list = base[menu + kView] == 0;
    const bool options = base[menu + kView] == 1 && base[menu + kScreen] == kOptionsScreen;
    if (g_open >= 0 && options) {  // our set's Options screen is up
        g_set = g_open, g_open = -1, g_cur = 0, g_edit = false;
        if (g_page.rows) g_rows = g_page.rows();
    }
    if (g_set >= 0) {
        if (options) return options_input(ctx, base, pad);
        if (list) {  // back from it: the CFG tab again
            g_set = -1, g_edit = false, g_gfx = true;
        } else {
            return false;
        }
    }
    if (!list) return g_open >= 0;
    const int tabs = base[menu + kTabs], tab = base[menu + kTab];
    const uint32_t pressed = ppc::ld32(base, pad + kPressed), repeat = ppc::ld32(base, pad + kRepeat);
    if (g_open >= 0) {  // opening a set: to Info (tab 1), its Options row, A
        clear_pad(base, pad, 0);
        if (++g_open_frames > 120) {
            g_open = -1;
            return g_gfx;
        }
        if (tab != 1) {
            ppc::st32(base, pad + kPressed, gui::kRight);
            return true;
        }
        const uint32_t options_text = text("Options");
        for (int i = 0; i < static_cast<int16_t>(ppc::ld16(base, menu + kRowCount)) && i < 6; ++i) {
            const uint32_t t = ppc::ld32(base, menu + kRows + static_cast<uint32_t>(i) * kRowSize + 8);
            if (t && std::strcmp(reinterpret_cast<const char*>(base + t), reinterpret_cast<const char*>(base + options_text)) == 0) {
                if (static_cast<int16_t>(ppc::ld16(base, menu + kRowCursor)) != i) {
                    ppc::st32(base, pad + kPressed, gui::kDown);  // the game's cursor walks there (it skips greyed rows)
                } else {
                    ppc::st32(base, pad + kPressed, gui::kDecide);
                    ppc::st32(base, pad + kTrigger, 0x20);
                }
                break;
            }
        }
        return true;
    }
    if (g_walk >= 0 && !g_gfx) {  // leaving CFG: right presses until the game's tab is the one wanted
        if (tab == g_walk || g_walk >= tabs) g_walk = -1;
        else ppc::st32(base, pad + kPressed, pressed | gui::kRight);
        return false;
    }
    float x = 0, y = 0;
    const auto on_tab = [&](int i) { return x >= kTabX[i][0] && x <= kTabX[i][1] && y >= kTabY0 && y <= kTabY1; };
    if (!g_gfx) {
        if (((pressed & gui::kRight) && tab == tabs - 1) || ((pressed & gui::kLeft) && tab == 0)) {
            ppc::st32(base, pad + kPressed, pressed & ~(gui::kLeft | gui::kRight));
            ppc::st32(base, pad + kRepeat, repeat & ~(gui::kLeft | gui::kRight));
            g_gfx = true;
            sound(ctx, base, kTabSound);
            return true;
        }
        // a click on the CFG plate (looked at only while the pointer is on it: other clicks are the game's tabs')
        if (tabs < 4 && menu_pointer(x, y) && on_tab(tabs) && menu_click()) {
            g_gfx = true;
            sound(ctx, base, kTabSound);
            return true;
        }
        return false;
    }
    // ours: nothing but B reaches the menu
    const uint32_t dirs = pressed | repeat;
    clear_pad(base, pad, gui::kCancel);
    if (pressed & gui::kCancel) {
        g_gfx = false;  // B closes the menu, as on the game's tabs
        return false;
    }
    const auto open = [&](int set) {
        g_open = set, g_open_frames = 0;  // (the game's own A on Options plays its open sound)
    };
    if (dirs & gui::kUp) g_tab_row = (g_tab_row + kTabRowCount - 1) % kTabRowCount, sound(ctx, base, kMoveSound);
    else if (dirs & gui::kDown) g_tab_row = (g_tab_row + 1) % kTabRowCount, sound(ctx, base, kMoveSound);
    else if (pressed & gui::kLeft) g_gfx = false, g_walk = tabs - 1, sound(ctx, base, kTabSound);
    else if (pressed & gui::kRight) g_gfx = false, g_walk = 0, sound(ctx, base, kTabSound);
    else if (pressed & gui::kDecide) open(g_tab_row);
    // the pointer, as on the item pouch (HoverWalk): moving onto a row selects it; a click on a row opens it; a click
    // on another tab leaves to it
    if (menu_pointer(x, y)) {
        int hit = -1;
        for (int i = 0; i < kTabRowCount; ++i)
            if (x >= kRowX0 && x <= kRowX1 && y >= kRowY + kRowStep * i && y < kRowY + kRowStep * i + kRowH) hit = i;
        if (const int d = g_tab_walk.step(x, y, hit, g_tab_row))
            g_tab_row = (g_tab_row + d + kTabRowCount) % kTabRowCount, sound(ctx, base, kMoveSound);
        if (g_tab_walk.arrived(g_tab_row)) open(g_tab_row);
        while (menu_click()) {
            for (int i = 0; i < tabs; ++i)
                if (on_tab(i)) g_gfx = false, g_walk = i, sound(ctx, base, kTabSound);
            if (!g_gfx) break;
            if (hit == g_tab_row) open(hit);
            else g_tab_walk.click(hit);
        }
    }
    return true;
}

// The list step: the game's list, or ours on the CFG tab.
PPC_FUNC(list_step) {
    const uint32_t self = ctx.r[3];
    const uint32_t menu = ppc::ld32(base, kSMenu);
    if (!menu || !base[menu + kOpen] || base[menu + kView] != 0 || ppc::ld32(base, self + 0x2C0) != menu + kView) {
        sub_026615A8__orig(ctx, base);
        return;
    }
    const int tabs = base[menu + kTabs];
    const bool village = tabs == 2;
    if (const uint32_t label = ppc::ld32(base, self + 0x580); village && label) gui::set_text(ctx, base, label, text("CFG"));
    uint8_t saved[kRows + 6 * kRowSize];  // the game's fields, shown ours for the draw
    std::memcpy(saved, base + menu, sizeof saved);
    if (village) base[menu + kTabs] = 3;  // the third plate shown (our tab)
    if (g_gfx) {
        const int ours = village ? 2 : tabs;  // (with Online there is no fourth plate yet)
        base[menu + kTab] = static_cast<uint8_t>(ours);
        ppc::st16(base, menu + kTab16, static_cast<uint16_t>(ours));
        ppc::st16(base, menu + kRowCount, kTabRowCount);
        ppc::st16(base, menu + kRowCursor, static_cast<uint16_t>(g_tab_row));
        for (int i = 0; i < 6; ++i) {
            const uint32_t row = menu + kRows + static_cast<uint32_t>(i) * kRowSize;
            std::memset(base + row, 0, kRowSize);
            if (i >= kTabRowCount) continue;
            base[row + 1] = i == g_tab_row;
            base[row + 2] = 1;
            ppc::st32(base, row + 8, text(kTabRows[i].label));
        }
    }
    sub_026615A8__orig(ctx, base);
    if (g_gfx) {  // our help (after the game's)
        const ppc::Context keep = ctx;
        ctx.r[3] = ppc::ld32(base, gui::kSID), ctx.r[4] = text(kTabRows[g_tab_row].help), ctx.r[5] = 0;
        sub_02144928(ctx, base);
        ctx = keep;
    }
    std::memcpy(base + menu, saved, sizeof saved);
}

// The Options screen step: the game's Options, or a set of ours in it.
PPC_FUNC(options_step) {
    const uint32_t self = ctx.r[3], view = ctx.r[4];
    if (g_set < 0) {
        sub_02664F74__orig(ctx, base);
        return;
    }
    const int first = set_first(g_set), n = set_size(g_set);
    if (g_cur >= n) g_cur = 0;
    uint8_t saved[0x200];
    std::memcpy(saved, base + view, sizeof saved);
    base[view + 1] = g_edit ? 1 : 0;
    for (int i = 0; i < 8; ++i) {
        const uint32_t row = view + 0xB0 + static_cast<uint32_t>(i) * kRowSize;
        base[row] = static_cast<uint8_t>(i < n ? 2 + i : 0x12);  // the game's options 2.. (texts ours); 0x12+ hidden
        base[row + 1] = i == g_cur;
        ppc::st32(base, row + 0xC, i == g_cur ? 0xFFBEE67Du : 0xFF7E8C6Eu);
    }
    base[view + 0x1F1] = static_cast<uint8_t>(2 + g_cur);
    base[view + 0x1F2] = 7;  // every slot stepped: ours shown, the rest hidden (as past the game's last page)
    base[view + 0x1F4] = 0;
    base[view + 0x1F6] = 1;
    sub_02664F74__orig(ctx, base);
    std::memcpy(base + view, saved, sizeof saved);
    // our texts: title, labels, values, help
    gui::set_text(ctx, base, ppc::ld32(base, self + 0x570), text(kTabRows[g_set].label));
    const uint32_t labels = ppc::ld32(base, self + 0x584);
    for (int i = 0; i < n; ++i) {
        const SettingRow& r = g_rows[static_cast<size_t>(first + i)];
        if (labels) gui::set_text(ctx, base, ppc::ld32(base, labels + static_cast<uint32_t>(i) * 4), text(r.label));
        if (const uint32_t v = ppc::ld32(base, self + (0x186 + static_cast<uint32_t>(i)) * 4)) gui::set_text(ctx, base, v, text(value_text(r)));
    }
    const ppc::Context keep = ctx;
    if (g_cur < n) {
        ctx.r[3] = ppc::ld32(base, gui::kSID), ctx.r[4] = text(help_text(g_rows[static_cast<size_t>(first + g_cur)].help)), ctx.r[5] = 0;
        sub_02144960(ctx, base);
        ctx = keep;
    }
}

}  // namespace

void register_field_gfx(SettingsPage page) {
    g_page = std::move(page);
    ppc::set_override(0x026615A8, &list_step);
    ppc::set_override(0x02664F74, &options_step);
    set_field_menu_hook(&pad_hook);
}

}  // namespace mh3u::game
