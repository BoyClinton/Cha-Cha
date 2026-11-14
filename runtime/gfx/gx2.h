// GX2 HLE registration (runtime/gfx/gx2.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace mh3u::os {
struct Runtime;
}

namespace mh3u::gfx {
void register_gx2(os::Runtime& r);
// Builds the pipelines earlier runs used (progress: done, total); before the game starts, on any thread.
void precompile_pipelines(const std::function<void(size_t, size_t)>& progress);
// Our effects, switched live (from the next frame): the Tri look, ambient occlusion, enhanced depth of field, vignette,
// global illumination and screen-space reflections (levels: 0 off, 1 Lite, 2 Full).
void set_effects(bool tri_look, bool ambient_occlusion, bool depth_of_field, bool vignette, int global_illumination, int reflections);
// Widescreen: the TV picture `factor` times wider than 16:9 (1 = 16:9, 1.3125 = 21:9, 2 = 32:9), HUD kept 16:9 in the
// middle; any thread, from the next frame. The game's camera follows through src/game/widescreen.cpp.
void set_widescreen(float factor);
// Texture replacement packs (<data>/texture_replacements) on/off, live.
void set_texture_packs(bool on);
// Texture dumping into <data>/texture_dump (files named for texture packs), live; files written so far.
void set_texture_dump(bool on);
uint64_t texture_dumps();
// Writes build/frame_<n>.bmp (+ _draws.txt) of a frame soon after this call, as MH3U_DUMP_FRAME does; any thread.
void dump_next_frame();
// The same for `count` frames, `step` frames apart (a burst, for flicker that comes and goes).
void dump_frames(int count, int step);
// GamePad screen on the PC window: 0 hidden (default), 1 picture-in-picture, 2 GamePad only. Initial value from
// MH3U_GAMEPAD_VIEW (off / pip / only); F3 in the window cycles it (F1 opens the settings, F2 the mods).
void cycle_gamepad_view();
// Rendering statistics (draws captured, clears, copies, display-list replay) for the end-of-run summary.
void print_gx2_stats();
// Writes the renderer's pipeline cache (at exit; also every 1800 frames by GX2SwapScanBuffers).
void save_gx2_caches();
}
