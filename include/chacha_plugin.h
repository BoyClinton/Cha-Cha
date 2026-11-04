/* Cha-Cha plugin API: native mods loaded from <data folder>/mods/<mod>/plugins/ (docs/modding.md).
 *
 * A plugin is a shared library (.dll on Windows, .so on Linux) that exports
 *
 *     CHACHA_EXPORT int chacha_plugin_init(const ChaChaApi* api, ChaChaPlugin* self);
 *
 * Cha-Cha calls it once at start-up, before the game's code runs. The plugin fills in `self` (its name and
 * callbacks) and returns CHACHA_API_VERSION, or 0 to stay unloaded. Everything the plugin may do goes through `api`:
 * the library links against nothing of Cha-Cha's, so it keeps working across Cha-Cha builds with the same API
 * version (this header only ever adds members at the end of ChaChaApi; size is in api->size).
 *
 * Threads: hooks run on the game's threads (any of its three cores, possibly at the same time); on_hud and
 * on_settings run on Cha-Cha's main thread. Guard data shared between them with a lock.
 *
 * Guest memory is the Wii U's: big-endian, addressed by 32-bit guest addresses. Read it with the helpers below
 * (chacha_ld32 etc.), and only where api->readable() says memory exists - an uncommitted address crashes the game.
 *
 * Plain C, so a plugin can be written in C, C++, Rust, Zig... This file is under the same GPL-3.0 as Cha-Cha.
 */
#ifndef CHACHA_PLUGIN_H
#define CHACHA_PLUGIN_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CHACHA_API_VERSION 1

#if defined(_WIN32)
#define CHACHA_EXPORT __declspec(dllexport)
#else
#define CHACHA_EXPORT __attribute__((visibility("default")))
#endif

