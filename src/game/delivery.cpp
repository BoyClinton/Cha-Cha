// Auto Delivery (user 2026-11-10; mods.h, off by default): a quest's item delivery goals are handed in on the spot as
// soon as the pouch holds enough of the requested item - no trip to the delivery box. Goals for items the hunter
// carries in their arms (eggs and the like) are not touched (user 2026-11-10): those still go to camp.
//
// Read in Ghidra 2026-11-10 (status: wip, not yet seen in game):
//   sQuest (0x1031537C) +0xB050 = the running quest's state (+0x3C its rQuestData); the main goals at state +0x88,
//   two of {u32 flags, u16 id, s16 remaining}: flags bit 0 a monster, bit 1 (2) an item to deliver (carried items
//   too: an egg goal is an ordinary item goal, seen in game 2026-11-10); 0x200 / 0x1000 / 0x2000 other kinds.
//   Carried items are pouch items as well: FUN_028660EC (player) returns the one in the hunter's arms by looking for
//   item 0x401, 0x41F-0x422 or 0x424 in pouch slots 0-23 - the game's own list of carried items, used here. The
//   game hands them in when the hunter walks into the delivery box's spot (FUN_0289D170 case 0x38: a sPop spot of
//   kind 10 and FUN_0289A250 - a goal item in the pouch - then the same FUN_02179578).
//   FUN_02179578 sQuest::deliver? (sQuest): the delivery box's action. Unless the quest is over (FUN_0216A820(q, 1)),
//   it walks the local hunter's pouch (pad block +0x4D8, 36 x {s16 item, s16 count}) and for every goal with flag 2
//   and that item shows "%s delivered" (sQuest text 0x16), lowers the goal's remaining count by what the pouch has,
//   at most the remaining (FUN_02178C78: at 0 the goal is complete, FUN_0216AE60) and takes those items out of the
//   pouch (online: FUN_0217397C instead).
// The mod calls that same action once a frame (after the pad update, outside the game's menus, while a quest runs)
// when an item goal's remaining count is in the pouch; goals the pouch cannot yet fill are hidden from it for that
// call (flag 2 cleared, then set again), so nothing is handed in part by part.
#include "delivery.h"

#include <cstdint>

#include "functions.h"
#include "menu_mouse.h"
#include "mods.h"

namespace mh3u::game {
namespace {

constexpr uint32_t kSGame = 0x101FDB9C, kSQuest = 0x1031537C, kSPlayer = 0x10314B80;  // ::mpInstance
constexpr uint32_t kState = 0xB050, kGoals = 0x88, kItemGoal = 2;
constexpr uint32_t kPad = 0xE30, kPouch = 0x4D8, kPouchSlots = 0x24;

// FUN_028660EC's carried items (eggs and the like): never handed in by the mod
bool carried(uint16_t item) { return item == 0x401 || (item >= 0x41F && item <= 0x422) || item == 0x424; }

int pouch_count(const uint8_t* base, uint32_t pad, uint16_t item) {
    int n = 0;
    for (uint32_t i = 0; i < kPouchSlots; ++i) {
        const uint32_t slot = pad + kPouch + i * 4;
        if (ppc::ld16(base, slot) == item) n += static_cast<int16_t>(ppc::ld16(base, slot + 2));
    }
    return n;
}

void step(ppc::Context& ctx, uint8_t* base, bool menu_open) {
    if (menu_open || !mod_enabled(Mod::AutoDelivery)) return;
    const uint32_t game = ppc::ld32(base, kSGame), quest = ppc::ld32(base, kSQuest);
    if (!game || !quest || base[game + 0x48] == 2) return;  // the lobby
    const uint32_t state = ppc::ld32(base, quest + kState);
    if (!state || !ppc::ld32(base, state + 0x3C)) return;   // no quest
    bool any = false;
    for (uint32_t g = 0; g < 2; ++g) {
        const uint32_t goal = state + kGoals + g * 8;
        const uint16_t item = ppc::ld16(base, goal + 4);
        if ((ppc::ld32(base, goal) & kItemGoal) && static_cast<int16_t>(ppc::ld16(base, goal + 6)) > 0 && item && !carried(item)) any = true;
    }
    if (!any) return;
    ctx.r[3] = quest, ctx.r[4] = 1;
    sub_0216A820(ctx, base);
    if (ctx.r[3] != 0) return;  // the quest is over (or not running)
    ctx.r[3] = ppc::ld32(base, kSPlayer), ctx.r[4] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(base[game + 0x4B])));
    if (!ctx.r[3]) return;
    sub_0215A580(ctx, base);
    const uint32_t player = ctx.r[3];
    if (!player || !ppc::ld32(base, player + kPad)) return;
    const uint32_t pad = ppc::ld32(base, player + kPad);

    bool ready = false, hidden[2] = {false, false};
    for (uint32_t g = 0; g < 2; ++g) {
        const uint32_t goal = state + kGoals + g * 8;
        const uint32_t flags = ppc::ld32(base, goal);
        const uint16_t item = ppc::ld16(base, goal + 4);
        const int remaining = static_cast<int16_t>(ppc::ld16(base, goal + 6));
        if (!(flags & kItemGoal) || remaining <= 0 || !item) continue;
        if (!carried(item) && pouch_count(base, pad, item) >= remaining) ready = true;
        else hidden[g] = true;  // a carried item, or not enough yet: keep it out of this hand-in
    }
    if (!ready) return;
    for (uint32_t g = 0; g < 2; ++g)
        if (hidden[g]) ppc::st32(base, state + kGoals + g * 8, ppc::ld32(base, state + kGoals + g * 8) & ~kItemGoal);
    ctx.r[3] = quest;
    sub_02179578(ctx, base);
    for (uint32_t g = 0; g < 2; ++g)
        if (hidden[g]) ppc::st32(base, state + kGoals + g * 8, ppc::ld32(base, state + kGoals + g * 8) | kItemGoal);
}

}  // namespace

void register_auto_delivery() { add_frame_hook(&step); }

}  // namespace mh3u::game
