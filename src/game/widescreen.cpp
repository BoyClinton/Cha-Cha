// Widescreen (issue #5, after the idea of the ultrawide pack for MH3U on the console emulator, which shows which values
// change; our own code). Hooks inside game functions (config/hooks.txt), status: wip - seen in headless captures
// at 2560x1080 and 3840x1080 in the village and Port Tanzia, not yet in a hunt.
//
// - Camera: each frame sCamera::vf6 (0x02BA0238) sets every camera's aspect (uCamera +0x38) from its viewport's
//   integer rectangle, f11 = (x1 - x0) / (y1 - y0) at 0x02BA0474 (r0 = width, r8 = height), stored next and turned into
//   the projection afterwards. For the full-screen 16:9 viewport (the TV's) the aspect is multiplied by the widescreen
//   factor: the vertical field of view stays, more is seen to the sides. The renderer makes the TV-sized targets that
//   much wider (gfx::set_widescreen), so the game's 1280x720 viewport covers the wider image.
// - Name tags: the tags over party members and Felynes are placed in HUD space as x = fraction x width (fraction 0..1
//   across the screen from the camera's projection, width from the screen size function 0x02617CE0). The renderer
//   squeezes the HUD into the middle 16:9, so the fraction is spread around the centre by the same factor:
//   ((fraction - 0.5) x factor + 0.5) x width, right after each fmuls f1 (0x02618214 / 0x026182BC: f7 x f8;
//   0x026189D4 / 0x02618A7C: f0 x f7).
#include "widescreen.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string_view>

#include "functions.h"

namespace mh3u::game {
namespace {

std::atomic<float> g_factor{1.0f};

float factor() { return g_factor.load(std::memory_order_relaxed); }

PPC_FUNC(camera_aspect) {
    const float k = factor();
    if (k == 1.0f) return;
    const int32_t w = static_cast<int32_t>(ctx.r[0]), h = static_cast<int32_t>(ctx.r[8]);
    // the full-screen 16:9 views only (the TV's 1280x720; 1920x1080 for a 1080p TV mode); smaller views keep theirs
    if (w < 1280 || w * 9 != h * 16) return;
    const double v = ppc::round_single(ctx.f[11].ps0 * k);
    ctx.f[11].ps0 = ctx.f[11].ps1 = v;
}

void tag_x(ppc::Context& ctx, int frac, int width) {
    const float k = factor();
    if (k == 1.0f) return;
    const double v = ppc::round_single(((ctx.f[frac].ps0 - 0.5) * k + 0.5) * ctx.f[width].ps0);
    ctx.f[1].ps0 = ctx.f[1].ps1 = v;
}
PPC_FUNC(tag_x_f7_f8) { tag_x(ctx, 7, 8); }
PPC_FUNC(tag_x_f0_f7) { tag_x(ctx, 0, 7); }

}  // namespace

namespace {
std::atomic<int> g_mode{1};
constexpr const char* kModeNames[] = {"auto", "16:9", "21:9", "32:9"};
}  // namespace

int aspect_mode_from(const char* v) {
    for (int i = 0; i < 4; ++i)
        if (std::string_view(v) == kModeNames[i]) return i;
    return 1;
}
const char* aspect_mode_name(int mode) { return kModeNames[std::clamp(mode, 0, 3)]; }
void set_aspect_mode(int mode) { g_mode = std::clamp(mode, 0, 3); }
int aspect_mode() { return g_mode; }

float aspect_factor(int mode, int width, int height) {
    switch (mode) {
        case 2: return 64.0f / 27.0f / (16.0f / 9.0f);  // "21:9" screens are 64:27 (2560x1080, 3440x1440 is 43:18)
        case 3: return 2.0f;
        case 0: {
            if (width <= 0 || height <= 0) return 1.0f;
            const float k = static_cast<float>(width) / static_cast<float>(height) / (16.0f / 9.0f);
            return k < 1.01f ? 1.0f : std::min(k, 3.0f);
        }
        default: return 1.0f;
    }
}

void set_widescreen(float k) { g_factor.store(std::clamp(std::isfinite(k) ? k : 1.0f, 1.0f, 3.0f)); }
float widescreen() { return factor(); }

void register_widescreen() {
    ppc::set_hook(0x02BA0474, &camera_aspect);
    ppc::set_hook(0x02618214, &tag_x_f7_f8);
    ppc::set_hook(0x026182BC, &tag_x_f7_f8);
    ppc::set_hook(0x026189D4, &tag_x_f0_f7);
    ppc::set_hook(0x02618A7C, &tag_x_f0_f7);
}

}  // namespace mh3u::game
