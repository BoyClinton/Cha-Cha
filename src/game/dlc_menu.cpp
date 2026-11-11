// The DLC screen as Cha-Cha's add-on menu (dlc_menu.h).
//
// Replaces: aDLContents::vf7 0x020135F4 (the DLC area's step; docs/research-log.md "DLC server protocol"). Status:
// wip, checked headless. The original always runs except where noted; around it:
//   - the DLC Menu's three buttons (state 9, cursor 0x1030C128, chosen in state 10) read Official / Quests / Mods at
//     the top. Official stays on the menu with the game's own labels back (Content Previews / Quests / Extras); B
//     there goes back up. Quests goes on as the game's Quests; Mods jumps to the event quest list (state 0x21),
//     whose rows (0x1030C1F0, 0x5B8 each, flags +4 bit 2 = ticked) are the mods: ticked = on, X (Download,
//     relabelled Save) saves, and leaving it goes back to the DLC Menu instead of the Quests menu.
//   - the notices before the DLC Menu (states 1-5) are skipped: their work runs, their boxes and waits don't
//     (skip_notices).
//   - labels change through text_trace's set_text, in place (the game's text objects keep the string it gave them).
//   - the Mods list's detail panel shows only its summary page (page 3 of 3, DAT_1030C150 = 2), without the page
//     indicator or page turns (detail_panel).
// The DLC area's states: DAT_1030C0E4 current, DAT_1030C0E8 next; pad bits at sAppPad+0x11C: 0x20 choose, 0x40 back,
// 0x100 X, 0x200 Y, 0x8000 + (FUN_0200BDE8, FUN_0200DC14). Y on the Mods list: Texture Dump on the download screen.
#include "dlc_menu.h"

#include <atomic>
#include <iterator>
#include <cstdio>
#include <string>
#include <utility>

#include "functions.h"
#include "guest_memory.h"
#include "gui_util.h"
#include "menu_mouse.h"
#include "mods.h"
#include "text_trace.h"

