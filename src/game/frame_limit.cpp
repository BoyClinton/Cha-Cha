// The game's frame limiter, skipped while Cha-Cha paces frames itself.
//
// Replaces: FUN_02BBFD3C (sLtRender, called once a frame by FUN_02BBFEEC after sLtRender::present). Status: wip -
// read in Ghidra; the boot-to-village headless run reaches it on 595 of 3410 frames (GX2GetSwapStatus, its only
// caller), the in-engine cutscenes it caps at 30 FPS still to be confirmed in a window.
//
// While renderer flag 1 is set (FUN_02BBA714(this, 1); cutscenes) it first waits for the flip queue to drain
// (GX2GetSwapStatus + GX2WaitForVsync) and then sleeps (OSSleepTicks) until a minimum frame time has passed since the
// previous frame. That holds cutscenes at the console's 30 FPS. With our own pacing (MH3U_FPS, the FRAME RATE LIMIT
// setting) the host decides the frame rate, so the limiter returns at once; headless runs keep it (game pacing, so
// frame-numbered input scripts stay in step).
#include "frame_limit.h"

#include "functions.h"

namespace mh3u::game {
namespace {

bool g_own_pacing = false;

PPC_FUNC(frame_limit) {
    if (!g_own_pacing) sub_02BBFD3C__orig(ctx, base);
}

}  // namespace

void register_frame_limit(bool own_pacing) {
    g_own_pacing = own_pacing;
    ppc::set_override(0x02BBFD3C, &frame_limit);
}

}  // namespace mh3u::game
