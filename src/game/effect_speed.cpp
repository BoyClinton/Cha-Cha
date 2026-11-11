// Effects (particles) stepped at the console's 30 steps a second, whatever the frame rate (issue #15: above 30 FPS
// fog, mist and dust puffs lived and moved FPS / 30 times too fast, so they blinked in and out instead of drifting).
//
// The particle system counts in whole frames: each update of an emitter (cParticleManager slot 17) flips its
// double-buffer bit (+0xEC bit 23), runs slot 15 (prepare), counts its delay / interval timer (s16 +0xE2) and frame
// counter (+0x88) by one, emits new particles (slot 22, init) and moves and ages every particle (slot 23) - one
// console frame per call, with no frame time anywhere (FUN_02b0a3bc / FUN_02b09f6c, the emit-count steps). It is
// called once per game frame, so at 100 FPS everything ran 3.3 times as fast. Status: wip - read in Ghidra
// (cParticleGenerator::vf17 0x02B0DC4C, cParticleNode::vf17 0x02B8D5A4); the pops were seen in a 20-frame capture at
// 100 FPS in Moga Woods area 7 (puffs gone mid-fade within 2-3 frames).
//
// Replaces (the four overrides of slot 17; the other generators inherit cParticleGenerator's):
//   0x02B0DC4C cParticleGenerator::vf17, 0x02B8D5A4 cParticleNode::vf17, 0x02B8E7FC cParticleNodeInfinite::vf17,
//   0x02B05284 cEffectUnitGenerator::vf17: (this) -> 1 while alive, 0 when finished.
//   0x0294B8DC sMain::framePacing? sets sMain+0x7C, the frame time in 1/60 s (2.0 at 30 FPS), once a frame.
// After framePacing the console frames this frame covers are added up (frame time / 2); each whole one is a step.
// An update with no step due returns "alive" without running (the particles stay as drawn); with two due (under
// 30 FPS) it runs twice, as the console would have. At exactly 30 FPS every frame is one step: the original.
// MH3U_EFFECT_STEP=frame turns this off (the old once-a-frame stepping, for comparison).
#include "effect_speed.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

#include "functions.h"

namespace mh3u::game {
namespace {

constexpr uint32_t kSMain = 0x103CEA44;  // sMain::mpInstance
double g_acc = 0;                        // console frames not yet stepped
int g_steps = 1;                         // steps due this frame
bool g_log = false;                      // MH3U_EFFECT_LOG=1: frames, steps and frame time once a second
int g_log_frames = 0, g_log_steps = 0;
double g_log_dt = 0;

void probe_print();
void log_frame(float dt) {
    static auto last = std::chrono::steady_clock::now();
    ++g_log_frames;
    g_log_steps += g_steps;
    g_log_dt += dt;
    if (const auto now = std::chrono::steady_clock::now(); now - last >= std::chrono::seconds(1)) {
        std::printf("effects: %d frames/s, %d steps/s, mean frame time %.3f console frames\n", g_log_frames, g_log_steps,
                    g_log_frames ? g_log_dt / g_log_frames : 0.0);
        probe_print();
        g_log_frames = g_log_steps = 0;
        g_log_dt = 0;
        last = now;
    }
}

PPC_FUNC(frame_pacing) {
    sub_0294B8DC__orig(ctx, base);
    const uint32_t main = ppc::ld32(base, kSMain);
    const float dt = main ? ppc::ldf32(base, main + 0x7C) * 0.5f : 1.0f;  // console frames since the last frame
    if (!std::isfinite(dt) || dt <= 0) {
        g_steps = 1;
        g_acc = 0;
        return;
    }
    g_acc += dt;
    const double whole = std::floor(g_acc + 1e-4);  // 30 FPS gives exactly 1.0 a frame: no drift into a skip
    g_steps = static_cast<int>(std::min(whole, 2.0));
    g_acc = whole > 2.0 ? 0.0 : g_acc - whole;  // a long hitch: catch up two steps, drop the rest
    if (g_acc < 0) g_acc = 0;
    if (g_log) log_frame(dt);
}

template <ppc::GuestFunction Orig>
void step(ppc::Context& ctx, uint8_t* base) {
    if (g_steps == 1) return Orig(ctx, base);
    if (g_steps <= 0) {
        ctx.r[3] = 1;  // alive; not stepped this frame
        return;
    }
    const auto self = ctx.r[3];
    Orig(ctx, base);
    for (int i = 1; i < g_steps && ctx.r[3] != 0; ++i) {
        ctx.r[3] = self;
        Orig(ctx, base);
    }
}

// MH3U_EFFECT_LOG: once a second, per generator vtable with live particles, one particle's life counter and colour
std::map<uint32_t, std::string> g_probe;
void probe(const uint8_t* base, uint32_t gen) {
    const uint32_t p = ppc::ld32(base, gen + 0xB0);
    if (!p) return;
    const uint32_t vt = ppc::ld32(base, gen);
    if (g_probe.count(vt)) return;
    int n = 0;
    for (uint32_t q = p; q && n < 1000; q = ppc::ld32(base, q + 4)) ++n;
    const uint32_t flags = ppc::ld32(base, p + 0xC);
    char s[256];
    std::snprintf(s, sizeof s, "n=%d age=%u life flags=%04X max=%u reset=%u value=%.2f step=%.2f colour=%08X/%08X cur=%u",
                  n, ppc::ld32(base, p + 0x14), ppc::ld16(base, p + 0x60), ppc::ld16(base, p + 0x64), ppc::ld16(base, p + 0x66),
                  ppc::ldf32(base, p + 0x68), ppc::ldf32(base, p + 0x6C), ppc::ld32(base, p + 0x90), ppc::ld32(base, p + 0x94),
                  (flags >> 7) & 1);
    g_probe[vt] = s;
}
void probe_print() {
    for (const auto& [vt, s] : g_probe) std::printf("  generator vtable %08X: %s\n", vt, s.c_str());
    g_probe.clear();
}

PPC_FUNC(generator_step) {
    const uint32_t gen = static_cast<uint32_t>(ctx.r[3]);
    step<sub_02B0DC4C__orig>(ctx, base);
    if (g_log) probe(base, gen);
}
PPC_FUNC(node_step) { step<sub_02B8D5A4__orig>(ctx, base); }
PPC_FUNC(node_infinite_step) { step<sub_02B8E7FC__orig>(ctx, base); }
PPC_FUNC(unit_generator_step) { step<sub_02B05284__orig>(ctx, base); }

}  // namespace

void register_effect_speed() {
    char* v = nullptr;
    size_t n = 0;
    const bool off = _dupenv_s(&v, &n, "MH3U_EFFECT_STEP") == 0 && v && std::string(v) == "frame";
    std::free(v);
    if (off) return;
    if (_dupenv_s(&v, &n, "MH3U_EFFECT_LOG") == 0 && v) g_log = std::string(v) == "1";
    std::free(v);
    ppc::set_override(0x0294B8DC, &frame_pacing);
    ppc::set_override(0x02B0DC4C, &generator_step);
    ppc::set_override(0x02B8D5A4, &node_step);
    ppc::set_override(0x02B8E7FC, &node_infinite_step);
    ppc::set_override(0x02B05284, &unit_generator_step);
}

}  // namespace mh3u::game
