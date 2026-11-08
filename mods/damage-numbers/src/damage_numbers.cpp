// Damage Numbers for Cha-Cha (MH3U US v32): every hit on a monster pops its damage over the monster, in the style
// of MH Wilds, MH Rise or the classic games. A Cha-Cha plugin (include/chacha_plugin.h, docs/modding.md).
//
// How it reads the game (docs/research-log.md, 2026-11-02 "Damage numbers"):
//   uEnemy::damageCheck? 0x025CB37C (r3 = the uEnemy) runs once a frame per monster. state = *(uEnemy + 0x1BE0):
//     state+0x8EC  int16[8]  damage taken this frame per part (physical + element), summed and taken off the HP
//     state+0xAB4  int16     further damage (taken off the HP the same way, not by part)
//     state+0x8B8  int32     HP, state+0x8BC max HP
//   uCoord (uEnemy derives from it): +0x40 mPos, +0x50 mQuat, +0x60 mScale; uBaseModel +0x158 mBoundaryScale,
//   +0x15C mBoundaryCenter (the model's bounds, local).
// The parts' damage is read before the check and the HP after it, so a number only shows when HP really went down.
// In an online session only the copy that controls a monster takes damage off in the check; the others get the new HP in a network
// message, between checks, with no parts to read. So each monster's HP is remembered from one check to the next, and a drop between
// checks shows as one number.
#include "chacha_plugin.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kDamageCheck = 0x025CB37C;
constexpr const char* kGameSha1 = "d97aabfc5a45362f88dfacb09f23c6dcae6faac2";  // MH3U US v32

enum Style { kWilds, kRise, kClassic, kStyles };
const char* const kStyleNames[kStyles] = {"Wilds", "Rise", "Classic"};
const char* const kStyleKeys[kStyles] = {"wilds", "rise", "classic"};

struct Settings {
    int enabled = 1;
    int other = 0;  // damage not taken by a part (state+0xAB4, and anything else the HP lost)
    int style = kWilds;
    float scale = 1.0f;     // 0.5 .. 3.0
    float lifetime = 0.9f;  // seconds, 0.4 .. 2.0
};

struct Popup {
    float world[3];
    float dx;  // spread in pixels so hits of the same frame do not overlap
    int value;
    bool other;
    double born;
};

const ChaChaApi* api = nullptr;
ChaChaPlugin* self = nullptr;
Settings g_set;                              // main thread (settings, HUD)
std::atomic<bool> g_on{true}, g_show_other{false};  // what the game threads' hooks read
void* g_font = nullptr;
void publish() {
    g_on = g_set.enabled != 0;
    g_show_other = g_set.other != 0;
}
std::mutex g_m;
std::vector<Popup> g_popups;  // guarded by g_m
uint32_t g_rng = 0x2545F491u;

float frand() {  // -1 .. 1
    g_rng ^= g_rng << 13, g_rng ^= g_rng >> 17, g_rng ^= g_rng << 5;
    return static_cast<float>(g_rng & 0xFFFF) / 32767.5f - 1.0f;
}

bool finite3(const float* v, float limit) {
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(v[i]) || std::fabs(v[i]) > limit) return false;
    return true;
}

// the monster's middle in the world: its bounds' centre turned by its rotation, else a little above its origin
void body_centre(uint32_t enemy, float out[3]) {
    const uint8_t* b = api->guest_base;
    float pos[3], q[4], scl[3], c[3];
    for (int i = 0; i < 3; ++i) {
        pos[i] = chacha_ldf(b, enemy + 0x40 + 4 * i);
        scl[i] = chacha_ldf(b, enemy + 0x60 + 4 * i);
        c[i] = chacha_ldf(b, enemy + 0x15C + 4 * i);
    }
    for (int i = 0; i < 4; ++i) q[i] = chacha_ldf(b, enemy + 0x50 + 4 * i);
    if (!finite3(scl, 100.0f)) scl[0] = scl[1] = scl[2] = 1.0f;
    if (!finite3(c, 5000.0f) || (c[0] == 0 && c[1] == 0 && c[2] == 0)) c[0] = 0, c[1] = 120.0f, c[2] = 0;
    for (int i = 0; i < 3; ++i) c[i] *= scl[i];
    // v' = v + 2w(q x v) + 2(q x (q x v)) with q = (x, y, z), w
    const float ql = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (std::isfinite(ql) && ql > 0.5f && ql < 1.5f) {
        const float x = q[0] / ql, y = q[1] / ql, z = q[2] / ql, w = q[3] / ql;
        const float tx = 2 * (y * c[2] - z * c[1]), ty = 2 * (z * c[0] - x * c[2]), tz = 2 * (x * c[1] - y * c[0]);
        const float r0 = c[0] + w * tx + (y * tz - z * ty), r1 = c[1] + w * ty + (z * tx - x * tz), r2 = c[2] + w * tz + (x * ty - y * tx);
        c[0] = r0, c[1] = r1, c[2] = r2;
    }
    for (int i = 0; i < 3; ++i) out[i] = pos[i] + c[i];
}

