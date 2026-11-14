// Widescreen (issue #5): the camera's aspect widened for 21:9 / 32:9 screens, name tags kept over their characters
// (widescreen.cpp). The render targets and the HUD squeeze are the renderer's (gfx::set_widescreen).
#pragma once

namespace mh3u::game {

void register_widescreen();
// How much wider than 16:9 the picture is (1 = 16:9, 1.333 = 21:9 (64:27), 2 = 32:9); any thread, from the next frame.
void set_widescreen(float factor);
float widescreen();
// The Aspect ratio setting (config.ini aspect_ratio): 0 auto (the window's shape), 1 16:9, 2 21:9, 3 32:9.
int aspect_mode_from(const char* config_value);
const char* aspect_mode_name(int mode);  // the config.ini value
void set_aspect_mode(int mode);
int aspect_mode();
// The widescreen factor for a mode and an output (window) size in pixels: auto follows the window between 16:9 and
// 48:9 (narrower windows keep 16:9 with bars above and below).
float aspect_factor(int mode, int width, int height);

}  // namespace mh3u::game
