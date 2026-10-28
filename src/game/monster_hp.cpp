// Monster HP: where a monster's maximum HP is set, for monster HP scaling (always on, no MODS switch).
//
// Replaces: FUN_0259FB84 uEnemy::setMaxHp? (called by FUN_0259FC98 at spawn from FUN_0259FCD0, and by the vf48 of
// uEm027/033/036/040 when they change form). Status: wip - read in Ghidra (docs/research-log.md, "Monster HP"); the
// original runs, then on a hub quest the maximum HP is scaled for the number of hunters (kHunterScale), as the game's
// own x0.55 is applied - in this function, before the spawn code goes on (FUN_0259FCD0 -> FUN_0259FC98). Every rank
// scales by the same fraction of its own HP: the rank is in the game's stat row. Each spawn is logged.
//
// What the game does (r3 = the uEnemy, r4 = base HP from em_statusNN, the rEnemyStatus at uEnemy+0x1DE0, word 0):
//   state = *(uEnemy + 0x1BE0)
//   row   = FUN_0259E3B4(enemy, 0): a 0x20-byte row of floats in a table sEnemy keeps per quest monster set
//           (FUN_020F3B9C(sEnemy, set id)), picked by the byte state+0x8E8 (+1, +2: other stats); row[0] = HP factor
//   *(state + 0x8BC) = (int)(base * row[0])            maximum HP
//   if the current quest has flag 0x100000 or 0x10 (quest data +0x1E4) and sGame+0x1B9 != 1: max HP *= 0.55
// Current quest data: *(*(sQuest + 0xB050) + 0x3C); its byte +0x24F is the quest rank (sEnemy+0x4284, 0..3).
#include "monster_hp.h"

#include <cstdio>

#include "functions.h"

namespace mh3u::game {
namespace {

constexpr uint32_t kSQuest = 0x1031537C, kSGame = 0x101FDB9C, kSLobby = 0x103147A0;

// HP for the number of hunters on a hub quest (user's choice 2026-10-28, MH4U-style): 1 -> 0.65 ... 4 -> 1.0 of the
// game's hub HP. Offline there is always one hunter.
constexpr float kHunterScale[4] = {0.65f, 0.80f, 0.90f, 1.0f};
constexpr int kHunters = 1;

PPC_FUNC(set_max_hp) {
    const uint32_t enemy = ctx.r[3], base_hp = ctx.r[4];
    sub_0259FB84__orig(ctx, base);
    const uint32_t state = ppc::ld32(base, enemy + 0x1BE0);
    if (!state) return;
    const uint32_t game_hp = ppc::ld32(base, state + 0x8BC);
    uint32_t quest_id = 0, flags = 0, rank = 0;
    if (const uint32_t lobby = ppc::ld32(base, kSLobby)) quest_id = ppc::ld16(base, lobby + 0x522C);
    if (const uint32_t sq = ppc::ld32(base, kSQuest))
        if (const uint32_t cur = ppc::ld32(base, sq + 0xB050))
            if (const uint32_t qd = ppc::ld32(base, cur + 0x3C)) {
                flags = ppc::ld32(base, qd + 0x1E4);
                rank = ppc::ld8(base, qd + 0x24F);
            }
    const uint32_t game = ppc::ld32(base, kSGame);
    const uint32_t row = ppc::ld8(base, state + 0x8E8);
    const uint32_t village = game ? ppc::ld8(base, game + 0x1B9) : 1;
    // a hub quest (sGame+0x1B9 is 1 in the village, 0 in Port Tanzia) the game has not already reduced (x0.55 for
    // flags 0x100000 / 0x10 there): scaled for the hunters
    const bool reduced = (flags & 0x100010) != 0;
    uint32_t hp = game_hp;
    if (village != 1 && !reduced) {
        hp = static_cast<uint32_t>(static_cast<float>(game_hp) * kHunterScale[kHunters - 1]);
        if (hp < 1) hp = 1;
        ppc::st32(base, state + 0x8BC, hp);
    }
    std::printf("monster hp: em%03u base %u -> %u -> %u (stat row %u; quest %u rank %u flags %08X sGame+1B9 %u)\n",
                ppc::ld8(base, enemy + 0xE33), base_hp, game_hp, hp, row, quest_id, rank, flags, village);
}

}  // namespace

void register_monster_hp() { ppc::set_override(0x0259FB84, &set_max_hp); }

}  // namespace mh3u::game
