// Boot notices and save prompts, confirmed by the game itself (setting skip_boot_notices, on by default).
//
// Replaces: the start menu's per-frame state step @ 0x027695A0 (switch on uIDStartMenu +0x2A0) and the save system's Yes/No
// answer @ 0x020E28A0. Status: wip - verified headless from a fresh data folder (no save) to the title screen.
//
// Before the title, a fresh start shows: "The game will begin set to the Wii U Pro Controller", "In-game language
// has been changed", "Save data is required... Create save data now?" (Yes/No), "Set extended data save settings?"
// (Yes/No), "Set to save extended data.", "Extended data created." and "About Data Transfer". They all run inside
// the start menu's state step (uIDStartMenu +0x2A0, the state vf63 switches on; states 0 and 1 are the boot / save
// setup) and wait in one of two ways (docs/research-log.md 2026-10-26):
//   - notices: `sAppPad->trigger (+0x11C) & 0x60` (decide or cancel pressed this frame)
//   - Yes/No:  0x020E28A0 polls the menu cursor (sMenu +0x204) with the pad: 1 = Yes decided, 2 = No
// Messages are shown by 0x020E24B8, which closes them at once while sAppSaveData +0x27F931 is set - the flag aBoot
// itself sets while it loads saves.
//
// While the step runs in a boot state, the replacement sets that flag (nothing is drawn), marks decide in the
// game's pad trigger and answers Yes; all three are restored afterwards, so in-game saving and the menus after the
// title are unchanged.
#include "boot_prompts.h"

#include "functions.h"

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

