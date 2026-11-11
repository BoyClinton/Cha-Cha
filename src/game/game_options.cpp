// Game Options with Cha-Cha's own boxes: a second column on the right of the title's Game Options screen (Monitor /
// Sound / Effect / Controller / Game / Language Settings), mirrored about the screen's centre, with Graphics Settings
// (Phase 5.5, user 2026-11-04; Texture Dump moved to DLC > Mods 2026-11-05). Always on.
//
// Replaces: FUN_02763A88 (the start menu's Game Options state, uIDStartMenu +0x2A0 == 8: sub-state +0x290, 2 = the
// list; cursor +0x2D8, copied to +0x2D4 and to uIDOption +0x330; up / down step it with sound 3, A opens the page with
// sound 0xD - next state +0x2A4, sub-state 8 -, B goes back with 0xE - FUN_020918FC(sGSound, handle, bank 8, id)) and
// draws through cGUIInstAnimation::draw? (gui_util.h). Status: wip - read in Ghidra; checked headless with frame
// dumps (MH3U_INPUT_SCRIPT: right / up / down / left / B in both columns; MH3U_POINTER_SCRIPT: hover and click across
// the columns); the sounds are the game's own calls but not yet heard in a window.
//
// The list belongs to uIDOption (uIDStartMenu +0x1717C, layout +0x32C == 7): plates +0x2F4 (6 cGUIInstAnimControl,
// layout instances 0, 4, 5, 6, 7, 35 - the layout has no spare one), their highlight variables +0x30C (value 1 =
// highlighted, FUN_0275A4CC), help text object +0x328 (set each frame by the page step FUN_0275A7DC from +0x330). A
// plate's label is a cGUIObjMessage in its tree (*(instance +0xF8) +8; text group 0x6A index 0x42 + row, Language
// 0x50).
//
// No new GUI instances are made: the right column's boxes are the game's own plates drawn again, moved, each with a
// label of its own (gui::clone_message). The game's cursor always names the highlighted plate's row, also while
// Cha-Cha's cursor is in the right column (it stays on the same row), so the game keeps highlighting and animating
// one plate; each box is drawn from the highlighted plate when it is the selected one, else from a plate that is not
// highlighted. While the right column is selected the game sees no up / down / left / right / A (they are taken out
// of sAppPad for its step) and the help box shows our text; moving and choosing play the game's sounds 3 and 0xD.
// Graphics Settings opens through the game's own page change, as Game Settings (state 20) showing our page
// (settings_page.cpp).
#include "game_options.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>

#include "boot_prompts.h"
#include "functions.h"
#include "gui_util.h"

