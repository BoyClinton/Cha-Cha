// The game camera's turn speed and easing, made independent of the frame rate (issues #2, #6, #7: above 30 FPS the
// camera turned too fast and stopped abruptly - the same happens on the console versions when their frame rate is
// unlocked).
//
// Replaces (all called from the uGameCamera update, r3 = the uGameCamera; status: wip - read in Ghidra, the steps
// below are the original ones times the frame time; 30 FPS gives the original results exactly. Not yet seen in game:
// the village camera is fixed and runs none of this, so it needs a field / quest run, MH3U_CAMERA_LOG=1 shows it):
//   FUN_0227FFC0 cameraEase?      (cam, target, current, shift) -> (s16)(target - current) >> shift, the step that
//                                 moves the camera's angles (block +0x98 / +0x9C / +0xA4) toward their targets
//                                 (+0x90 / +0x94 / +0xA0) once a frame (shift 2, 5)
//   FUN_02286DE8 cameraAutoTurn?  (f1-f3 curve, cam, angle) -> s16 step that swings the camera behind the hunter
//   FUN_02286F18 cameraTurnInput? (cam, block, player): while camera left / right is held (player pad +0x1C8 bits 6
//                                 / 8, bit 9 = camera control on) the target yaw +0x94 moves by
//                                 K * ramp / max a frame, K = 0x40B (0x34B with +0xC0, 0x2EB with +0x129 == 1),
//                                 ramp +0xE4 counting the frame time up to max (5, 10 under water)
//   FUN_02286890 cameraPitchInput? (cam, block, player): under water (pad +0x3D == 3) camera up / down (bits 2 / 4,
//                                 swapped by the inverted-camera bit 2 of +0x697) move the target pitch +0x90 by
//                                 K * ramp / 12 a frame, K = 0x230 (0x170 with +0xC0 == 0 and +0x129 == 1), ramp
//                                 +0xD8, clamped to +-0x3E94; +0x110 = 1, +0x114 = 30.0 mark that path
// None of these steps uses the frame time. The camera's own frame time, uGameCamera+0x8E4 (= cUnit+0x1C x 0.5:
// 1.0 at the console's 30 FPS, 0.25 at 120), is what the rest of its update scales by; here it scales these steps
// too: turns are multiplied by it, the ease becomes 1 - (1 - 2^-shift)^t, so 30 FPS stays bit for bit the original.
#include "camera_speed.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "functions.h"
#include "modern_camera.h"