namespace mh3u::game {
namespace {

constexpr uint32_t kState = 0x1030C0E4, kNext = 0x1030C0E8, kCursor = 0x1030C128, kRows = 0x1030C1F0,
                   kRowCount = 0x1030C138, kFadeTarget = 0x1030C120;  // FUN_0200A3BC's state after its fade
constexpr uint32_t kRowSize = 0x5B8, kRowFlags = 4, kRowQuest = 8, kTicked = 2;
constexpr uint32_t kPadBack = 0x40, kPadX = 0x100, kPadY = 0x200;
constexpr uint32_t kMenu = 9, kMenuChosen = 10, kMenuInit = 7, kEventList = 0x21, kList = 0x2A, kListLeft = 0x2D;
constexpr uint32_t kSoundBack = 0xE;  // FUN_020BFD88 id, as the DLC Menu plays it
constexpr uint32_t kGroup = 0x95;                          // GUI\font\dlc\DLC
constexpr uint32_t kFirstModQuest = 20990;                 // the Mods list's rows are "quests" 20990...: the
constexpr uint32_t kTextureDump = kFirstModQuest + static_cast<uint32_t>(Mod::Count);  // switches, then Texture Dump
constexpr uint32_t kListLoad = 0x29, kCancelling = 0x35;                       // the list (re)made from its file
constexpr uint32_t kSlots = 0xCB9C;                        // aDLContents: the save's quest slots in memory
// the list's detail panel (FUN_02710998): its page DAT_1030C150 (0-2 of DAT_1030C14C = 3; the list's case 0x2A of
// FUN_0200DC14 turns it on pad 0x80 / 0x8 at sAppPad+0x11C or +0x154), its "01/03" indicator the object at +0x2D0
// (FUN_02ADB830(obj, shown))
constexpr uint32_t kDetailPage = 0x1030C150, kSummaryPage = 2, kPadDetailPage = 0x80 | 0x8, kPageIndicator = 0x2D0;

struct Label {
    uint32_t index, caller;
    const char* text;
    std::string original;
};
// the DLC Menu's buttons and the notice board's headings
Label g_top[] = {{1, 0x027440B0, "Official", {}}, {2, 0x02744208, "Mods", {}},
                 {6, 0x0270F458, "Official", {}}, {8, 0x0270F8B0, "Mods", {}}};
// the quest list as the Mods list (heading, X button, the detail page's quest-only labels and units)
Label g_mods[] = {{4, 0x02710AFC, "Mods", {}},      {14, 0x02745B8C, "Texture Dump", {}},     {23, 0x02710FC0, "Status", {}},
                  {24, 0x0271102C, "Solo HP", {}},  {25, 0x02711098, "Duo HP", {}},   {26, 0x02711104, "Trio HP", {}},
                  {35, 0x027112B4, "%", {}},        {36, 0x02711314, "%", {}},        {36, 0x02711374, "%", {}},
                  {27, 0x02711170, "", {}},         {28, 0x027111DC, "", {}},         {29, 0x027117B4, "", {}},
                  {31, 0x0271188C, "", {}},         {28, 0x027118F8, "", {}},
                  // the list's help line, and Texture Dump (Y) on the download screen: its question and its
                  // "Downloading content..."
                  {129, 0x0200E940, "Tick a mod to switch it on, untick it to\r\nswitch it off. Texture Dump: every texture.", {}},
                  {55, 0x0200EAAC, "Dump every texture of the game?\r\n(This takes a few minutes.)", {}},
                  {85, 0x02746684, "Dumping the game's textures...\r\n\r\ninto texture_dump in Cha-Cha's data folder", {}}};

// Official's quest lists: the X bar (Download) reads Apply
Label g_official[] = {{14, 0x02745B8C, "Apply", {}}};

std::atomic<DlcSection> g_section{DlcSection::Top};
bool g_ticks_set = false;
bool g_relayout = false;  // the top labels changed: the menu sets its texts again (labels_step)
std::function<void()> g_on_saved, g_on_dump_cancel;
bool g_dump_cancelled = false;
uint32_t g_sound_handle = 0;

std::string read_cstr(uint8_t* base, uint32_t a) {
    std::string s;
    for (; a && s.size() < 256; ++a) {
        const char c = static_cast<char>(ppc::ld8(base, a));
        if (!c) break;
        s += c;
    }
    return s;
}

// the game's own text (not ours: game_text asks from a caller no replacement is keyed on)
void keep_originals(ppc::Context& ctx, uint8_t* base, Label* labels, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (labels[i].original.empty()) labels[i].original = read_cstr(base, gui::game_text(ctx, base, kGroup, labels[i].index));
}

template <size_t N>
void show(Label (&labels)[N], bool ours) {
    for (Label& l : labels) set_text(kGroup, l.index, ours ? std::string(l.text) : l.original, l.caller);
}

void gui_sound(ppc::Context& ctx, uint8_t* base, uint32_t id) {
    if (!g_sound_handle)
        if (core::GuestMemory* gm = core::GuestMemory::current()) g_sound_handle = gm->alloc_hle(16, 8);
    const ppc::Context saved = ctx;
    ctx.r[3] = g_sound_handle;
    ctx.r[4] = id;
    sub_020BFD88(ctx, base);
    ctx = saved;
}

void set_section(ppc::Context& ctx, uint8_t* base, DlcSection s) {
    const DlcSection old = g_section.exchange(s);
    if (old == s) return;
    keep_originals(ctx, base, g_top, std::size(g_top));
    if (s == DlcSection::Mods) keep_originals(ctx, base, g_mods, std::size(g_mods));
    if (s == DlcSection::Official) keep_originals(ctx, base, g_official, std::size(g_official));
    if (old == DlcSection::Official) show(g_official, false);  // (before Mods sets its own text for the same label)
    if (s == DlcSection::Top || s == DlcSection::Official) {
        show(g_top, s == DlcSection::Top);
        g_relayout = true;
    }
    if (s == DlcSection::Mods || old == DlcSection::Mods) show(g_mods, s == DlcSection::Mods);
    if (s == DlcSection::Official) show(g_official, true);
    g_ticks_set = false;
    std::printf("dlc menu: %s\n", s == DlcSection::Top ? "top" : s == DlcSection::Official ? "official" : s == DlcSection::Custom ? "quests" : "mods");
}

// each Mods row's tick from the switches, once the list is up
void set_ticks(uint8_t* base) {
    const uint32_t rows = ppc::ld32(base, kRows);
    const uint32_t n = ppc::ld32(base, kRowCount);
    for (uint32_t i = 0; rows && i < n && i < 8; ++i) {
        const uint32_t row = rows + i * kRowSize;
        const uint32_t m = ppc::ld32(base, row + kRowQuest) - kFirstModQuest;
        if (m >= static_cast<uint32_t>(Mod::Count)) continue;
        const uint32_t flags = ppc::ld32(base, row + kRowFlags);
        ppc::st32(base, row + kRowFlags, mod_enabled(static_cast<Mod>(m)) ? flags | kTicked : flags & ~kTicked);
    }
}

// Texture Dump's "download" lands in a save slot like a quest (FUN_02014A20: in memory a slot is the 16-byte name,
// then the data); ours are numbered after every real quest, so taking it out leaves the slots as they were
void clear_our_slots(uint8_t* base, uint32_t self) {
    const uint32_t slots = ppc::ld32(base, self + kSlots);
    if (!slots) return;
    for (uint32_t i = 0; i < 60 + 20; ++i) {
        const uint32_t slot = slots + (i < 60 ? i * 0x2010 : 0x783C0 + (i - 60) * 0x2010);
        if (read_cstr(base, slot).rfind("q_2099", 0) != 0) continue;
        std::printf("dlc menu: %s taken out of the save's slots\n", read_cstr(base, slot).c_str());
        for (uint32_t k = 0; k < 0x2010; k += 4) ppc::st32(base, slot + k, 0);
    }
}

// Texture Dump (Y on the Mods list): the game downloads ticked rows, so the first row lends its number for one
// "download" (q_<kTextureDump>, answered as the dump goes) and is put back when the list is up again
uint32_t g_lent_row = 0, g_lent_id = 0, g_lent_flags = 0;

void lend_row(uint8_t* base) {
    const uint32_t rows = ppc::ld32(base, kRows);
    if (!rows || !ppc::ld32(base, kRowCount)) return;
    g_lent_row = rows;
    g_lent_id = ppc::ld32(base, rows + kRowQuest);
    g_lent_flags = ppc::ld32(base, rows + kRowFlags);
    const uint32_t n = ppc::ld32(base, kRowCount);
    for (uint32_t i = 0; i < n && i < 8; ++i)  // only that one ticked
        ppc::st32(base, rows + i * kRowSize + kRowFlags, ppc::ld32(base, rows + i * kRowSize + kRowFlags) & ~kTicked);
    ppc::st32(base, rows + kRowQuest, kTextureDump);
    ppc::st32(base, rows + kRowFlags, (g_lent_flags & ~1u) | kTicked);  // not "downloaded already"
}

void give_back_row(uint8_t* base) {
    if (!g_lent_row) return;
    if (ppc::ld32(base, g_lent_row + kRowQuest) == kTextureDump) {  // (a list made again has its own rows)
        ppc::st32(base, g_lent_row + kRowQuest, g_lent_id);
        ppc::st32(base, g_lent_row + kRowFlags, g_lent_flags);
    }
    g_lent_row = 0;
}

// the switches' ticks differ from the switches (a row was ticked / unticked)
bool ticks_changed(uint8_t* base) {
    const uint32_t rows = ppc::ld32(base, kRows);
    const uint32_t n = ppc::ld32(base, kRowCount);
    for (uint32_t i = 0; rows && i < n && i < 8; ++i) {
        const uint32_t row = rows + i * kRowSize;
        const uint32_t m = ppc::ld32(base, row + kRowQuest) - kFirstModQuest;
        if (m < static_cast<uint32_t>(Mod::Count) && ((ppc::ld32(base, row + kRowFlags) & kTicked) != 0) != mod_enabled(static_cast<Mod>(m)))
            return true;
    }
    return false;
}

void save_ticks(uint8_t* base) {
    const uint32_t rows = ppc::ld32(base, kRows);
    const uint32_t n = ppc::ld32(base, kRowCount);
    for (uint32_t i = 0; rows && i < n && i < 8; ++i) {
        const uint32_t row = rows + i * kRowSize;
        const uint32_t m = ppc::ld32(base, row + kRowQuest) - kFirstModQuest;
        if (m < static_cast<uint32_t>(Mod::Count))
            set_mod_enabled(static_cast<Mod>(m), (ppc::ld32(base, row + kRowFlags) & kTicked) != 0);
    }
    // what the rows show now: Status (field 9, +0x158) and the HP numbers (fields 10-12, +0x198 / +0x1A0 / +0x19C)
    const bool scaled = mod_enabled(Mod::QuestScaling);
    for (uint32_t i = 0; rows && i < n && i < 8; ++i) {
        const uint32_t row = rows + i * kRowSize;
        const uint32_t m = ppc::ld32(base, row + kRowQuest) - kFirstModQuest;
        if (m >= static_cast<uint32_t>(Mod::Count)) continue;
        const std::string status = mod_enabled(static_cast<Mod>(m)) ? "On" : "Off";
        for (uint32_t k = 0; k < 4; ++k) ppc::st8(base, row + 0x158 + k, k < status.size() ? static_cast<uint8_t>(status[k]) : 0);
        ppc::st32(base, row + 0x198, scaled ? 65 : 100);
        ppc::st32(base, row + 0x1A0, scaled ? 80 : 100);
        ppc::st32(base, row + 0x19C, scaled ? 90 : 100);
    }
    for (int m = 0; m < static_cast<int>(Mod::Count); ++m)
        std::printf("dlc menu: mod %s %s\n", mod_info(static_cast<Mod>(m)).key, mod_enabled(static_cast<Mod>(m)) ? "on" : "off");
    if (g_on_saved) g_on_saved();
}

// The menu pointer on the DLC screens (TV pixels, measured on captures): hovering moves the game's cursor (its "wanted"
// cursor DAT_1030C12C, which the screen takes with its move sound; the Yes / No cursor DAT_1030C130), a click presses
// for the screen what the item answers to (sAppPad+0x11C: 0x20 choose, 0x40 back, 0x100 X, 0x400 / 0x800 R / L,
// 0x8000 +).
struct Box {
    float x0, y0, x1, y1;
    bool has(float x, float y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
};
constexpr uint32_t kCursorWanted = 0x1030C12C, kYesNo = 0x1030C130, kPageRows = 0x1030C134;
constexpr uint32_t kSubmenu = 0x20, kConfirm = 0x30, kDownloading = 0x31, kNotices = 0x55, kSoundMove = 2;
constexpr uint32_t kPadChoose = 0x20, kPadR = 0x400, kPadL = 0x800, kPadPlus = 0x8000;
constexpr Box kMenuItems[3] = {{1080, 351, 1530, 437}, {1080, 487, 1530, 573}, {1080, 623, 1530, 709}};
constexpr Box kSubmenuItems[2] = {{1060, 322, 1560, 520}, {1060, 550, 1560, 745}};
constexpr Box kExit = {1170, 790, 1480, 835};
constexpr float kListX0 = 1002, kListX1 = 1598, kListTop = 313, kListStep = 70;  // rows: 64 high, 70 apart
constexpr Box kXBar = {1082, 660, 1528, 742};
constexpr Box kPageBack[2] = {{1415, 262, 1480, 312}, {500, 802, 570, 842}}, kPageOn[2] = {{1570, 262, 1635, 312}, {665, 802, 735, 842}};
constexpr Box kYes = {700, 608, 912, 656}, kNo = {1008, 608, 1220, 656};
constexpr Box kCancel = {795, 776, 1140, 824};
float g_last_x = -1, g_last_y = -1;
bool g_yes_next = false;  // a click on Yes while No was picked: the cursor goes to Yes, Yes is chosen the next frame

void pointer(ppc::Context& ctx, uint8_t* base, uint32_t state, uint32_t pad_obj) {
    float x = 0, y = 0;
    if (!pad_obj || !menu_pointer(x, y)) return;
    const bool moved = x != g_last_x || y != g_last_y;
    g_last_x = x, g_last_y = y;
    uint32_t press = 0;
    const bool click = menu_click();
    if (g_yes_next) {
        g_yes_next = false;
        if (state == kConfirm && ppc::ld32(base, kYesNo) == 0) press |= kPadChoose;
    }
    const auto want = [&](uint32_t item, bool sound) {  // the cursor onto item (on a move), the item chosen (a click)
        if (moved && ppc::ld32(base, kCursor) != item) {
            ppc::st32(base, kCursorWanted, item);
            if (sound) gui_sound(ctx, base, kSoundMove);
        }
        if (click) {
            ppc::st32(base, kCursorWanted, item);
            press |= kPadChoose;
        }
    };
    if (state == kMenu) {
        for (uint32_t i = 0; i < 3; ++i)
            if (kMenuItems[i].has(x, y)) want(i, true);
        if (click && kExit.has(x, y)) press |= kPadPlus;
    } else if (state == kSubmenu) {
        for (uint32_t i = 0; i < 2; ++i)
            if (kSubmenuItems[i].has(x, y)) want(i, true);
        if (click && kExit.has(x, y)) press |= kPadPlus;
    } else if (state == kList) {
        const uint32_t rows = ppc::ld32(base, kPageRows);
        for (uint32_t i = 0; i < rows && i < 5; ++i)
            if (x >= kListX0 && x <= kListX1 && y >= kListTop + kListStep * i && y < kListTop + kListStep * i + 64) want(i, false);
        if (click) {
            if (kXBar.has(x, y)) press |= kPadX;
            if (kExit.has(x, y)) press |= kPadPlus;
            // (the second boxes sit on the detail panel's page indicator, hidden on the Mods list)
            const size_t boxes = g_section == DlcSection::Mods ? 1 : std::size(kPageBack);
            for (size_t i = 0; i < boxes; ++i) {
                if (kPageBack[i].has(x, y)) press |= kPadL;
                if (kPageOn[i].has(x, y)) press |= kPadR;
            }
        }
    } else if (state == kConfirm) {
        // the question's cursor DAT_1030C130 (0 Yes, 1 No) moves with R / L (pad 0x400 / 0x800, the game's move sound,
        // its highlight sMenu+0x59); A on No or B answers No
        const uint32_t yn = ppc::ld32(base, kYesNo);
        for (uint32_t i = 0; i < 2; ++i)
            if ((i ? kNo : kYes).has(x, y)) {
                if (click) press |= i ? kPadBack : (yn == 0 ? kPadChoose : kPadL);  // Yes from No: back to Yes first
                else if (moved && yn != i) press |= i ? kPadR : kPadL;
            }
        if (click && (press & kPadL) && !(press & kPadBack)) g_yes_next = true;
    } else if (state == kDownloading) {
        if (click && kCancel.has(x, y)) press |= kPadBack;
    } else if ((state >= kNotices && state < 0x60) || state == 0x0B || state == 0x2E || state == 0x3F) {
        // a notice ("Canceled.", "No information found." etc.; 0x61 / 0x62 are the loading waits): A closes it
        if (click) press |= kPadChoose;
    }
    if (press) ppc::st32(base, pad_obj + 0x11C, ppc::ld32(base, pad_obj + 0x11C) | press);
}

// The notices before the DLC Menu, skipped (FUN_0200A474, states 1-5; DLC texts 72-81): "Connect to get downloadable
// content?", "Please do not turn off your console...", "Checking save data." / "Finished checking save data.",
// "Currently, there are %d quests saved.", "Downloaded Extras found:", "Connecting to the Internet..." and
// "Retrieving information...". The work under them still runs (the save game before connecting from the field, the
// save check, the connection to Cha-Cha's DLC server, the notice board's fetch); what goes is each wait for A (done
// here as the game's A branch does it, minus its confirm sound), each minimum time on screen, and the boxes. Errors
// keep theirs: state 2's offline notice, state 6, the "Retry ?" of state 5 and the error states (0x55...).
constexpr uint32_t kSub = 0x1030C0F0, kSteps = 0x1030C0FC;  // the state's substate, its steps so far
constexpr uint32_t kQueuedSub = 0x10164FB4;  // the substate the next state starts in
constexpr uint32_t kIdle = 9;                // a substate states 1 and 4 have no case for: waits for the state change
constexpr uint32_t kSMenu = 0x10314980;      // sMenu::mpInstance; +0x52 its message kind, +0x2FC the message's text
constexpr uint8_t kMessage = 0x1E, kWaitMessage = 0x22;

void skip_notices(uint8_t* base, uint32_t self, uint32_t state) {
    const uint32_t sub = ppc::ld32(base, kSub), steps = ppc::ld32(base, kSteps);
    const auto shown_for = [&](uint32_t n) { if (steps < n) ppc::st32(base, kSteps, n); };
    if (state == 1) {
        if (steps == 0) {  // "Connect...?": Yes, without asking
            ppc::st32(base, kYesNo, 0);
            ppc::st32(base, kSteps, 1);
            ppc::st32(base, kSub, 1);
        } else if (sub == 10) {  // "Please do not turn off your console..."
            ppc::st32(base, kNext, 2);
            ppc::st32(base, kSub, kIdle);
        }
    } else if (state == 3) {
        if (sub == 10) {  // "Checking save data." (the check itself runs in 0xB)
            ppc::st32(base, self + 0xCBA4, 0);
            ppc::st32(base, kSub, 0xB);
        } else if (sub == 0xC) {
            shown_for(0x5B);
        } else if (sub == 0x10) {  // "Currently, there are %d quests saved."
            ppc::st32(base, kSub, 0x12);
        }
    } else if (state == 4) {
        if (sub == 2) {
            shown_for(0x5B);
        } else if (sub == 4) {  // "Finished checking save data."
            ppc::st32(base, kQueuedSub, 0x10);
            ppc::st32(base, kNext, 3);
            ppc::st32(base, kSub, kIdle);
        } else if (sub == 6) {  // "Downloaded Extras found:"
            ppc::st32(base, kSub, 8);
        }
    } else if (state == 5 && sub == 3) {  // connected: on without the wait
        shown_for(0xB5);
    }
}

// a box the skipped states opened this step, closed again before it is drawn (as FUN_021434DC closes one)
void hide_notice(uint8_t* base, uint32_t state) {
    const uint32_t menu = ppc::ld32(base, kSMenu);
    if (menu == 0xFFFFFFCCu || base[menu + 0x4C]) return;  // (the originals' own tests)
    const uint8_t kind = base[menu + 0x52];
    if ((state == 3 && (kind == kMessage || kind == kWaitMessage)) || (state == 5 && kind == kWaitMessage)) {
        base[menu + 0x52] = 0;
        ppc::st32(base, menu + 0x2FC, 0);
    }
}

PPC_FUNC(dlc_step) {
    const uint32_t self = ctx.r[3];
    const uint32_t state = ppc::ld32(base, kState);
    if (state >= 1 && state <= 5) {  // (entering: the top level, as below)
        if (g_section != DlcSection::Top) set_section(ctx, base, DlcSection::Top);
        skip_notices(base, self, state);
        sub_020135F4__orig(ctx, base);
        hide_notice(base, state);
        return;
    }
    const uint32_t pad_obj = ppc::ld32(base, gui::kSAppPad);
    pointer(ctx, base, state, pad_obj);
    const uint32_t pad = pad_obj ? ppc::ld32(base, pad_obj + 0x11C) : 0;
    const DlcSection section = g_section;

    if (state < kMenuInit && section != DlcSection::Top) set_section(ctx, base, DlcSection::Top);  // (re)entering
    if (state == kMenuInit && (section == DlcSection::Custom || section == DlcSection::Mods))
        set_section(ctx, base, DlcSection::Top);  // back from Quests / Mods
    if (state == kMenuChosen && section == DlcSection::Top) {
        const uint32_t choice = ppc::ld32(base, kCursor);
        if (choice == 0) {  // Official: the same menu, the game's own buttons
            set_section(ctx, base, DlcSection::Official);
            ppc::st32(base, kState, kMenu);
            ppc::st32(base, kNext, kMenu);
        } else if (choice == 1) {
            set_section(ctx, base, DlcSection::Custom);  // the game goes on to its Quests menu
        } else if (choice == 2) {  // Mods: the game's way to Quests, its fade then ending on the event quest list
            set_section(ctx, base, DlcSection::Mods);
            ppc::st32(base, kCursor, 1);
            ppc::st32(base, self + 0x98FC, 0);
            sub_020135F4__orig(ctx, base);
            ppc::st32(base, kFadeTarget, kEventList);
            ppc::st32(base, self + 0x98F8, choice);  // the menu's remembered button
            return;
        }
    }
    if (state == kMenu && section == DlcSection::Official && (pad & kPadBack)) {
        gui_sound(ctx, base, kSoundBack);
        set_section(ctx, base, DlcSection::Top);
    }
    if (section == DlcSection::Mods) {
        clear_our_slots(base, self);
        // Cancel on the download screen (state 0x35 waits for the request to end): the dump stops, the request ends
        if (state == kCancelling && g_lent_row && !g_dump_cancelled && g_on_dump_cancel) {
            g_dump_cancelled = true;
            g_on_dump_cancel();
        }
        if (state == kList) g_dump_cancelled = false;
        if (state == kListLoad) {  // made again (after a dump): ticks from the switches again
            g_ticks_set = false;
            give_back_row(base);  // (the list keeps its rows: a lent row would stay Texture Dump's)
        }
        if (state == kList) {
            // the detail panel stays on its summary page: no page turns
            for (const uint32_t at : {pad_obj + 0x11C, pad_obj + 0x154})
                if (pad_obj) ppc::st32(base, at, ppc::ld32(base, at) & ~kPadDetailPage);
            ppc::st32(base, kDetailPage, kSummaryPage);
            if (!g_ticks_set) {
                set_ticks(base);
                g_ticks_set = true;
            }
            give_back_row(base);  // back from a Texture Dump (or its question answered No)
            if (pad & (kPadX | kPadY)) {  // the X bar (Texture Dump), or Y: the game's question, then its download screen
                lend_row(base);
                ppc::st32(base, pad_obj + 0x11C, (pad & ~kPadY) | kPadX);
            }
        }
    }
    sub_020135F4__orig(ctx, base);
    // a tick changed (A, with the game's tick sounds): saved at once
    if (g_section == DlcSection::Mods && ppc::ld32(base, kState) == kList && g_ticks_set && !g_lent_row && ticks_changed(base)) save_ticks(base);
    // B on the Mods list fades to state 0x2D, which goes to the Quests menu: to the DLC Menu instead
    if (g_section == DlcSection::Mods && ppc::ld32(base, kFadeTarget) == kListLeft) ppc::st32(base, kFadeTarget, kMenuInit);
}

// uIDDLContents' label step (FUN_02743EBC, every frame): sets the DLC Menu's texts again whenever its screen id
// (+0x298) is not the current one (DAT_1030C0E0); a stale id after a relabel makes it lay ours out.
PPC_FUNC(labels_step) {
    if (g_relayout) {
        ppc::st32(base, ctx.r[3] + 0x298, 0xFFFFFFFFu);
        g_relayout = false;
    }
    sub_02743EBC__orig(ctx, base);
}

// The list's detail panel (FUN_02710998): on the Mods list the summary page only, its page indicator hidden
PPC_FUNC(detail_panel) {
    const uint32_t self = ctx.r[3];
    const bool mods = g_section == DlcSection::Mods;
    if (mods) ppc::st32(base, kDetailPage, kSummaryPage);
    sub_02710998__orig(ctx, base);
    if (!mods) return;
    if (const uint32_t indicator = ppc::ld32(base, self + kPageIndicator)) {
        const ppc::Context saved = ctx;
        ctx.r[3] = indicator;
        ctx.r[4] = 0;
        sub_02ADB830(ctx, base);
        ctx = saved;
    }
}

}  // namespace

DlcSection dlc_section() { return g_section; }

void register_dlc_menu(std::function<void()> on_mods_saved, std::function<void()> on_dump_cancel) {
    g_on_saved = std::move(on_mods_saved);
    g_on_dump_cancel = std::move(on_dump_cancel);
    for (const Label& l : g_top) set_text(kGroup, l.index, l.text, l.caller);  // the top level as the screen is made
    ppc::set_override(0x020135F4, &dlc_step);
    ppc::set_override(0x02743EBC, &labels_step);
    ppc::set_override(0x02710998, &detail_panel);
}

}  // namespace mh3u::game
