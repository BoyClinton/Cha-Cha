// Charm Table Fix for Cha-Cha (MH3U US v32): every talisman can come out of every save. A Cha-Cha plugin
// (include/chacha_plugin.h, docs/modding.md).
//
// The bug (docs/research-log.md, 2026-11-11 "Charm tables"): the talisman generator 0x0217F63C (makeTalisman?)
// draws every number from the game's RNG slot 1, sGame+0x42 (sGame = *(0x101FDB9C); 0x0211971C steps a slot
// x -> (x || 1) * 176 mod 65363). 65363 = 163 * 401 is not prime, so the 65362 states fall into 17 separate
// cycles: twelve of 5400, two of 200 and three of 54 states. The save keeps the state and nothing reseeds it, so a
// save never leaves the cycle it started on, and a talisman is decided by the state its first draw starts from:
// those cycles are the 17 "charm tables", and the five short ones are the "cursed" tables.
//
// The fix: while the generator runs, slot 1 starts from a state picked at random from all 65362, so each talisman
// is rolled as if from any table, with the game's own odds. Afterwards the save's own state is put back and moved on
// by as many draws as the generator made, exactly as without the mod: the save stays on its table, and switching
// the mod off later continues where the game would have been.
#include "chacha_plugin.h"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <random>

namespace {

constexpr uint32_t kMakeTalisman = 0x0217F63C;
constexpr uint32_t kGamePtr = 0x101FDB9C;  // sGame::mpInstance
constexpr uint32_t kSlot1 = 0x42;          // sGame +0x40: u16 RNG slots 0..3
constexpr uint32_t kModulus = 65363, kMultiplier = 176;
constexpr int kMaxDraws = 64;  // the generator makes 3 to 8
constexpr const char* kGameSha1 = "d97aabfc5a45362f88dfacb09f23c6dcae6faac2";  // MH3U US v32

const ChaChaApi* api = nullptr;
ChaChaPlugin* self = nullptr;
int g_enabled = 1;  // main thread writes, game threads read (an int: a stale read only delays the switch by one roll)

std::mutex g_rng_m;
std::mt19937 g_rng{std::random_device{}()};

uint16_t step(uint32_t x) { return static_cast<uint16_t>(((x ? x : 1) * kMultiplier) % kModulus); }

uint16_t ld16(uint32_t a) { return chacha_ld16(api->guest_base, a); }
void st16(uint32_t a, uint16_t v) {
    api->guest_base[a] = static_cast<uint8_t>(v >> 8);
    api->guest_base[a + 1] = static_cast<uint8_t>(v);
}

// one generator call in flight per game thread
struct Roll {
    bool active = false;
    uint32_t slot = 0;
    uint16_t saved = 0, start = 0;
};
thread_local Roll t_roll;

uint32_t slot_addr() {
    const uint8_t* b = api->guest_base;
    if (!api->readable(kGamePtr, 4)) return 0;
    const uint32_t game = chacha_ld32(b, kGamePtr);
    return game && api->readable(game + kSlot1, 2) ? game + kSlot1 : 0;
}

void before_talisman(void*, ChaChaCall*) {
    t_roll.active = false;
    if (!g_enabled) return;
    const uint32_t slot = slot_addr();
    if (!slot) return;
    uint16_t start;
    {
        std::lock_guard g(g_rng_m);
        start = static_cast<uint16_t>(std::uniform_int_distribution<uint32_t>(1, kModulus - 1)(g_rng));
    }
    t_roll = {true, slot, ld16(slot), start};
    st16(slot, start);
}

void after_talisman(void*, ChaChaCall*) {
    if (!t_roll.active) return;
    t_roll.active = false;
    // how many draws the generator made: the steps from our start to where slot 1 is now
    const uint16_t now = ld16(t_roll.slot);
    int draws = -1;
    uint16_t x = t_roll.start;
    for (int k = 0; k <= kMaxDraws; ++k, x = step(x))
        if (x == now) {
            draws = k;
            break;
        }
    if (draws < 0) draws = 1;  // not reached (cannot happen with the game's own code): move the save on by one draw
    uint16_t s = t_roll.saved;
    for (int k = 0; k < draws; ++k) s = step(s);
    st16(t_roll.slot, s);
}

void load_settings() {
    const char* v = api->config_get(self, "enabled", "true");
    g_enabled = std::strcmp(v, "false") != 0 && std::strcmp(v, "0") != 0;
}
void save_settings() {
    api->config_set(self, "enabled", g_enabled ? "true" : "false");
    api->config_save(self);
}
void on_save(ChaChaPlugin*) { save_settings(); }
void on_defaults(ChaChaPlugin*) {
    g_enabled = 1;
    save_settings();
}

void on_settings(ChaChaPlugin*) {
    api->ui_group("Settings");
    if (api->ui_switch("Any talisman from any table", &g_enabled,
                       "Each talisman is rolled as if from a random one of the 17 charm tables, with the game's own odds, "
                       "so cursed tables no longer lock talismans out. Your save keeps its own table: switched off, the "
                       "game rolls exactly as it would have.") != 0)
        save_settings();
}

}  // namespace

extern "C" CHACHA_EXPORT int chacha_plugin_init(const ChaChaApi* a, ChaChaPlugin* p) {
    api = a;
    self = p;
    p->name = "Charm Table Fix";
    p->version = "1.0";
    if (a->version < 1 || std::strcmp(a->game_sha1, kGameSha1) != 0) {
        a->log(p, "made for MH3U US v32 - this game's executable differs, staying off");
        return 0;
    }
    load_settings();
    p->on_settings = &on_settings;
    if (chacha_has_save_hooks(a)) p->on_save = &on_save, p->on_defaults = &on_defaults;
    if (!a->hook(p, kMakeTalisman, &before_talisman, &after_talisman, nullptr)) {
        a->log(p, "this Cha-Cha has no hook point for the talisman generator (needs a Cha-Cha build with it)");
        return 0;
    }
    a->log(p, "ready");
    return CHACHA_API_VERSION;
}