// what the damage check found before it ran, per thread (each monster's check runs on the thread updating it)
struct Before {
    uint32_t enemy = 0, state = 0;
    int32_t hp = 0;
    int16_t parts[8] = {};
};
thread_local Before t_before;

void before_check(void*, ChaChaCall* call) {
    t_before.enemy = 0;
    if (!g_on) return;
    const uint32_t enemy = call->arg_r[0];
    if (!enemy || !api->readable(enemy, 0x1BE4)) return;
    const uint32_t state = chacha_ld32(api->guest_base, enemy + 0x1BE0);
    if (!state || !api->readable(state, 0xAB8)) return;
    t_before.enemy = enemy;
    t_before.state = state;
    if (std::getenv("CHACHA_DN_DEBUG")) {  // every monster's first check: HP and where it would show
        static std::mutex m;
        static std::vector<uint32_t> seen;
        std::lock_guard g(m);
        if (std::find(seen.begin(), seen.end(), enemy) == seen.end() && seen.size() < 64) {
            seen.push_back(enemy);
            float c[3], sx = 0, sy = 0, depth = 0;
            body_centre(enemy, c);
            const int on = api->world_to_screen(c, &sx, &sy, &depth);
            char msg[200];
            std::snprintf(msg, sizeof msg, "debug: uEnemy %08X em%03u HP %d / %d, centre (%.0f %.0f %.0f) -> %s %.0f,%.0f depth %.0f", enemy,
                          chacha_ld8(api->guest_base, enemy + 0xE33), static_cast<int32_t>(chacha_ld32(api->guest_base, state + 0x8B8)),
                          static_cast<int32_t>(chacha_ld32(api->guest_base, state + 0x8BC)), c[0], c[1], c[2], on ? "screen" : "off",
                          sx, sy, depth);
            api->log(self, msg);
        }
    }
    t_before.hp = static_cast<int32_t>(chacha_ld32(api->guest_base, state + 0x8B8));
    for (int i = 0; i < 8; ++i) t_before.parts[i] = static_cast<int16_t>(chacha_ld16(api->guest_base, state + 0x8EC + 2 * i));
}

// HP after each monster's last check. Few monsters exist at once, and a check is once a frame per monster.
struct LastHp {
    uint32_t enemy, state;
    int32_t hp;
};
std::mutex g_last_m;
std::vector<LastHp> g_last;

// The drop since this monster's last check (0 for a monster not seen before, or one reused for another), and remembers its HP now.
int32_t drop_since_last_check(uint32_t enemy, uint32_t state, int32_t hp_before_check, int32_t hp_now) {
    std::lock_guard g(g_last_m);
    for (LastHp& l : g_last)
        if (l.enemy == enemy) {
            const int32_t drop = l.state == state ? l.hp - hp_before_check : 0;
            l.state = state;
            l.hp = hp_now;
            return std::max(drop, 0);
        }
    if (g_last.size() >= 32) g_last.erase(g_last.begin());
    g_last.push_back({enemy, state, hp_now});
    return 0;
}

