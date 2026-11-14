// True Attack for Cha-Cha (MH3U US v32): menus show a weapon's true attack instead of the "bloated" one.
// A Cha-Cha plugin (include/chacha_plugin.h, docs/modding.md).
//
// How the game does it (docs/research-log.md, 2026-11-04 "True attack"): weapon data holds the true attack; for
// display only, FUN_02045108(item, raw) returns raw x table[weapon class] / 100 (u16), the table at 0x1017C13C being
// u32 x100: 480 140 520 230 148 130 130 330 460 230 120 140 460 170 (index from FUN_02158CC0 by the item's kind;
// kind 0xB with FUN_020450C0 uses the last entry). Every live caller is menu or text code (Smithy, equipment, item
// lists, a sprintf); damage reads the true attack directly. So returning the raw value changes the number shown and
// nothing else. A result of 0 (no weapon class) is left alone.
#include "chacha_plugin.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr uint32_t kDisplayAttack = 0x02045108;
constexpr const char* kGameSha1 = "d97aabfc5a45362f88dfacb09f23c6dcae6faac2";  // MH3U US v32

const ChaChaApi* api = nullptr;
ChaChaPlugin* self = nullptr;
int g_enabled = 1;               // main thread (settings)
std::atomic<bool> g_on{true};    // read by the game threads

std::atomic<int> g_debug{0};  // CHACHA_TRUE_ATTACK_LOG=1: log the first calls (raw -> shown)

void after_display(void*, ChaChaCall* call) {
    if (g_debug.load(std::memory_order_relaxed) > 0) {
        --g_debug;
        char msg[96];
        std::snprintf(msg, sizeof msg, "attack shown: raw %u -> game %u", call->arg_r[1] & 0xFFFF, call->ret_r3);
        api->log(self, msg);
    }
    if (!g_on.load(std::memory_order_relaxed) || call->ret_r3 == 0) return;
    api->set_reg(call->ctx, 3, call->arg_r[1] & 0xFFFF);  // the raw attack, as the game stores it
}

void on_settings(ChaChaPlugin*) {
    api->ui_group("Settings");
    if (api->ui_switch("Show true attack", &g_enabled,
                       "Menus show the weapon's true attack (what the damage formula uses) instead of the bloated number. "
                       "Applies at once; reopen a menu to see it.")) {
        g_on = g_enabled != 0;
        api->config_set(self, "enabled", g_enabled ? "true" : "false");
        api->config_save(self);
    }
}

void on_save(ChaChaPlugin*) {
    api->config_set(self, "enabled", g_enabled ? "true" : "false");
    api->config_save(self);
}
void on_defaults(ChaChaPlugin*) {
    g_enabled = 1;
    g_on = true;
    on_save(nullptr);
}

}  // namespace

extern "C" CHACHA_EXPORT int chacha_plugin_init(const ChaChaApi* a, ChaChaPlugin* p) {
    api = a;
    self = p;
    p->name = "True Attack";
    p->version = "1.0";
    if (a->version < 1 || std::strcmp(a->game_sha1, kGameSha1) != 0) {
        a->log(p, "made for MH3U US v32 - this game's executable differs, staying off");
        return 0;
    }
    const char* en = a->config_get(p, "enabled", "true");
    g_enabled = std::strcmp(en, "false") != 0 && std::strcmp(en, "0") != 0;
    g_on = g_enabled != 0;
    p->on_settings = &on_settings;
    if (chacha_has_save_hooks(a)) p->on_save = &on_save, p->on_defaults = &on_defaults;
    if (const char* d = std::getenv("CHACHA_TRUE_ATTACK_LOG"); d && *d == '1') g_debug = 30;
    if (!a->hook(p, kDisplayAttack, nullptr, &after_display, nullptr)) {
        a->log(p, "this Cha-Cha has no hook point for the attack display (needs Cha-Cha 0.2.2 or later)");
        return 0;
    }
    a->log(p, "ready");
    return CHACHA_API_VERSION;
}
