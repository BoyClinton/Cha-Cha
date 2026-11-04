// MH3U_PLAY_LOG=<file>: a per-frame log for finding steps that run once a frame instead of by the frame time
// (windpress push, weapon attacks, the talk marker over villagers; issues #2, #7). Debug only, nothing changes.
//
// Wraps (status: wip, the originals run unchanged):
//   FUN_0227E138 cameraPadCopy?  (r3 = uGameCamera): copies sAppPad buttons / right stick into the camera block, once a
//                                frame in the village, the field and quests, right after FUN_0227EB7C stored the
//                                hunter at block +0x690 - so the hunter is known every frame
//   0x025F2810 uIDCockpitAbovehead::vf63 (r3 = the marker): places the display above the hunter's head and picks its
//                                animation (+0x2E8 child, its +0xC = animation 1 / 0x1D, 9 / 0x1E with pad +0x714 7)
// Lines (CSV): "p" = time ms, line number, sMain+0x7C (frame time, 1/60 s units), hunter cUnit+0x1C, position
// (pad block +0xE0/E4/E8), yaw (+0x120), input bits (+0x1C8), pad block bytes +0x00..+0x3F and +0x700..+0x71F (hex);
// "a" = time ms, marker address, its child's animation; "d" = time ms, then every 4-byte word of the hunter (P, 0x2900
// bytes from the uPlayer) and its pad block (B, 0x1600 bytes) that changed since the frame before, as offset=value
// (escape gauges, push speeds, timers). With a watch callback, every write to the hunter's x position (pad block
// +0xE0) is counted per writing function and second (read_watch, report at exit; RelWithDebInfo for names).
// The first monster's changed words follow on the "d" line as M<offset>=value; "s" = time ms, sAppPad sticks: raw
// left x / y, right x / y, strength left / right, angle left / right (hex).
#include "play_log.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include <algorithm>
#include <map>

#include "functions.h"

namespace mh3u::game {
namespace {

constexpr uint32_t kSMain = 0x103CEA44;  // sMain::mpInstance
FILE* g_file = nullptr;
uint32_t g_frames = 0;
std::chrono::steady_clock::time_point g_start;
std::function<void(uint32_t)> g_watch;  // arms the position-write watch on a guest address
constexpr uint32_t kPlayerSize = 0x2900, kPadSize = 0x1600;
uint32_t g_prev_player = 0, g_prev_pad = 0;
std::vector<uint8_t> g_old_player, g_old_pad;
constexpr uint32_t kSEnemy = 0x10313260, kSAppPad = 0x10312BB0;  // sEnemy / sAppPad ::mpInstance
constexpr uint32_t kMonsterSize = 0x2400;                         // the uEmNNN classes are 0x2230..0x22D0 bytes
uint32_t g_prev_monster = 0;
std::vector<uint8_t> g_old_monster;
// the village talk arrow: the stage's uGameEffectModel (vtable 0x10035CE4, found by the unit log) and its uGameEffect
// (+0xE38); their changed words go on the "d" line as E<offset> / G<offset>
constexpr uint32_t kEffectModelVtable = 0x10035CE4, kEffectModelSize = 0xE40, kEffectSize = 0x230;
uint32_t g_effect_model = 0, g_prev_model = 0, g_prev_effect = 0;
std::vector<uint8_t> g_old_model, g_old_effect;

double now_ms() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_start).count();
}
void hex(const uint8_t* base, uint32_t a, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) std::fprintf(g_file, "%02X", base[a + i]);
}
// changed words of [a, a + n) against the copy in old (a new object: everything is taken as the base, nothing logged)
void diff(const uint8_t* base, char tag, uint32_t a, uint32_t n, uint32_t& prev, std::vector<uint8_t>& old) {
    if (prev != a) {
        old.assign(base + a, base + a + n);
        prev = a;
        return;
    }
    for (uint32_t o = 0; o < n; o += 4) {
        if (std::memcmp(&old[o], base + a + o, 4) == 0) continue;
        std::fprintf(g_file, ";%c%X=%08X", tag, o, ppc::ld32(base, a + o));
        std::memcpy(&old[o], base + a + o, 4);
    }
}