void after_check(void*, ChaChaCall* call) {
    const Before b = t_before;
    t_before.enemy = 0;
    if (!b.enemy || b.enemy != call->arg_r[0]) return;
    const int32_t hp = static_cast<int32_t>(chacha_ld32(api->guest_base, b.state + 0x8B8));
    if (const int32_t remote = drop_since_last_check(b.enemy, b.state, b.hp, hp); remote > 0 && b.hp > 0) {
        float centre[3];
        body_centre(b.enemy, centre);
        static std::atomic<int> logged_remote{0};
        if (logged_remote < 12) {
            ++logged_remote;
            char msg[120];
            std::snprintf(msg, sizeof msg, "network hit: em%03u HP %d -> %d", chacha_ld8(api->guest_base, b.enemy + 0xE33), b.hp + remote, b.hp);
            api->log(self, msg);
        }
        const double now = api->time();
        std::lock_guard g(g_m);
        g_popups.push_back({{centre[0], centre[1], centre[2]}, frand() * 40.0f, remote, false, now});
    }
    const int32_t lost = b.hp - hp;
    if (lost <= 0) return;  // nothing taken off (healing, a hit that did not count)
    int sum = 0;
    for (int16_t p : b.parts) sum += std::max<int>(0, p);
    float centre[3];
    body_centre(b.enemy, centre);
    static std::atomic<int> logged{0};
    if (logged < 12) {  // the first hits in the log, to check positions and values (MODS issues)
        ++logged;
        float sx = 0, sy = 0, depth = 0;
        const int on = api->world_to_screen(centre, &sx, &sy, &depth);
        char msg[200];
        std::snprintf(msg, sizeof msg, "hit: em%03u HP %d -> %d, parts %d %d %d %d %d %d %d %d, at (%.0f %.0f %.0f) -> %s %.0f,%.0f",
                      chacha_ld8(api->guest_base, b.enemy + 0xE33), b.hp, hp, b.parts[0], b.parts[1], b.parts[2], b.parts[3], b.parts[4],
                      b.parts[5], b.parts[6], b.parts[7], centre[0], centre[1], centre[2], on ? "screen" : "off screen", sx, sy);
        api->log(self, msg);
    }
    const double now = api->time();
    std::lock_guard g(g_m);
    int n = 0;
    for (int16_t p : b.parts)
        if (p > 0) {
            g_popups.push_back({{centre[0], centre[1], centre[2]}, frand() * 40.0f + n * 26.0f, p, false, now});
            ++n;
        }
    if (g_show_other && lost > sum) g_popups.push_back({{centre[0], centre[1], centre[2]}, frand() * 40.0f, lost - sum, true, now});
    if (g_popups.size() > 96) g_popups.erase(g_popups.begin(), g_popups.begin() + static_cast<long>(g_popups.size() - 96));
}

// ---- drawing ----------------------------------------------------------------------------------------------
float ease_out(float t) { return 1.0f - (1.0f - t) * (1.0f - t); }

void draw_popup(const Popup& p, float age, float base_px) {
    float sx, sy, depth;
    if (!api->world_to_screen(p.world, &sx, &sy, &depth)) return;
    const float life = g_set.lifetime, t = std::clamp(age / life, 0.0f, 1.0f);
    const float fade = t < 0.65f ? 1.0f : 1.0f - (t - 0.65f) / 0.35f;
    const uint32_t alpha = static_cast<uint32_t>(std::clamp(fade, 0.0f, 1.0f) * 255.0f);
    char text[16];
    std::snprintf(text, sizeof text, "%d", p.value);
    ChaChaText tx{};
    tx.align = CHACHA_ALIGN_CENTER;
    tx.font = g_font;
    const float px = base_px * g_set.scale;
    switch (g_set.style) {
    case kWilds: {  // outlined digits that pop in and drift up
        const float pop = age < 0.10f ? 1.0f + 0.45f * (1.0f - age / 0.10f) : 1.0f;
        tx.size = px * pop;
        tx.x = sx + p.dx * g_set.scale;
        tx.y = sy - ease_out(t) * px * 1.4f;
        tx.color = (p.other ? 0xB4B4B400u : 0xFFFFFF00u) | alpha;
        tx.outline_color = 0x10080400u | (alpha * 220 / 255);
        tx.outline = std::max(1.5f, px * 0.075f);
        api->draw_text(&tx, text);
        break;
    }
    case kRise: {  // slanted, warm, flicked up and to the right
        tx.size = px * (age < 0.08f ? 1.25f - 0.25f * age / 0.08f : 1.0f);
        tx.x = sx + p.dx * g_set.scale + ease_out(t) * px * 0.5f;
        tx.y = sy - ease_out(t) * px * 1.1f;
        tx.color = (p.other ? 0xC8B8A800u : 0xFFF2D200u) | alpha;
        tx.outline_color = 0x3A1A0600u | (alpha * 230 / 255);
        tx.outline = std::max(1.5f, px * 0.065f);
        tx.shear = 0.24f;
        // a short slash behind the number
        float w, h;
        api->text_size(g_font, tx.size, text, &w, &h);
        const float x0 = tx.x - w * 0.7f, x1 = tx.x + w * 0.7f, y0 = tx.y + h * 0.42f, y1 = tx.y - h * 0.18f;
        api->draw_line(x0, y0, x1, y1, 0xFF9A3C00u | (alpha * 150 / 255), std::max(2.0f, px * 0.08f));
        api->draw_text(&tx, text);
        break;
    }
    default: {  // classic: small plain digits, straight up
        tx.size = px * 0.8f;
        tx.x = sx + p.dx * g_set.scale * 0.6f;
        tx.y = sy - t * px * 1.8f;
        tx.color = (p.other ? 0xB0B0B000u : 0xFFFFFF00u) | alpha;
        tx.outline_color = 0x00000000u | (alpha * 160 / 255);
        tx.outline = 1.0f;
        api->draw_text(&tx, text);
        break;
    }
    }
}

