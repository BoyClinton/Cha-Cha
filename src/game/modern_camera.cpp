// Camera Distance (issue #17, user 2026-11-10: CFG > Camera Settings): the quest camera's distance from what it looks
// at, in percent of the game's own - for the game's camera and the Modern Camera alike. The game's camera: FUN_02287D70
// (camera shake; overridden here) is handed the eye and look-at targets, block +0x70 / +0x80, before the eye and
// look-at heights are eased and the shake added; the eye target is pushed away from the look-at target there, so the
// easing works on the farther target (scaling the eased eye instead would feed back into the easing). Quests and free
// roam; Moga Village and Port Tanzia keep their fixed cameras (user 2026-11-10): FUN_0228984C runs none of this in
// town (sGame +0x48 == 2), and the factor is 1 there besides.
//
// The Modern Camera (user 2026-11-09; a mod until 2026-11-10, now CFG > Camera Settings, off by default): on quests and in the field the land camera is
// replaced by a free orbit camera, as in later games - the mouse moves it directly (1:1, a little smoothing), the
// right stick turns and tilts it at once (analog, diagonals included), and after a short pause it drifts back behind
// the running hunter. The game's L (reset behind / target camera), area cameras and cutscenes keep their behaviour:
// whatever the game itself sets is taken over. Under water the classic camera stays (it already looks up and down).
//
// Status: wip - read in Ghidra (2026-11-09); not yet seen in a quest. The pieces of the game's camera update
// uGameCamera::fieldUpdate? FUN_0228984C (cam = uGameCamera, block = cam+0x8E0, player = block+0x690):
//   FUN_02286890 cameraPitchInput?  on land, camera up / down steps a height level, block +0xA8 (0..5), one per
//                                   press (pad bits 1 / 3); its player-controlled path always ends with block
//                                   +0x12D = 0 (1 under water) - the mark used here; block +0x12C = 1 asks for the
//                                   level of the game's camera option (area changes)
//   block +0xAC                     the level's preset, 0x101FE460 + level x 0x30 (filled by FUN_02123134): +4 eye
//                                   height, +8 distance behind, +0x14 look-at height over the hunter's feet. From
//                                   level 0 to 5 they run from 43 degrees down (400 up, 220 back) to 31 up (50, 270),
//                                   the distance shortest at both ends: the shape of this camera's orbit
//   the land eye                    hunter + rotY((0, +4, +8), yaw block +0x9C); look-at hunter + (0, +0x14, 0);
//                                   rotY (FUN_0205D6DC): x' = cos x + sin z, z' = cos z - sin x (sin FUN_0312A4B4,
//                                   cos FUN_0312ACE8), angles u16 (0x10000 = a turn); block +0x94 the yaw target the
//                                   game eases +0x9C to (L sets it to facing + 0x8000: block+0x690's pad +0x120)
//   then shake offsets (block +0x140 / +0x160) are added, and FUN_02288824 (overridden here: monster-event camera,
//   roars) and FUN_0227FAD0 run on block +0x00 (eye) / +0x10 (look-at). The orbit camera writes those two at
//   FUN_02288824's entry, so shake, roars and what follows still apply on top.
// Pad block (player+0xE30): +0x3D 3 = under water, +0xE0 position, +0x120 facing, +0x1BE / +0x1C2 right stick angle
// (u16, 0x4000 up) / magnitude (0..0x7FFF, 0 inside the game's dead zone 0x1D4C). Camera options: block +0x697 bit 0
// flips left / right, bit 1 up / down.
#include "modern_camera.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "functions.h"
#include <atomic>