PPC_FUNC(camera_pad_copy) {
    const uint32_t cam = ctx.r[3];
    sub_0227E138__orig(ctx, base);
    const uint32_t player = ppc::ld32(base, ppc::ld32(base, cam + 0x8E0) + 0x690);
    if (!player) return;
    const uint32_t pad = ppc::ld32(base, player + 0xE30);
    if (!pad) return;
    const uint32_t main = ppc::ld32(base, kSMain);
    std::fprintf(g_file, "p,%.1f,%u,%.4f,%.4f,%.3f,%.3f,%.3f,%04X,%08X,", now_ms(), ++g_frames,
                 main ? ppc::ldf32(base, main + 0x7C) : 0.0f, ppc::ldf32(base, player + 0x1C),
                 ppc::ldf32(base, pad + 0xE0), ppc::ldf32(base, pad + 0xE4), ppc::ldf32(base, pad + 0xE8),
                 ppc::ld32(base, pad + 0x120) & 0xFFFF, ppc::ld32(base, pad + 0x1C8));
    hex(base, pad, 0x40);
    std::fputc(',', g_file);
    hex(base, pad + 0x700, 0x20);
    std::fprintf(g_file, "\nd,%.1f", now_ms());
    diff(base, 'P', player, kPlayerSize, g_prev_player, g_old_player);
    diff(base, 'B', pad, kPadSize, g_prev_pad, g_old_pad);
    // the first monster in sEnemy's list (+0x34, 4-byte slots): its words that changed, as M<offset>=value
    if (const uint32_t enemies = ppc::ld32(base, kSEnemy)) {
        uint32_t monster = 0;
        for (uint32_t i = 0; i < 8 && !monster; ++i) monster = ppc::ld32(base, enemies + 0x34 + i * 4);
        if (monster) diff(base, 'M', monster, kMonsterSize, g_prev_monster, g_old_monster);
    }
    if (g_effect_model && ppc::ld32(base, g_effect_model) == kEffectModelVtable) {
        diff(base, 'E', g_effect_model, kEffectModelSize, g_prev_model, g_old_model);
        if (const uint32_t effect = ppc::ld32(base, g_effect_model + 0xE38))
            diff(base, 'G', effect, kEffectSize, g_prev_effect, g_old_effect);
    }
    // sAppPad sticks: raw x / y (left +0x124 / +0x12C, right +0x128 / +0x130), angle and strength (+0x144.. / +0x148..)
    if (const uint32_t pad_sys = ppc::ld32(base, kSAppPad)) {
        std::fprintf(g_file, "\ns,%.1f", now_ms());
        for (uint32_t o : {0x124u, 0x12Cu, 0x128u, 0x130u, 0x148u, 0x14Cu})
            std::fprintf(g_file, ",%d", static_cast<int32_t>(ppc::ld32(base, pad_sys + o)));
        std::fprintf(g_file, ",%04X,%04X", ppc::ld16(base, pad_sys + 0x144), ppc::ld16(base, pad_sys + 0x146));
    }
    std::fputc('\n', g_file);
    if (g_watch) g_watch(pad + 0xE0);
    if (g_frames % 120 == 0) std::fflush(g_file);
}

// sUnit::vf11 (r3 = sUnit, r4 = move line): updates the units of one line (list head sUnit + line * 0x18 + 0x40,
// next unit +0x14). Logged after it: "u,time,+|-,line,unit,vtable,+0xC" for a unit joining / leaving the line, and
// "c,time,line,unit,vtable,old,new" when a unit's state word +0xC changes (top bits: state; others: draw / pause
// flags) - to find which object a marker on screen is, by when it appears.
std::map<uint32_t, std::pair<uint32_t, uint32_t>> g_units;  // unit -> (line, +0xC)
std::map<uint32_t, std::vector<uint32_t>> g_line_units;     // line -> units seen last time
PPC_FUNC(unit_line) {
    const uint32_t units = ctx.r[3], line = ctx.r[4];
    sub_02C0E43C__orig(ctx, base);
    std::vector<uint32_t> now;
    for (uint32_t u = ppc::ld32(base, units + line * 0x18 + 0x40), n = 0; u && n < 4096; u = ppc::ld32(base, u + 0x14), ++n)
        now.push_back(u);
    auto& before = g_line_units[line];
    const double t = now_ms();
    for (uint32_t u : now) {
        const uint32_t state = ppc::ld32(base, u + 0xC);
        if (ppc::ld32(base, u) == kEffectModelVtable) g_effect_model = u;
        auto it = g_units.find(u);
        if (it == g_units.end() || it->second.first != line) {
            std::fprintf(g_file, "u,%.1f,+,%u,%08X,%08X,%08X\n", t, line, u, ppc::ld32(base, u), state);
            g_units[u] = {line, state};
        } else if (it->second.second != state) {
            std::fprintf(g_file, "c,%.1f,%u,%08X,%08X,%08X,%08X\n", t, line, u, ppc::ld32(base, u), it->second.second,
                         state);
            it->second.second = state;
        }
    }
    for (uint32_t u : before)
        if (std::find(now.begin(), now.end(), u) == now.end()) {
            std::fprintf(g_file, "u,%.1f,-,%u,%08X,%08X,%08X\n", t, line, u, ppc::ld32(base, u), 0u);
            if (auto it = g_units.find(u); it != g_units.end() && it->second.first == line) g_units.erase(it);
        }
    before = std::move(now);
}

PPC_FUNC(above_head) {
    const uint32_t self = ctx.r[3];
    sub_025F2810__orig(ctx, base);
    const uint32_t child = ppc::ld32(base, self + 0x2E8);
    std::fprintf(g_file, "a,%.1f,%08X,%d\n", now_ms(), self,
                 child ? static_cast<int>(ppc::ld32(base, child + 0xC)) : -1);
}

}  // namespace

void register_play_log(std::function<void(uint32_t)> watch) {
    char* e = nullptr;
    size_t len = 0;
    std::string path;
    if (_dupenv_s(&e, &len, "MH3U_PLAY_LOG") == 0 && e) path = e;
    std::free(e);
    if (path.empty() || fopen_s(&g_file, path.c_str(), "w") != 0 || !g_file) return;
    g_start = std::chrono::steady_clock::now();
    g_watch = std::move(watch);
    std::printf("play log: %s\n", path.c_str());
    ppc::set_override(0x0227E138, &camera_pad_copy);
    ppc::set_override(0x025F2810, &above_head);
    ppc::set_override(0x02C0E43C, &unit_line);
}

}  // namespace mh3u::game