void on_hud(ChaChaPlugin*) {
    const double now = api->time();
    std::vector<Popup> live;
    {
        std::lock_guard g(g_m);
        g_popups.erase(std::remove_if(g_popups.begin(), g_popups.end(), [&](const Popup& p) { return now - p.born > g_set.lifetime; }),
                       g_popups.end());
        if (!g_set.enabled) g_popups.clear();
        live = g_popups;
    }
    if (live.empty()) return;
    float gx, gy, gw, gh;
    api->game_rect(&gx, &gy, &gw, &gh);
    const float base_px = gh * 0.042f;  // follows the picture's height
    for (const Popup& p : live) draw_popup(p, static_cast<float>(now - p.born), base_px);
}

// ---- settings ---------------------------------------------------------------------------------------------
void load_settings() {
    Settings s;
    const auto num = [](const char* v, float d) {
        char* end = nullptr;
        const float f = v ? std::strtof(v, &end) : d;
        return v && end != v && std::isfinite(f) ? f : d;
    };
    const char* en = api->config_get(self, "enabled", "true");
    s.enabled = std::strcmp(en, "false") != 0 && std::strcmp(en, "0") != 0;
    const char* ot = api->config_get(self, "other", "false");
    s.other = std::strcmp(ot, "true") == 0 || std::strcmp(ot, "1") == 0;
    const char* st = api->config_get(self, "style", "wilds");
    for (int i = 0; i < kStyles; ++i)
        if (std::strcmp(st, kStyleKeys[i]) == 0) s.style = i;
    s.scale = std::clamp(num(api->config_get(self, "scale", nullptr), 1.0f), 0.5f, 3.0f);
    s.lifetime = std::clamp(num(api->config_get(self, "lifetime", nullptr), 0.9f), 0.4f, 2.0f);
    g_set = s;
    publish();
}
bool save_settings() {
    char buf[32];
    api->config_set(self, "enabled", g_set.enabled ? "true" : "false");
    api->config_set(self, "other", g_set.other ? "true" : "false");
    api->config_set(self, "style", kStyleKeys[g_set.style]);
    std::snprintf(buf, sizeof buf, "%.2f", g_set.scale);
    api->config_set(self, "scale", buf);
    std::snprintf(buf, sizeof buf, "%.2f", g_set.lifetime);
    api->config_set(self, "lifetime", buf);
    return api->config_save(self) != 0;
}

void on_settings(ChaChaPlugin*) {
    api->ui_group("Settings");
    api->ui_switch("Show damage numbers", &g_set.enabled, "Every hit on a monster shows the damage it did, over the monster.");
    api->ui_switch("Show other damage", &g_set.other,
                   "Also shows HP the monster lost that no hit on a part accounts for (bombs, poison and the like), in grey.");
    api->ui_choice("Style", &g_set.style, kStyleNames, kStyles,
                   "Wilds: outlined digits that pop in. Rise: slanted digits with a slash. Classic: small plain digits.");
    api->ui_float("Number size", &g_set.scale, 0.5f, 3.0f, 0.1f, "%.1fx", "How big the numbers are. They follow the picture's height.");
    api->ui_float("Duration", &g_set.lifetime, 0.4f, 2.0f, 0.1f, "%.1f s", "How long each number stays on screen.");
    publish();
}
// the Mods page's Save settings / Restore defaults (for all mods, at its end)
void on_save(ChaChaPlugin*) {
    if (!save_settings()) api->toast(self, "Damage Numbers: settings could not be saved");
}
void on_defaults(ChaChaPlugin*) {
    g_set = Settings{};
    publish();
}

}  // namespace

extern "C" CHACHA_EXPORT int chacha_plugin_init(const ChaChaApi* a, ChaChaPlugin* p) {
    api = a;
    self = p;
    p->name = "Damage Numbers";
    p->version = "1.0";
    if (a->version < 1 || std::strcmp(a->game_sha1, kGameSha1) != 0) {
        a->log(p, "made for MH3U US v32 - this game's executable differs, staying off");
        return 0;
    }
    load_settings();
    p->on_hud = &on_hud;
    p->on_settings = &on_settings;
    if (chacha_has_save_hooks(a)) {  // a host with the Mods page's Save / Restore rows
        p->on_save = &on_save;
        p->on_defaults = &on_defaults;
    }
    if (!a->hook(p, kDamageCheck, &before_check, &after_check, nullptr)) {
        a->log(p, "this Cha-Cha has no hook point for the damage check (needs Cha-Cha with mod support)");
        return 0;
    }
    // a font of the mod's own, if one is put next to the plugin (fonts/*.ttf); else Cha-Cha's HUD font
    g_font = a->load_font(p, "fonts/damage.ttf");
    a->log(p, "ready");
    return CHACHA_API_VERSION;
}