namespace mh3u::game {
namespace {

constexpr uint32_t kBlock = 0x8E0, kFrameTime = 0x8E4;  // uGameCamera
constexpr uint32_t kEye = 0x00, kLook = 0x10, kYawTarget = 0x94, kYaw = 0x9C, kLevel = 0xA8, kReset = 0x12C,
                   kMark = 0x12D, kShakeEye = 0x140, kShakeLook = 0x160, kOptions = 0x697;  // camera block
constexpr uint32_t kPad = 0xE30;                                                              // player
constexpr uint32_t kPresets = 0x101FE460, kPresetSize = 0x30;
constexpr int kLevels = 6;
constexpr float kUnitsPerDeg = 65536.0f / 360.0f;
constexpr float kPi = 3.14159265f;

// feel
constexpr float kStickYaw = 210.0f, kStickPitch = 130.0f;  // degrees a second, stick all the way
constexpr float kStickRamp = 0.06f;                         // s: stick rate easing in / out
constexpr float kMouseSmooth = 0.02f;                       // s: mouse movement spread over a frame or two
constexpr float kPivotSmooth = 0.12f;                       // s: the look-at point following jumps and slopes
constexpr float kFollowDelay = 0.8f, kFollowFade = 0.5f;    // s without camera input, then easing in
constexpr float kFollowRate = 1.3f;                         // 1/s, while running
constexpr float kTopDeg = 60.0f;                            // highest view (the presets stop at 43)

bool g_log = false;  // MH3U_CAMERA_LOG=1
std::atomic<bool> g_modern{false};      // set_modern_camera
std::atomic<float> g_distance{1.0f};   // set_camera_distance

constexpr uint32_t kSGame = 0x101FDB9C;  // sGame::mpInstance
// Camera Distance's factor now: the setting in the field (quests, free roam), 1 in town
float distance(const uint8_t* base) {
    const uint32_t game = ppc::ld32(base, kSGame);
    return game && base[game + 0x48] != 2 ? g_distance.load(std::memory_order_relaxed) : 1.0f;
}
ModernCameraMouse g_mouse;  // set_modern_camera_mouse (the host input layer)
void take_look(float& x, float& y) {
    if (g_mouse.take) g_mouse.take(x, y);
}
void set_direct_look(bool on) {
    if (g_mouse.direct) g_mouse.direct(on);
}

struct Point {
    float elev, dist, height;  // degrees above the look-at point, distance, look-at height
};
std::array<Point, kLevels> g_level;  // by level
std::array<Point, kLevels + 1> g_orbit;  // sorted by elevation, with the top added
bool g_have_orbit = false;

bool load_orbit(const uint8_t* base) {
    for (int i = 0; i < kLevels; ++i) {
        const uint32_t p = kPresets + static_cast<uint32_t>(i) * kPresetSize;
        const float y = ppc::ldf32(base, p + 4), z = ppc::ldf32(base, p + 8), h = ppc::ldf32(base, p + 0x14);
        if (!(z > 1 && std::isfinite(y) && h > 1 && h < 1000)) return false;  // not filled in yet
        g_level[static_cast<size_t>(i)] = {std::atan2(y - h, z) * 180.0f / kPi, std::hypot(y - h, z), h};
    }
    std::copy(g_level.begin(), g_level.end(), g_orbit.begin());
    std::sort(g_orbit.begin(), g_orbit.begin() + kLevels, [](const Point& a, const Point& b) { return a.elev < b.elev; });
    g_orbit[kLevels] = {std::max(kTopDeg, g_orbit[kLevels - 1].elev + 1), g_orbit[kLevels - 1].dist, g_orbit[kLevels - 1].height};
    return true;
}
Point orbit_at(float elev) {
    if (elev <= g_orbit.front().elev) return g_orbit.front();
    for (size_t i = 1; i < g_orbit.size(); ++i)
        if (elev <= g_orbit[i].elev) {
            const Point &a = g_orbit[i - 1], &b = g_orbit[i];
            float t = (elev - a.elev) / std::max(1e-3f, b.elev - a.elev);
            t = t * t * (3 - 2 * t);  // eased between the presets: no kinks in the distance
            return {elev, a.dist + (b.dist - a.dist) * t, a.height + (b.height - a.height) * t};
        }
    return g_orbit.back();
}

float wrap_units(float d) {  // -32768..32768
    d = std::fmod(d, 65536.0f);
    if (d > 32768.0f) d -= 65536.0f;
    if (d < -32768.0f) d += 65536.0f;
    return d;
}
float approach(float dt, float tau) { return tau <= 0 ? 1.0f : 1.0f - std::exp(-dt / tau); }

struct State {
    bool active = false;
    uint32_t block = 0;
    float yaw = 0, elev = 0;              // u16 units (unwrapped float), degrees
    float yaw_rate = 0, elev_rate = 0;    // stick, degrees a second
    float mouse_yaw = 0, mouse_elev = 0;  // mouse movement still to apply, degrees
    float pivot_y = 0, last_x = 0, last_z = 0;
    float idle = 0;                       // s without camera input
    bool snap = false;                    // the game set a yaw (L, target camera, an area's camera)
    float snap_yaw = 0;
    uint32_t written = 0;                 // the yaw written to block +0x94 last frame
    uint8_t level = 0;
};
State g_s;
bool g_candidate = false;  // this frame's pitch step took the player-controlled land path with the mod on

void log_state(const char* why) {
    if (!g_log) return;
    static int n = 0;
    if (++n % 30 && why[0] == 'f') return;
    std::printf("modern camera: %s yaw %04X elev %.1f level %d idle %.2f%s\n", why,
                static_cast<unsigned>(std::lround(g_s.yaw)) & 0xFFFF, g_s.elev, g_s.level, g_s.idle, g_s.snap ? " snap" : "");
}

void update(uint8_t* base, uint32_t cam, uint32_t block, uint32_t player) {
    float t = ppc::ldf32(base, cam + kFrameTime);
    if (!std::isfinite(t) || t <= 0 || t > 100) t = 1.0f;
    const float dt = t / 30.0f;
    const uint32_t pad = ppc::ld32(base, player + kPad);
    const float px = ppc::ldf32(base, pad + 0xE0), py = ppc::ldf32(base, pad + 0xE4), pz = ppc::ldf32(base, pad + 0xE8);
    const uint8_t level = std::min<uint8_t>(base[block + kLevel], kLevels - 1);

    if (!g_s.active || g_s.block != block) {  // take over the game's camera as it is
        g_s = State{};
        g_s.active = true;
        g_s.block = block;
        g_s.yaw = static_cast<float>(ppc::ld32(base, block + kYaw) & 0xFFFF);
        g_s.written = ppc::ld32(base, block + kYawTarget) & 0xFFFF;
        g_s.level = level;
        g_s.elev = g_level[level].elev;
        g_s.pivot_y = py;
        g_s.last_x = px, g_s.last_z = pz;
        log_state("start");
    }
    // what the game set since the last frame
    if (const uint32_t target = ppc::ld32(base, block + kYawTarget) & 0xFFFF; target != g_s.written) {
        g_s.snap = true;
        g_s.snap_yaw = static_cast<float>(target);
        log_state("game yaw");
    }
    if (level != g_s.level) {
        g_s.level = level;
        g_s.elev = g_level[level].elev;
        log_state("game level");
    }

    const uint8_t options = base[block + kOptions];
    const float yaw_sign = (options & 1) ? -1.0f : 1.0f, pitch_sign = (options & 2) ? -1.0f : 1.0f;
    // stick: analog, both axes at once (the game's own bits drop up / down while turning)
    float sx = 0, sy = 0;
    if (const int mag = static_cast<int16_t>(ppc::ld16(base, pad + 0x1C2)); mag > 0) {
        const float m = std::clamp(static_cast<float>(mag) / 32767.0f, 0.0f, 1.0f);
        const float resp = std::pow(std::clamp((m - 0.229f) / 0.771f, 0.0f, 1.0f), 1.5f);
        const float a = static_cast<float>(ppc::ld16(base, pad + 0x1BE)) * (2 * kPi / 65536.0f);
        sx = std::cos(a) * resp, sy = std::sin(a) * resp;
    }
    // stick left / up = mouse left / up: yaw up, camera down (looking up), as the game's own camera turns
    g_s.yaw_rate += (-sx * kStickYaw * yaw_sign - g_s.yaw_rate) * approach(dt, kStickRamp);
    g_s.elev_rate += (-sy * kStickPitch * pitch_sign - g_s.elev_rate) * approach(dt, kStickRamp);
    float mx = 0, my = 0;
    take_look(mx, my);
    set_direct_look(true);
    g_s.mouse_yaw += -mx * yaw_sign;
    g_s.mouse_elev += -my * pitch_sign;
    const float mouse_k = approach(dt, kMouseSmooth);
    const float myaw = g_s.mouse_yaw * mouse_k, melev = g_s.mouse_elev * mouse_k;
    g_s.mouse_yaw -= myaw, g_s.mouse_elev -= melev;
    g_s.yaw += (g_s.yaw_rate * dt + myaw) * kUnitsPerDeg;
    g_s.elev += g_s.elev_rate * dt + melev;

    const bool touched = std::fabs(sx) + std::fabs(sy) > 0.02f || mx != 0 || my != 0;
    g_s.idle = touched ? 0 : g_s.idle + dt;
    if (touched) g_s.snap = false;
    if (g_s.snap) {  // the game's own ease (shift 2: a quarter a frame)
        const float d = wrap_units(g_s.snap_yaw - g_s.yaw);
        g_s.yaw += d * (1.0f - std::pow(0.75f, t));
        if (std::fabs(d) < 8) g_s.yaw = g_s.snap_yaw, g_s.snap = false;
    } else if (g_s.idle > kFollowDelay) {  // drift behind the running hunter
        const float speed = std::hypot(px - g_s.last_x, pz - g_s.last_z) / std::max(dt, 1e-4f);
        const float run = std::clamp((speed - 80.0f) / 400.0f, 0.0f, 1.0f);
        const float fade = std::clamp((g_s.idle - kFollowDelay) / kFollowFade, 0.0f, 1.0f);
        const float behind = static_cast<float>((ppc::ld32(base, pad + 0x120) + 0x8000) & 0xFFFF);
        const float d = wrap_units(behind - g_s.yaw), deg = std::fabs(d) / kUnitsPerDeg;
        const float facing = std::clamp((140.0f - deg) / 40.0f, 0.0f, 1.0f);  // not when running at the camera
        g_s.yaw += d * (1.0f - std::exp(-kFollowRate * run * fade * facing * dt));
    }
    g_s.elev = std::clamp(g_s.elev, g_orbit.front().elev, g_orbit.back().elev);
    g_s.yaw = std::fmod(g_s.yaw, 65536.0f);
    if (g_s.yaw < 0) g_s.yaw += 65536.0f;

    // the game's yaw follows ours: hunter movement, L, the auto-turn's maths all see this camera
    const uint32_t yaw = static_cast<uint32_t>(std::lround(g_s.yaw)) & 0xFFFF;
    ppc::st32(base, block + kYawTarget, yaw);
    ppc::st32(base, block + kYaw, yaw);
    g_s.written = yaw;

    if (std::fabs(py - g_s.pivot_y) > 400) g_s.pivot_y = py;  // a warp
    g_s.pivot_y += (py - g_s.pivot_y) * approach(dt, kPivotSmooth);
    g_s.last_x = px, g_s.last_z = pz;
    Point o = orbit_at(g_s.elev);
    o.dist *= distance(base);
    const float yr = static_cast<float>(yaw) * (2 * kPi / 65536.0f), er = g_s.elev * (kPi / 180.0f);
    const float back = o.dist * std::cos(er);
    const float lx = px, ly = g_s.pivot_y + o.height, lz = pz;
    const float ex = lx + std::sin(yr) * back, ey = ly + o.dist * std::sin(er), ez = lz + std::cos(yr) * back;
    ppc::stf32(base, block + kEye + 0, ex + ppc::ldf32(base, block + kShakeEye + 0));
    ppc::stf32(base, block + kEye + 4, ey + ppc::ldf32(base, block + kShakeEye + 4));
    ppc::stf32(base, block + kEye + 8, ez + ppc::ldf32(base, block + kShakeEye + 8));
    ppc::stf32(base, block + kLook + 0, lx + ppc::ldf32(base, block + kShakeLook + 0));
    ppc::stf32(base, block + kLook + 4, ly + ppc::ldf32(base, block + kShakeLook + 4));
    ppc::stf32(base, block + kLook + 8, lz + ppc::ldf32(base, block + kShakeLook + 8));
    log_state("frame");
}

// FUN_0285E2B0(player, 0, 5 / 6 / 7): the hunter states with the game's fixed side camera (FUN_0228984C's other
// branch: look-at only 30 over the feet) - left to the game
bool fixed_state(ppc::Context& ctx, uint8_t* base, uint32_t player) {
    for (uint32_t s = 5; s <= 7; ++s) {
        ctx.r[3] = player, ctx.r[4] = 0, ctx.r[5] = s;
        sub_0285E2B0(ctx, base);
        if (ctx.r[3] & 0xFF) return true;
    }
    return false;
}

PPC_FUNC(camera_shake) {  // FUN_02287D70 (cam, block, player): the eye target pushed back first
    const uint32_t block = ctx.r[4];
    const float k = distance(base);
    if (block && std::fabs(k - 1.0f) > 1e-3f) {
        for (uint32_t i = 0; i < 12; i += 4) {
            const float eye = ppc::ldf32(base, block + 0x70 + i), look = ppc::ldf32(base, block + 0x80 + i);
            if (std::isfinite(eye) && std::isfinite(look)) ppc::stf32(base, block + 0x70 + i, look + (eye - look) * k);
        }
    }
    sub_02287D70__orig(ctx, base);
}

PPC_FUNC(event_camera) {  // FUN_02288824 (cam, block, player)
    const uint32_t cam = ctx.r[3], block = ctx.r[4], player = ctx.r[5];
    const bool candidate = g_candidate && block == ppc::ld32(base, cam + kBlock);
    g_candidate = false;
    if (candidate && (g_have_orbit || (g_have_orbit = load_orbit(base))) && !fixed_state(ctx, base, player)) {
        update(base, cam, block, player);
    } else if (g_s.active) {
        g_s.active = false;
        set_direct_look(false);
        log_state("off");
    }
    ctx.r[3] = cam, ctx.r[4] = block, ctx.r[5] = player;
    sub_02288824__orig(ctx, base);
}

}  // namespace

ModernPitchSave modern_before_pitch(uint8_t* base, uint32_t block) {
    ModernPitchSave s;
    g_candidate = false;
    if (!g_modern.load(std::memory_order_relaxed)) {
        if (g_s.active) g_s.active = false, set_direct_look(false);
        return s;
    }
    s.on = true;
    s.mark = base[block + kMark], s.level = base[block + kLevel], s.reset = base[block + kReset];
    base[block + kMark] = 0xAA;
    return s;
}

void modern_after_pitch(uint8_t* base, uint32_t block, const ModernPitchSave& saved) {
    if (!saved.on) return;
    if (base[block + kMark] == 0xAA) {  // the step ran no camera control (cutscene, menu, game camera)
        base[block + kMark] = saved.mark;
        return;
    }
    if (base[block + kMark] != 0) return;  // under water: the classic camera
    g_candidate = true;
    // the land step's height levels are ours now: undo a press, keep the game's own reset (block +0x12C)
    if (!saved.reset) base[block + kLevel] = saved.level;
}

bool modern_camera_engaged() { return g_candidate || g_s.active; }

void set_modern_camera(bool on) { g_modern.store(on, std::memory_order_relaxed); }
void set_camera_distance(float factor) { g_distance.store(std::clamp(factor, 0.5f, 3.0f), std::memory_order_relaxed); }

void set_modern_camera_mouse(ModernCameraMouse mouse) { g_mouse = mouse; }

void register_modern_camera() {
    char* e = nullptr;
    size_t len = 0;
    g_log = _dupenv_s(&e, &len, "MH3U_CAMERA_LOG") == 0 && e && std::string(e) == "1";
    std::free(e);
    ppc::set_override(0x02288824, &event_camera);
    ppc::set_override(0x02287D70, &camera_shake);
}

}  // namespace mh3u::game