namespace mh3u::game {
namespace {

constexpr uint32_t kOption = 0x1717C;                          // uIDStartMenu -> uIDOption
// uIDOption: +0x32C the layout shown (FUN_0275C088 sets 7 for page 8, the list)
constexpr uint32_t kPlates = 0x2F4, kHelpObj = 0x328, kPage = 0x32C, kListLayout = 7;
constexpr uint32_t kCursor = 0x2D8, kCursorCopy = 0x2D4;       // uIDStartMenu
constexpr int kRows = 6;
// Game Options sounds (bank 8, the floats the screen passes)
constexpr uint32_t kMoveSound = 3, kDecideSound = 0xD, kSoundF1 = 0x1006F214, kSoundF2 = 0x1006F1F8;
constexpr uint32_t kGameSettingsState = 0x14;  // the page Graphics Settings shows as

struct Item {
    const char* label;
    const char* help;
};
constexpr Item kItems[] = {
    {"Display Settings", "Change how the game is shown: window,\r\nresolution, frame rate."},
    {"Graphics Settings", "Change how the game looks."},
};  // (Texture Dump moved to the DLC screen's Mods list, 2026-11-05: it runs on the game's download screen)
constexpr int kItemCount = static_cast<int>(std::size(kItems));
enum { kDisplay = 0, kGraphics = 1 };
constexpr int kControllerRow = 3;  // the game's Controller Settings (left column): our controller remapping instead
constexpr int kKeyboardRow = 4;    // the game's Game Settings (left column): Keyboard Settings, our keyboard remapping
constexpr const char* kKeyboardLabel = "Keyboard Settings";
constexpr const char* kKeyboardHelp = "Change keyboard and mouse settings.";

// TV pixels (the GUI's 1920 x 1080): the plates are 106 apart, 608 wide from x 176 (their trees' matrices), so the
// mirror about x 960 (the same 176 margin on the right) starts 960 further right
constexpr float kRowStep = 106.0f, kColumnDx = 1920.0f - 2 * 176.0f - 608.0f;

GameOptionsPages g_pages;
int g_col = 0, g_row = 0;           // guest thread: the column (1 = ours) and our row
int g_resume_col = -1, g_resume = 0;  // back from our page: the cursor returns to its box (column, row)
std::atomic<int> g_right_row{-1};   // for the menu pointer
std::atomic<int> g_hover{-1};       // a box the pointer moved onto (game_options_hover)
uint32_t g_labels[kItemCount] = {}, g_helps[kItemCount] = {};
uint32_t g_row_texts[kRows] = {};
// labels of the boxes not drawn with their plate's own label: left rows drawn from another plate, then the right
// column; remade when the layout object changes
constexpr int kClones = kRows + kItemCount;
uint32_t g_clones[kClones] = {}, g_clone_owner = 0;

void play(ppc::Context& ctx, uint8_t* base, uint32_t id) { gui::play_sound(ctx, base, 8, id, kSoundF1, kSoundF2); }

uint32_t label_object(uint8_t* base, uint32_t plate) {
    const uint32_t list = ppc::ld32(base, plate + 0xF8);
    return list ? ppc::ld32(base, list + 8) : 0;
}

// the game's label of a row, copied (game text may come from a buffer that is reused)
uint32_t row_label(ppc::Context& ctx, uint8_t* base, int row) {
    if (!g_row_texts[row] && row == kKeyboardRow) g_row_texts[row] = gui::guest_string(kKeyboardLabel);
    if (!g_row_texts[row])
        if (const uint32_t s = gui::game_text(ctx, base, 0x6A, row == 5 ? 0x50 : 0x42 + row))
            g_row_texts[row] = gui::guest_string(reinterpret_cast<const char*>(base + s));
    return g_row_texts[row];
}
uint32_t item_label(int item) {
    if (!g_labels[item]) g_labels[item] = gui::guest_string(kItems[item].label);
    return g_labels[item];
}

// the start menu's uIDOption while it shows the Game Options list, else 0
uint32_t list_options(uint8_t* base) {
    const uint32_t sm = start_menu_widget();
    if (!sm || ppc::ld32(base, sm + 0x2A0) != 8) return 0;
    const uint32_t opt = ppc::ld32(base, sm + kOption);
    return opt && ppc::ld32(base, opt + kPage) == kListLayout ? opt : 0;
}

void set_cursor(uint8_t* base, uint32_t sm, int row) {
    ppc::st32(base, sm + kCursor, static_cast<uint32_t>(row));
    ppc::st32(base, sm + kCursorCopy, static_cast<uint32_t>(row));
}

// The selected box of the right column is drawn from the highlighted plate, whose label also draws through its font
// filters (the highlight's glow): a copy would show the plate's own text there. So that plate's label itself takes
// the box's text while our column is selected (set in the menu step, every step - the game's own step puts Monitor
// Settings' label back each frame - never between two draws), and the game's back when the cursor leaves our column
// or the screen.
int g_relabelled = -1;  // the plate whose label shows one of our texts
uint32_t g_relabel_opt = 0;
void relabel_highlighted(ppc::Context& ctx, uint8_t* base, uint32_t opt, int row) {
    const auto label = [&](int r) { return label_object(base, ppc::ld32(base, opt + kPlates + static_cast<uint32_t>(r) * 4)); };
    if (row == g_relabelled && opt == g_relabel_opt) {
        if (opt && row >= 0) gui::set_text(ctx, base, label(row), item_label(row));
        return;
    }
    if (g_relabelled >= 0 && g_relabel_opt && g_relabel_opt == opt) gui::set_text(ctx, base, label(g_relabelled), row_label(ctx, base, g_relabelled));
    g_relabelled = -1;
    g_relabel_opt = opt;
    if (opt && row >= 0) {
        row_label(ctx, base, row);  // the game's text, kept before it is replaced
        gui::set_text(ctx, base, label(row), item_label(row));
        g_relabelled = row;
    }
}

// One of our pages, the game's own way into a page (as A on Game Settings): the next state, then sub-state 8 saves
// the options and changes state; the Game Settings page shows ours.
void open_page(ppc::Context& ctx, uint8_t* base, uint32_t sm, const SettingsPage* page, int col, int row) {
    settings_page::open(ctx, base, page);
    g_resume_col = col, g_resume = row;
    ppc::st32(base, sm + 0x2A4, kGameSettingsState);
    ppc::st32(base, sm + 0x290, 8);
    // and uIDOption's layout follows, as the game's A does: FUN_0275C088(uIDOption, next state, cursor)
    const ppc::Context saved = ctx;
    ctx.r[3] = ppc::ld32(base, sm + kOption);
    ctx.r[4] = kGameSettingsState;
    ctx.r[5] = ppc::ld32(base, sm + kCursor);
    sub_0275C088(ctx, base);
    ctx = saved;
}

void choose(ppc::Context& ctx, uint8_t* base, uint32_t sm, int item) {
    play(ctx, base, kDecideSound);
    std::printf("game options: %s chosen\n", kItems[item].label);
    if (item == kDisplay) open_page(ctx, base, sm, &g_pages.display, 1, item);
    else if (item == kGraphics) open_page(ctx, base, sm, &g_pages.graphics, 1, item);
}

// Game Options (r3 = uIDStartMenu)
PPC_FUNC(options_step) {
    const uint32_t sm = ctx.r[3];
    const uint32_t pad = ppc::ld32(base, gui::kSAppPad);
    const bool list = pad && ppc::ld32(base, sm + 0x2A0) == 8 && ppc::ld32(base, sm + 0x290) == 2;
    if (list) settings_page::closed();
    if (list && g_resume_col >= 0) {  // back from our page: on its box again
        g_col = g_resume_col, g_row = g_resume, g_resume_col = -1;
        set_cursor(base, sm, g_row);
    }
    uint32_t held = 0, pressed = 0;
    bool chosen = false, take_decide = false;
    if (!list) {
        g_col = 0;
    } else {
        held = ppc::ld32(base, pad + 0x80);
        pressed = ppc::ld32(base, pad + 0x6C);
        const uint32_t dirs = held | pressed;
        if (const int h = g_hover.exchange(-1); h >= 0 && h < kRows + kItemCount) {  // the pointer's box, at once
            const int col = h >= kRows ? 1 : 0, r = col ? h - kRows : h;
            if (col != g_col || r != (col ? g_row : static_cast<int>(ppc::ld32(base, sm + kCursor)))) {
                g_col = col;
                if (col) g_row = r;
                set_cursor(base, sm, r);
                play(ctx, base, kMoveSound);
            }
        }
        const int row = static_cast<int>(ppc::ld32(base, sm + kCursor));
        if (g_col == 0 && row == kControllerRow && (pressed & gui::kDecide) && g_pages.controls.rows) {
            play(ctx, base, kDecideSound);  // the game's Controller Settings: ours instead
            open_page(ctx, base, sm, &g_pages.controls, 0, row);
            chosen = take_decide = true;
        } else if (g_col == 0 && row == kKeyboardRow && (pressed & gui::kDecide) && g_pages.keyboard.rows) {
            play(ctx, base, kDecideSound);  // the game's Game Settings: Keyboard Settings instead
            open_page(ctx, base, sm, &g_pages.keyboard, 0, row);
            chosen = take_decide = true;
        } else if (g_col == 0 && (dirs & gui::kRight) && row >= 0 && row < kRows) {
            g_col = 1;
            g_row = std::min(row, kItemCount - 1);
            set_cursor(base, sm, g_row);
            play(ctx, base, kMoveSound);
        } else if (g_col == 1 && (dirs & gui::kLeft)) {
            g_col = 0;
            set_cursor(base, sm, g_row);
            play(ctx, base, kMoveSound);
        } else if (g_col == 1) {
            if (dirs & (gui::kUp | gui::kDown)) {
                g_row = (g_row + ((dirs & gui::kUp) ? kItemCount - 1 : 1)) % kItemCount;
                set_cursor(base, sm, g_row);
                play(ctx, base, kMoveSound);
            }
            if (pressed & gui::kDecide) {
                choose(ctx, base, sm, g_row);
                chosen = true;
            }
        }
    }
    const bool ours = list && g_col == 1;
    // the game's step sees no moves and no A while our column is selected (B still goes back), nor the A that
    // opened one of our pages
    const uint32_t taken = ours ? gui::kUp | gui::kDown | gui::kLeft | gui::kRight | gui::kDecide : take_decide ? gui::kDecide : 0;
    if (taken) {
        ppc::st32(base, pad + 0x80, held & ~taken);
        ppc::st32(base, pad + 0x6C, pressed & ~taken);
    }
    sub_02763A88__orig(ctx, base);
    if (taken) {
        ppc::st32(base, pad + 0x80, held);
        ppc::st32(base, pad + 0x6C, pressed);
    }
    if (ours) {
        if (const uint32_t opt = list_options(base)) {
            if (!g_helps[g_row]) g_helps[g_row] = gui::guest_string(kItems[g_row].help);
            const uint32_t help = g_helps[g_row];
            gui::set_text(ctx, base, ppc::ld32(base, opt + kHelpObj), help);
        }
    }
    const bool still = !chosen && ppc::ld32(base, sm + 0x2A0) == 8 && ppc::ld32(base, sm + 0x290) == 2;  // B may have left
    // Game Settings is Keyboard Settings: its plate's label and, while it is selected, the help box
    if (const uint32_t opt = still ? list_options(base) : 0) {
        if (const uint32_t label = label_object(base, ppc::ld32(base, opt + kPlates + kKeyboardRow * 4)))
            gui::set_text(ctx, base, label, row_label(ctx, base, kKeyboardRow));
        static const uint32_t help = gui::guest_string(kKeyboardHelp);
        if (g_col == 0 && static_cast<int>(ppc::ld32(base, sm + kCursor)) == kKeyboardRow) gui::set_text(ctx, base, ppc::ld32(base, opt + kHelpObj), help);
    }
    relabel_highlighted(ctx, base, ppc::ld32(base, sm + kOption), still && g_col == 1 ? g_row : -1);
    g_right_row.store(still && g_col == 1 ? g_row : -1);
}

// On the Game Options list the first plate's draw (plate 0 is drawn before the others) draws every box, and the
// other plates' own draws draw nothing. The boxes drawn from the highlighted plate (its glow) come last.
bool draw_list(ppc::Context& ctx, uint8_t* base, uint32_t inst) {
    const uint32_t opt = list_options(base);
    if (!opt) return false;
    int k = -1;
    for (int i = 0; i < kRows; ++i)
        if (ppc::ld32(base, opt + kPlates + static_cast<uint32_t>(i) * 4) == inst) k = i;
    if (k < 0) return false;
    if (k != 0) return true;  // drawn with plate 0's call
    if (opt != g_clone_owner) {  // a new layout: its labels are copied again
        g_clone_owner = opt;
        for (uint32_t& c : g_clones) c = 0;
    }
    const int h = static_cast<int>(ppc::ld32(base, start_menu_widget() + kCursor));  // the highlighted plate
    const bool right = g_col == 1;
    // a box at (column, row) drawn from plate s, its label replaced by a copy holding the box's text (-1: the
    // plate's own label)
    const auto box = [&](int s, int col, int row, int clone) {
        const uint32_t plate = ppc::ld32(base, opt + kPlates + static_cast<uint32_t>(s) * 4), label = label_object(base, plate);
        if (!label) return;
        if (clone >= 0 && !g_clones[clone])
            g_clones[clone] = gui::clone_message(ctx, base, label, clone < kRows ? row_label(ctx, base, clone) : item_label(clone - kRows));
        const float dx = col ? kColumnDx : 0.0f, dy = static_cast<float>(row - s) * kRowStep;
        if (clone >= 0) gui::draw_moved(ctx, base, plate, dx, dy, {{label, g_clones[clone]}});
        else gui::draw_moved(ctx, base, plate, dx, dy);
    };
    // the plate a box is drawn from: the highlighted one if the box is selected, else its own row's or another
    // plate that is not highlighted
    const auto source = [&](int row, bool selected) { return selected ? h : row != h ? row : (h == 0 ? 1 : 0); };
    for (int pass = 0; pass < 2; ++pass) {  // boxes from the highlighted plate last
        for (int r = 0; r < kRows; ++r) {
            const int s = source(r, !right && r == h);
            if ((s == h) == (pass == 1)) box(s, 0, r, r == s ? -1 : r);
        }
        for (int r = 0; r < kItemCount; ++r) {
            const int s = source(r, right && r == h);
            if ((s == h) == (pass == 1)) box(s, 1, r, right && r == h ? -1 : kRows + r);  // selected: relabelled
        }
    }
    return true;
}

}  // namespace

void register_game_options(GameOptionsPages pages) {
    g_pages = std::move(pages);
    ppc::set_override(0x02763A88, &options_step);
    gui::add_draw_hook(&draw_list);
    settings_page::install();
}

int game_options_right_row() { return g_right_row.load(); }
void game_options_hover(int item) { g_hover.store(item); }

}  // namespace mh3u::game