namespace mh3u::game {
namespace {

constexpr uint32_t kSAppPad = 0x10312BB0;       // sAppPad::mpInstance
constexpr uint32_t kSAppSaveData = 0x101D6D08;  // sAppSaveData::mpInstance
constexpr uint32_t kPadTrigger = 0x11C;          // buttons pressed this frame (0x20 decide, 0x40 cancel): save system
constexpr uint32_t kMenuTrigger = 0x6C;          // the same for menus (start menu: FUN_02761584(this, 0x20))
constexpr uint32_t kSaveQuiet = 0x27F931;        // u8: save messages are closed instead of shown
constexpr uint32_t kMenuState = 0x2A0;           // uIDStartMenu: state stepped by vf63
constexpr uint32_t kDecide = 0x20;

bool g_enabled = true;
thread_local bool t_confirming = false;  // inside a boot-state step: Yes/No answers are Yes

// start-menu states that run the boot save setup: 0 (FUN_027605e0), 1 (FUN_027607c4: create save data, extended
// data); MH3U_BOOT_STATES="a,b,..." replaces the set for testing
std::set<uint32_t> g_states = {0, 1};
bool boot_state(uint32_t state) { return g_states.count(state) != 0; }

PPC_FUNC(start_menu_step) {
    const uint32_t self = ctx.r[3], state = ppc::ld32(base, self + kMenuState);
    static uint32_t seen = ~0u;
    if (state != seen) {
        seen = state;
        std::printf("boot prompts: start menu state %u\n", state);
    }
    if (!g_enabled || !boot_state(state)) return sub_027695A0__orig(ctx, base);
    const uint32_t pad = ppc::ld32(base, kSAppPad), save = ppc::ld32(base, kSAppSaveData);
    const uint32_t trigger = pad ? ppc::ld32(base, pad + kPadTrigger) : 0, menu = pad ? ppc::ld32(base, pad + kMenuTrigger) : 0;
    const uint8_t quiet = save ? base[save + kSaveQuiet] : 0;
    if (pad) {
        ppc::st32(base, pad + kPadTrigger, trigger | kDecide);
        ppc::st32(base, pad + kMenuTrigger, menu | kDecide);
    }
    if (save) base[save + kSaveQuiet] = 1;
    t_confirming = true;
    sub_027695A0__orig(ctx, base);
    t_confirming = false;
    if (pad) {
        ppc::st32(base, pad + kPadTrigger, trigger);
        ppc::st32(base, pad + kMenuTrigger, menu);
    }
    if (save) base[save + kSaveQuiet] = quiet;
    static uint32_t last = ~0u;
    if (state != last) {
        last = state;
        std::printf("boot prompts: start menu state %u confirmed\n", state);
    }
}

PPC_FUNC(yes_no_answer) {
    if (t_confirming) {
        ctx.r[3] = 1;  // Yes
        return;
    }
    sub_020E28A0__orig(ctx, base);
}

// ---- system notices (aGame::vf5 steps one while it is shown; 1 = still waiting, 0 = done) --------------------------
// Rewritten from the originals' "decide pressed" branch, minus the confirm sound. Found with MH3U_WATCH_READ on the
// pad trigger while the controller notice was up (docs/research-log.md 2026-10-26).
constexpr uint32_t kSGame = 0x101FDB9C, kSID = 0x10314578, kSMenu = 0x10314980;  // ::mpInstance
constexpr uint32_t kOptionController = 0x1D;  // game option: 1 Pro Controller, 0 GamePad

void set_option(ppc::Context& ctx, uint8_t* base, uint32_t option, uint32_t value) {  // FUN_02119ce0(sGame, opt, v)
    ctx.r[3] = ppc::ld32(base, kSGame);
    ctx.r[4] = option;
    ctx.r[5] = value;
    sub_02119CE0(ctx, base);
}
void close_notice(ppc::Context& ctx, uint8_t* base) {  // FUN_02143064(sID)
    ctx.r[3] = ppc::ld32(base, kSID);
    sub_02143064(ctx, base);
}
// the menu-drawn notices keep their text in sMenu (+0x52 kind, +0x2FC text); clearing both closes them
void clear_menu_notice(uint8_t* base) {
    const uint32_t menu = ppc::ld32(base, kSMenu);
    if (menu == 0xFFFFFFCCu) return;  // the originals test sMenu against -0x34
    base[menu + 0x52] = 0;
    ppc::st32(base, menu + 0x2FC, 0);
}

PPC_FUNC(notice_controller_pro) {  // 0x02019C0C: "The game will begin set to the Wii U Pro Controller..."
    if (!g_enabled) return sub_02019C0C__orig(ctx, base);
    set_option(ctx, base, kOptionController, 1);
    close_notice(ctx, base);
    ctx.r[3] = 0;
}
PPC_FUNC(notice_controller_pad_a) {  // 0x02019AD4: the GamePad counterpart
    if (!g_enabled) return sub_02019AD4__orig(ctx, base);
    set_option(ctx, base, kOptionController, 0);
    close_notice(ctx, base);
    ctx.r[3] = 0;
}
PPC_FUNC(notice_controller_pad_b) {  // 0x02019B70
    if (!g_enabled) return sub_02019B70__orig(ctx, base);
    set_option(ctx, base, kOptionController, 0);
    close_notice(ctx, base);
    ctx.r[3] = 0;
}
PPC_FUNC(notice_close) {  // 0x02019CA8
    if (!g_enabled) return sub_02019CA8__orig(ctx, base);
    close_notice(ctx, base);
    ctx.r[3] = 0;
}
PPC_FUNC(notice_no_controller) {  // 0x02019D30: "The set controller could not be detected..."
    if (!g_enabled) return sub_02019D30__orig(ctx, base);
    close_notice(ctx, base);
    ctx.r[3] = 0;
}
PPC_FUNC(notice_language) {  // 0x02019E38: "In-game language has been changed."
    if (!g_enabled) return sub_02019E38__orig(ctx, base);
    clear_menu_notice(base);
    ctx.r[3] = 0;
}
PPC_FUNC(notice_message) {  // 0x0201A020
    if (!g_enabled) return sub_0201A020__orig(ctx, base);
    clear_menu_notice(base);
    ctx.r[3] = 0;
}

}  // namespace

void register_boot_prompts(bool enabled) {
    g_enabled = enabled;
    char* env = nullptr;
    size_t len = 0;
    if (_dupenv_s(&env, &len, "MH3U_BOOT_STATES") == 0 && env)
        for (char* p = env; *p;) {
            char* end = nullptr;
            if (p == env) g_states.clear();
            g_states.insert(static_cast<uint32_t>(std::strtoul(p, &end, 10)));
            if (end == p) break;
            p = *end ? end + 1 : end;
        }
    std::free(env);
    ppc::set_override(0x027695A0, &start_menu_step);
    ppc::set_override(0x020E28A0, &yes_no_answer);
    ppc::set_override(0x02019C0C, &notice_controller_pro);
    ppc::set_override(0x02019AD4, &notice_controller_pad_a);
    ppc::set_override(0x02019B70, &notice_controller_pad_b);
    ppc::set_override(0x02019CA8, &notice_close);
    ppc::set_override(0x02019D30, &notice_no_controller);
    ppc::set_override(0x02019E38, &notice_language);
    ppc::set_override(0x0201A020, &notice_message);
}

}  // namespace mh3u::game