/* ---- guest memory: big-endian loads and stores at base + address ------------------------------------------- */
static inline uint32_t chacha_ld32(const uint8_t* base, uint32_t a) {
    const uint8_t* p = base + a;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline uint16_t chacha_ld16(const uint8_t* base, uint32_t a) {
    const uint8_t* p = base + a;
    return (uint16_t)((p[0] << 8) | p[1]);
}
static inline uint8_t chacha_ld8(const uint8_t* base, uint32_t a) { return base[a]; }
static inline float chacha_ldf(const uint8_t* base, uint32_t a) {
    const uint32_t v = chacha_ld32(base, a);
    float f;
    memcpy(&f, &v, 4);
    return f;
}
static inline void chacha_st32(uint8_t* base, uint32_t a, uint32_t v) {
    uint8_t* p = base + a;
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

/* ---- hooks ------------------------------------------------------------------------------------------------ */
/* What a hook sees of one call of a game function. `before` runs first: it reads the arguments in arg_r / arg_f
 * (copies: change an argument with api->set_reg(call->ctx, 3..10, v)) and may set skip = 1 to not run the game's
 * function (then set r3 / f1 as its result with set_reg / set_freg). `after` runs
 * once the function returned: arg_r / arg_f still hold the arguments as they were when the call began, ret_r3 /
 * ret_f1 the results. Several plugins can hook the same function; they run in load order. */
typedef struct ChaChaCall {
    uint32_t addr;     /* the hooked function's guest address */
    uint32_t arg_r[8]; /* r3..r10 on entry */
    double arg_f[8];   /* f1..f8 on entry */
    uint32_t ret_r3;   /* after: r3 on return */
    double ret_f1;     /* after: f1 on return */
    int skip;          /* before: 1 = do not call the game's function */
    void* ctx;         /* the guest register context, for api->get_reg / set_reg */
} ChaChaCall;

typedef void (*ChaChaHookFn)(void* user, ChaChaCall* call);

/* ---- the camera the game draws the world with (updated every game frame) --------------------------------- */
typedef struct ChaChaCamera {
    float pos[3], target[3], up[3];
    float fov_y_degrees, aspect, near_plane, far_plane;
    uint32_t guest_addr; /* the uGameCamera */
    uint32_t frame;      /* counts up with every camera update */
} ChaChaCamera;

/* text alignment for draw_text */
enum { CHACHA_ALIGN_LEFT = 0, CHACHA_ALIGN_CENTER = 1, CHACHA_ALIGN_RIGHT = 2 };

/* Text drawn by draw_text. size is in pixels of the window; outline 0 for none; shear slants the glyphs (0.2 =
 * italic-like); colours are 0xRRGGBBAA. */
typedef struct ChaChaText {
    float x, y, size;
    uint32_t color, outline_color;
    float outline;
    float shear;
    int align;      /* CHACHA_ALIGN_* around x; y is the text's vertical middle */
    void* font;     /* from load_font, or NULL for Cha-Cha's HUD font */
} ChaChaText;

typedef struct ChaChaPlugin ChaChaPlugin;

typedef struct ChaChaApi {
    uint32_t version; /* CHACHA_API_VERSION of the host */
    uint32_t size;    /* sizeof(ChaChaApi) of the host: members past it are missing */
    uint8_t* guest_base;
    const char* data_dir;   /* Cha-Cha's data folder (UTF-8) */
    const char* game_sha1;  /* SHA-1 of the game's RPX: check it before using addresses */

    /* logging (Cha-Cha's console / log), and a toast over the game */
    void (*log)(ChaChaPlugin* self, const char* message);
    void (*toast)(ChaChaPlugin* self, const char* message);

    /* guest memory: 1 if [addr, addr + len) exists (is committed) */
    int (*readable)(uint32_t addr, uint32_t len);

    /* Hooks a game function. Only functions Cha-Cha has a hook point for can be hooked (config/overrides.txt in
     * Cha-Cha's source); returns 1 on success, 0 for any other address. Call from chacha_plugin_init. */
    int (*hook)(ChaChaPlugin* self, uint32_t guest_addr, ChaChaHookFn before, ChaChaHookFn after, void* user);
    /* guest registers of a hooked call's context: n = 0..31 for r0..r31 / f0..f31 */
    uint32_t (*get_reg)(void* ctx, int n);
    void (*set_reg)(void* ctx, int n, uint32_t value);
    double (*get_freg)(void* ctx, int n);
    void (*set_freg)(void* ctx, int n, double value);

    /* camera: 1 when the game has a live camera (out filled) */
    int (*camera)(ChaChaCamera* out);
    /* World position to window pixels through the live camera; 0 when behind the camera or no camera.
     * depth (may be NULL) gets the distance along the view direction. */
    int (*world_to_screen)(const float world[3], float* sx, float* sy, float* depth);
    /* the game picture's rectangle in the window (it keeps 16:9, centred), in pixels */
    void (*game_rect)(float* x, float* y, float* w, float* h);
    /* seconds since start, for animations (main thread) */
    double (*time)(void);

    /* drawing: only inside on_hud (main thread) */
    void (*draw_text)(const ChaChaText* text, const char* utf8);
    void (*text_size)(void* font, float size, const char* utf8, float* w, float* h);
    void (*draw_line)(float x0, float y0, float x1, float y1, uint32_t color, float thickness);
    void (*draw_rect)(float x0, float y0, float x1, float y1, uint32_t color, float rounding);
    void (*draw_circle)(float x, float y, float radius, uint32_t color);
    void (*draw_poly)(const float* xy, int points, uint32_t color); /* a filled convex polygon */
    /* A TrueType / OpenType font file (absolute, or relative to the mod's folder) for draw_text; NULL if it can't
     * be read. Call from chacha_plugin_init: fonts are built once, before the first frame. */
    void* (*load_font)(ChaChaPlugin* self, const char* path);

    /* settings rows in MODS -> <mod>: only inside on_settings. Each returns 1 when the user changed the value. */
    void (*ui_group)(const char* title);
    int (*ui_switch)(const char* label, int* on, const char* description);
    int (*ui_choice)(const char* label, int* index, const char* const* items, int count, const char* description);
    int (*ui_float)(const char* label, float* value, float min, float max, float step, const char* format,
                    const char* description);
    int (*ui_button)(const char* label, const char* value, const char* description); /* 1 when pressed */
    void (*ui_info)(const char* label, const char* value, const char* description);

    /* settings file: <mod folder>/settings.ini, key = value lines (kept between sessions) */
    const char* (*config_get)(ChaChaPlugin* self, const char* key, const char* fallback);
    void (*config_set)(ChaChaPlugin* self, const char* key, const char* value);
    int (*config_save)(ChaChaPlugin* self); /* 1 when written */
    const char* (*mod_dir)(ChaChaPlugin* self); /* the mod's folder (UTF-8) */
    /* Cha-Cha 0.2.2+: sizeof(ChaChaPlugin) of the host. A plugin sets ChaChaPlugin members past `host` (on_save,
     * on_defaults) only when the host has them: chacha_has_save_hooks(api). */
    uint32_t plugin_size;
} ChaChaApi;

struct ChaChaPlugin {
    /* filled by the plugin in chacha_plugin_init */
    const char* name;    /* shown in MODS */
    const char* version;
    void* user;
    void (*on_hud)(ChaChaPlugin* self);      /* every drawn frame while the game runs: draw here */
    void (*on_settings)(ChaChaPlugin* self); /* the plugin's rows in MODS -> <mod> */
    void (*on_shutdown)(ChaChaPlugin* self);
    void* host; /* Cha-Cha's own, do not touch */
    /* optional (Cha-Cha 0.2.2+; NULL = none): the Mods page ends with one "Save settings" and one "Restore defaults"
     * row for all mods, which call these. on_save keeps the current settings (config_save); on_defaults puts them back
     * to the plugin's defaults. Main thread. */
    void (*on_save)(ChaChaPlugin* self);
    void (*on_defaults)(ChaChaPlugin* self);
};

typedef int (*ChaChaPluginInit)(const ChaChaApi* api, ChaChaPlugin* self);

/* 1 when this Cha-Cha has ChaChaPlugin::on_save / on_defaults (an older one's struct ends at `host`) */
static inline int chacha_has_save_hooks(const ChaChaApi* api) {
    return api->size >= offsetof(ChaChaApi, plugin_size) + sizeof(uint32_t) &&
           api->plugin_size >= offsetof(ChaChaPlugin, on_defaults) + sizeof(void*);
}

#ifdef __cplusplus
}
#endif

#endif /* CHACHA_PLUGIN_H */
