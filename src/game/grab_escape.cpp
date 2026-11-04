// Escaping a monster's grab made independent of the frame rate (issue #2: above 30 FPS it was very hard to wiggle out
// of a grab or pin).
//
// Replaces: FUN_028B0F70 playerEscapeGauge? (r3 = the uPlayer), run every frame while the hunter is held. Fields in the
// pad block (uPlayer+0xE30): +0x6B4 gauge (+0x6B8 its previous value), +0x6BC a pointer to the frame time (1.0 at the
// console's 30 FPS), +0x6C0 / +0x6C4 a one-frame cooldown, +0x6B0 fills still needed, +0x6CC held. Each wiggle the
// stick check FUN_0286C37C accepts adds *(+0x6BC) to the gauge; at 4 (2 with the skill FUN_0285F738(0x30)) it resets
// and +0x6B0 counts down; the hunter breaks free when it runs out. Adding the frame time per wiggle made each wiggle
// worth 0.3 at 100 FPS instead of 1.0: about 13 wiggles per fill instead of 4. Status: wip - read in Ghidra, seen in a
// play log (Arzuros grab at 100 FPS: the gauge rising by 0.25-0.34 on stick flips, a fill about every second).
// The original runs with +0x6BC pointed at a constant 1.0 (0x10029C08) for the call, so every frame rate counts a
// wiggle as the console does; nothing else reads that pointer.
#include "grab_escape.h"

#include "functions.h"

namespace mh3u::game {
namespace {

constexpr uint32_t kOne = 0x10029C08;  // .rodata float 1.0

PPC_FUNC(escape_gauge) {
    const uint32_t pad = ppc::ld32(base, ctx.r[3] + 0xE30);
    if (!pad) {
        sub_028B0F70__orig(ctx, base);
        return;
    }
    const uint32_t frame_time = ppc::ld32(base, pad + 0x6BC);
    ppc::st32(base, pad + 0x6BC, kOne);
    sub_028B0F70__orig(ctx, base);
    ppc::st32(base, pad + 0x6BC, frame_time);
}

}  // namespace

void register_grab_escape() { ppc::set_override(0x028B0F70, &escape_gauge); }

}  // namespace mh3u::game
