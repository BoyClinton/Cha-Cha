// The windpress push made independent of the frame rate (issue #2: above 30 FPS wind pressure pushed hunters much
// too far).
//
// Replaces: FUN_028ABC3C playerWindpress? (r3 = the uPlayer, r4 = a variant), the hunter's windpress reaction, phase
// in pad block +7: 0 starts motion 0xDA, 1 = pushed back while bracing, 2 counts (+8) into motion 0xE2, 3 stands
// until the motion ends. In phase 1 it adds, every frame, a push along the hunter's facing (+0x11C):
// K * cos(motion frame ...) added to the position (pad block +0xE0 / +0xE8) - without the frame time, so the push
// went FPS / 30 times as far. Status: wip - read in Ghidra; seen in play logs (MH3U_PLAY_LOG + MH3U_PLAY_WATCH, the
// only writer of the position in that phase; a straight slide of ~0.8 s then the 1.3 s stand, at 100 and 20 FPS).
// The original runs; in phase 1 its push is scaled by the hunter's frame time (cUnit+0x1C in 1/60 s, 2.0 at the
// console's 30 FPS), so 30 FPS stays exactly the original.
#include "windpress.h"

#include <bit>
#include <cmath>

#include "functions.h"

namespace mh3u::game {
namespace {

PPC_FUNC(windpress) {
    const uint32_t player = ctx.r[3];
    const uint32_t pad = ppc::ld32(base, player + 0xE30);
    const bool pushing = pad && base[pad + 7] == 1;
    const float x = pushing ? ppc::ldf32(base, pad + 0xE0) : 0, z = pushing ? ppc::ldf32(base, pad + 0xE8) : 0;
    sub_028ABC3C__orig(ctx, base);
    if (!pushing || base[pad + 7] != 1) return;  // not the push phase, or it moved on before pushing
    const float t = ppc::ldf32(base, player + 0x1C) * 0.5f;
    if (!std::isfinite(t) || t <= 0 || t > 50 || std::fabs(t - 1.0f) < 1e-4f) return;
    const auto put = [&](uint32_t a, float v) { ppc::st32(base, a, std::bit_cast<uint32_t>(v)); };
    put(pad + 0xE0, x + (ppc::ldf32(base, pad + 0xE0) - x) * t);
    put(pad + 0xE8, z + (ppc::ldf32(base, pad + 0xE8) - z) * t);
}

}  // namespace

void register_windpress() { ppc::set_override(0x028ABC3C, &windpress); }

}  // namespace mh3u::game
