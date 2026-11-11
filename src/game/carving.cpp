// Two gathering mods (mods.h):
//
// Auto Carve - hold to gather: keep the button held to carve / gather / mine again and again until the spot is used up.
// Replaces: FUN_020ECA2C sCockpit::obtainItem? (cockpit, player, item, ..., flags, count, ...), the one step every
// carve, gathering spot and mining point goes through in a quest (seen 2026-10-28: a carve, gathers and mining, all
// from it): it adds the item (FUN_0285ECD4 in a quest, the sUserData pouch elsewhere) and shows "%s obtained."
// (FUN_020EF0A8 kind 2 text 8) when the add returns < 2. Status: wip - the original runs unchanged; after an item is
// obtained the input layer keeps "pressing" the held button (input::repeat_held), as if the player mashed it, until
// a few seconds pass without another item.
//
// Instant Gathering (user 2026-11-10): one gathering / mining / bug net animation takes everything the spot has left;
// each item still rolls its reward and the tool's breakage. Instant Carve (user 2026-11-11): the same for carving
// (the gather action's kinds 1 and 5, carving on land / under water? - the only kinds with a crouch first stage),
// split out because Instant Gathering also emptied carcasses in one carve.
// Replaces: FUN_028BC31C gatherReward? (player, kind, roll, item), called only from the gather action state
// FUN_028BC650 (player, kind) at the animation's reward moment, after
//   FUN_02162430 sPop::rollSpot? (sPop, area pad+0x56, position pad+0xE0, spot pad+0x9BC): finds the spot's slot
//                (sPop + i x 0x110, i 0x7F..0: +0x524B0 / +0x524B1 set, +0x524B5 area, +0x52570 spot, +0x52580 not
//                -1) and its count +0x5257E: 0 = used up (returns 0 or 0xFF), -1 = endless (1); else the gathering
//                skills' early-end roll, then count - 1: 1 = more left, 2 = that was the last
//   FUN_0216272C sPop::rollItem? (sPop, table): a weighted pick from the spot's {weight, item} list
// FUN_028BC31C itself adds the item (gathering skills 0xF7 / 0x106 for a second one) through obtainItem and, for
// kinds 2, 3 and 8, rolls the tool's breakage (FUN_028BC038: the tool pad+0x56C, its use counter pad+0x9C4..0x9CC
// reset to 0 when it breaks and one tool is taken from the pouch). The mod calls the same three again until the spot
// says it was the last, the tool breaks, the pouch takes no more, or the spot is endless (one item, as usual).
// Status: wip - read in Ghidra 2026-11-10; not yet seen in game.
#include "carving.h"

#include <cstdint>
#include <utility>

#include "functions.h"
#include "mods.h"

namespace mh3u::game {
namespace {

constexpr uint32_t kSPop = 0x10314C70;  // sPop::mpInstance
constexpr uint32_t kPad = 0xE30;        // player: its pad / state block
constexpr int kMaxExtra = 50;           // safety: no spot holds that many

std::function<void()> g_on_obtain;
uint32_t g_last_obtain = 0;  // obtainItem's result (< 2 = obtained)

PPC_FUNC(obtain_item) {
    sub_020ECA2C__orig(ctx, base);
    g_last_obtain = ctx.r[3];
    if (ctx.r[3] < 2 && g_on_obtain && mod_enabled(Mod::AutoCarve)) g_on_obtain();  // obtained (the game's own test before "%s obtained.")
}

// The tool's use counters: one going back to 0 is a break (FUN_028BC038 resets it only then).
struct Uses {
    uint8_t n[9];
};
Uses uses(const uint8_t* base, uint32_t pad) {
    Uses u{};
    for (uint32_t i = 0; i < 9; ++i) u.n[i] = base[pad + 0x9C4 + i];
    return u;
}
bool broke(const Uses& before, const Uses& after) {
    for (int i = 0; i < 9; ++i)
        if (before.n[i] != 0 && after.n[i] == 0) return true;
    return false;
}

// The spot's slots in sPop (as FUN_02162430 finds them, without its distance test): 1 = some have items left,
// -1 = endless, 0 = none
int spot_state(const uint8_t* base, uint32_t pad) {
    const uint32_t pop = ppc::ld32(base, kSPop);
    const uint32_t spot = ppc::ld32(base, pad + 0x9BC);
    if (!pop || !spot) return 0;
    const int8_t area = static_cast<int8_t>(base[pad + 0x56]);
    int state = 0;
    for (uint32_t i = 0; i < 0x80; ++i) {
        const uint32_t s = pop + i * 0x110;
        if (!base[s + 0x524B0] || !base[s + 0x524B1] || static_cast<int8_t>(base[s + 0x524B5]) != area) continue;
        if (static_cast<int16_t>(ppc::ld16(base, s + 0x52580)) == -1 || ppc::ld32(base, s + 0x52570) != spot) continue;
        const int16_t count = static_cast<int16_t>(ppc::ld16(base, s + 0x5257E));
        if (count == -1) return -1;
        if (count > 0) state = 1;
    }
    return state;
}

PPC_FUNC(gather_reward) {
    const uint32_t player = ctx.r[3], kind = ctx.r[4], roll = ctx.r[5], item = ctx.r[6];
    const uint32_t pad = ppc::ld32(base, player + kPad);
    Uses before = uses(base, pad);
    g_last_obtain = 0xFFFFFFFF;
    sub_028BC31C__orig(ctx, base);
    // kinds 1 and 5 are carving (on land / under water; FUN_028d204c's gather states pass 0-9, and only these two
    // start with the crouch, anims 0x131 / 0x138 and 0x13F / 0x143): Instant Carve, the rest Instant Gathering
    const bool carve = kind == 1 || kind == 5;
    if (!mod_enabled(carve ? Mod::InstantCarve : Mod::InstantGathering)) return;
    if (roll != 1 || (item & 0xFFFF) == 0) return;  // 2: that was the last
    if (ppc::ld16(base, pad + 0x9C0) == 0x10) return;                                     // the game's own special item
    for (int n = 0; n < kMaxExtra; ++n) {
        Uses after = uses(base, pad);
        if (broke(before, after) || g_last_obtain >= 2 || base[pad + 9] != 0) return;  // tool broke, pouch, item list
        if (spot_state(base, pad) != 1) return;                                            // used up, or endless
        before = after;
        ctx.r[3] = ppc::ld32(base, kSPop), ctx.r[4] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(base[pad + 0x56])));
        ctx.r[5] = pad + 0xE0, ctx.r[6] = ppc::ld32(base, pad + 0x9BC);
        sub_02162430(ctx, base);
        const uint32_t r = ctx.r[3] & 0xFF;
        if (r != 1 && r != 2) return;
        ctx.r[3] = ppc::ld32(base, kSPop), ctx.r[4] = ppc::ld32(base, pad + 0x9BC);
        sub_0216272C(ctx, base);
        const uint32_t next = ctx.r[3] & 0xFFFF;
        if (next == 0xFFFF || next == 0) return;
        g_last_obtain = 0xFFFFFFFF;
        ctx.r[3] = player, ctx.r[4] = kind, ctx.r[5] = r, ctx.r[6] = next;
        sub_028BC31C__orig(ctx, base);
        if (r == 2) return;
    }
}

}  // namespace

void register_carving(std::function<void()> on_obtain) {
    g_on_obtain = std::move(on_obtain);
    ppc::set_override(0x020ECA2C, &obtain_item);
    ppc::set_override(0x028BC31C, &gather_reward);
}

}  // namespace mh3u::game