namespace mh3u::game {
namespace {

constexpr uint32_t kFrameTime = 0x8E4;  // uGameCamera: float, 1.0 = one console frame (1/30 s)
constexpr uint32_t kBlock = 0x8E0;      // uGameCamera: the camera work block
constexpr uint32_t kPad = 0xE30;        // player: its pad / state block (+0x1C8 input bits, +0x3D state)
bool g_log = false;                     // MH3U_CAMERA_LOG=1

float frame_time(const uint8_t* base, uint32_t cam) {
    const float t = ppc::ldf32(base, cam + kFrameTime);
    return std::isfinite(t) && t > 0 && t < 100 ? t : 1.0f;
}
bool same(float t) { return std::fabs(t - 1.0f) < 1e-4f; }
bool input(const uint8_t* base, uint32_t player, int bit) {
    return (ppc::ld32(base, ppc::ld32(base, player + kPad) + 0x1C8) >> bit) & 1;
}
// a turn step as the game computes it: (int)(k * ramp / max), low 16 bits
uint32_t turn_step(float k, float ramp, float max, float t) { return static_cast<uint32_t>(static_cast<int>(k * (ramp / max) * t)) & 0xFFFF; }

// MH3U_CAMERA_LOG: once a second, how often the ease and the turn input ran, the frame time and the yaw
int g_eases = 0, g_turns = 0;
void log_update(const uint8_t* base, uint32_t cam) {
    static auto last = std::chrono::steady_clock::now();
    if (const auto now = std::chrono::steady_clock::now(); now - last >= std::chrono::seconds(1)) {
        const uint32_t block = ppc::ld32(base, cam + kBlock);
        std::printf("camera: %d eases/s, %d turn inputs/s, frame time %.3f, yaw target %04X now %04X\n", g_eases,
                    g_turns, frame_time(base, cam), ppc::ld32(base, block + 0x94) & 0xFFFF,
                    ppc::ld32(base, block + 0x9C) & 0xFFFF);
        last = now;
        g_eases = g_turns = 0;
    }
}

PPC_FUNC(camera_ease) {
    const int diff = static_cast<int16_t>(ctx.r[4] - ctx.r[5]);
    const int shift = static_cast<int>(ctx.r[6] & 0x3F);
    const float t = frame_time(base, ctx.r[3]);
    ++g_eases;
    if (g_log) log_update(base, ctx.r[3]);
    if (same(t) || shift > 15) {
        ctx.r[3] = static_cast<uint32_t>(diff >> shift);
        return;
    }
    const double f = std::min(1.0, 1.0 - std::pow(1.0 - std::ldexp(1.0, -shift), static_cast<double>(t)));
    ctx.r[3] = static_cast<uint32_t>(static_cast<int>(std::lround(diff * f)));
}

PPC_FUNC(camera_auto_turn) {
    if (modern_camera_engaged()) {  // the Modern Camera follows the hunter its own way
        ctx.r[3] = 0;
        return;
    }
    const float t = frame_time(base, ctx.r[3]);
    sub_02286DE8__orig(ctx, base);
    if (!same(t)) ctx.r[3] = static_cast<uint32_t>(static_cast<int16_t>(std::lround(static_cast<int16_t>(ctx.r[3]) * t)));
}

PPC_FUNC(camera_turn_input) {
    const uint32_t cam = ctx.r[3], block = ctx.r[4], player = ctx.r[5];
    const uint32_t before = ppc::ld32(base, block + 0x94);
    sub_02286F18__orig(ctx, base);
    if (modern_camera_engaged()) {  // the Modern Camera reads the stick itself; L and the game's other yaws stay
        if (input(base, player, 9) && (input(base, player, 6) || input(base, player, 8))) ppc::st32(base, block + 0x94, before);
        return;
    }
    const float t = frame_time(base, cam);
    ++g_turns;
    if (same(t) || !input(base, player, 9) || !(input(base, player, 6) || input(base, player, 8))) return;
    const uint32_t pad = ppc::ld32(base, player + kPad);
    float k = 843.0f;  // 0x34B
    if (base[block + 0xC0] == 0) k = base[block + 0x129] == 1 ? 747.0f : 1035.0f;  // 0x2EB : 0x40B
    const float max = base[pad + 0x3D] == 3 ? 10.0f : 5.0f, ramp = ppc::ldf32(base, block + 0xE4);
    const bool flip = base[ppc::ld32(base, cam + kBlock) + 0x697] & 1;
    const auto signed_step = [&](float scale) {
        const uint32_t s = turn_step(k, ramp, max, scale);
        return flip ? (0u - s) & 0xFFFF : s;
    };
    const bool left = input(base, player, 6);
    const uint32_t expected = left ? before + signed_step(1) : before - signed_step(1);
    if (ppc::ld32(base, block + 0x94) != expected) return;  // another path set the yaw (lock-on, reset behind)
    ppc::st32(base, block + 0x94, left ? before + signed_step(t) : before - signed_step(t));
}

void camera_pitch_step(ppc::Context& ctx, uint8_t* base) {
    const uint32_t cam = ctx.r[3], block = ctx.r[4], player = ctx.r[5];
    const uint32_t before = ppc::ld32(base, block + 0x90);
    sub_02286890__orig(ctx, base);
    const float t = frame_time(base, cam);
    const uint32_t pad = ppc::ld32(base, player + kPad);
    if (same(t) || base[pad + 0x3D] != 3 || base[block + 0x110] != 1 || ppc::ldf32(base, block + 0x114) != 30.0f)
        return;
    const bool inv = base[ppc::ld32(base, cam + kBlock) + 0x697] & 2;
    const bool b2 = input(base, player, 2), b4 = input(base, player, 4);
    int dir = 0;
    if (!inv) dir = b2 ? 1 : b4 ? -1 : 0;
    else dir = b4 ? 1 : b2 ? -1 : 0;
    if (!dir) return;
    const float k = base[block + 0xC0] == 0 && base[block + 0x129] == 1 ? 368.0f : 560.0f;  // 0x170 : 0x230
    const float ramp = ppc::ldf32(base, block + 0xD8);
    const auto pitch = [&](float scale) {
        const uint32_t s = turn_step(k, ramp, 12.0f, scale);
        uint32_t v = dir > 0 ? before + s : before - s;
        if (dir > 0 && static_cast<int16_t>(v) > 0x3E94) v = 0x3E94;
        if (dir < 0 && static_cast<int16_t>(v) < -0x3E94) v = static_cast<uint32_t>(-0x3E94);
        return v;
    };
    if (ppc::ld32(base, block + 0x90) != pitch(1)) return;
    ppc::st32(base, block + 0x90, pitch(t));
}

PPC_FUNC(camera_pitch_input) {
    const uint32_t block = ctx.r[4];
    const ModernPitchSave saved = modern_before_pitch(base, block);
    camera_pitch_step(ctx, base);
    modern_after_pitch(base, block, saved);
}

}  // namespace

void register_camera_speed() {
    char* e = nullptr;
    size_t len = 0;
    g_log = _dupenv_s(&e, &len, "MH3U_CAMERA_LOG") == 0 && e && std::string(e) == "1";
    std::free(e);
    ppc::set_override(0x0227FFC0, &camera_ease);
    ppc::set_override(0x02286DE8, &camera_auto_turn);
    ppc::set_override(0x02286F18, &camera_turn_input);
    ppc::set_override(0x02286890, &camera_pitch_input);
    register_modern_camera();  // the Modern Camera mod hooks in through the steps above
}

}  // namespace mh3u::game
