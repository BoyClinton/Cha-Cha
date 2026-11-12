// Graphics Settings in the game's own menus: the rows (two pages: the screen, the picture) with short help in the
// game's tone, each change saved to config.ini at once and applied on the host main thread.
#include "graphics_menu.h"

#include "filters.h"
#include "gx2.h"
#include "modern_camera.h"
#include "mods.h"
#include "presenter.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace mh3u::boot {
namespace {

const std::vector<std::string> kDisplayModes = {"windowed", "borderless", "fullscreen"};
const std::vector<std::string> kDisplayNames = {"Windowed", "Borderless", "Fullscreen"};
const std::vector<float> kScales = {0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f};
const std::vector<int> kFps = {0, 30, 60, 120, 144, 240, -1};
const std::vector<std::string> kFpsNames = {"Monitor", "30", "60", "120", "144", "240", "Unlimited"};
const std::vector<std::string> kDofModes = {"original", "off", "enhanced"};
const std::vector<std::string> kDofNames = {"Original", "Off", "Enhanced"};
const std::vector<std::string> kOffOn = {"Off", "On"};
const std::vector<std::string> kLevels = {"off", "lite", "full"};  // global illumination, reflections
const std::vector<std::string> kLevelNames = {"Off", "Lite", "Full"};
const std::vector<int> kDistances = {100, 110, 120, 130, 140, 150, 160, 180, 200};  // percent (issue #17)

enum Row {  // seven rows a page at most (settings_page.cpp)
    kDisplayMode, kWindowSize, kResolution, kScaling, kFrameRate, kVSync,  // page 1: the screen
    kLook, kBloom, kDepthOfField, kAmbientOcclusion, kGlobalIllumination, kReflections, kVignette,  // page 2: the picture
    kModernCamera, kCameraDistance,  // page 3: the camera (user 2026-11-10)
    kRowCount,
    kModsFirst = kRowCount  // page 4: the mods (mods.h), one row each
};
constexpr int kModCount = static_cast<int>(game::Mod::Count);

int level(const std::string& s) { return s == "full" ? 2 : s == "lite" ? 1 : 0; }

void apply_effects(const Settings& s) {
    gfx::set_effects(s.look == "tri", s.ambient_occlusion, s.depth_of_field == "enhanced", s.vignette, level(s.global_illumination),
                     level(s.reflections));
}

template <class T>
int index_of(const std::vector<T>& v, const T& x) {
    const auto it = std::find(v.begin(), v.end(), x);
    return it == v.end() ? 0 : static_cast<int>(it - v.begin());
}

// The sizes up to the whole display (not its usable area: on a 2560x1440 screen the taskbar left 2560x1392, so 1440p
// was missing - user 2026-11-09), plus the display's own size when it is not one of them (ultrawide and the like).
std::vector<std::string> window_sizes(gfx::Presenter* presenter) {
    SDL_Rect r{0, 0, 1 << 16, 1 << 16};
    int native_w = 0, native_h = 0;
    if (presenter)
        if (auto* w = static_cast<SDL_Window*>(presenter->window_handle())) {
            const SDL_DisplayID d = SDL_GetDisplayForWindow(w);
            SDL_GetDisplayBounds(d, &r);
            native_w = r.w, native_h = r.h;
        }
    std::vector<std::pair<int, int>> sizes = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3200, 1800}, {3840, 2160}};
    if (native_w > 0 && native_h > 0 && std::find(sizes.begin(), sizes.end(), std::pair{native_w, native_h}) == sizes.end())
        sizes.push_back({native_w, native_h});
    std::sort(sizes.begin(), sizes.end(), [](auto a, auto b) { return a.first * a.second < b.first * b.second; });
    std::vector<std::string> out;
    for (auto [w, h] : sizes)
        if (w <= r.w && h <= r.h) out.push_back(std::to_string(w) + "x" + std::to_string(h));
    if (out.empty()) out.push_back("1280x720");
    return out;
}

std::string scale_name(float s) {
    return std::to_string(static_cast<int>(std::lround(1280 * s))) + "x" + std::to_string(static_cast<int>(std::lround(720 * s)));
}

}  // namespace

void apply_live_settings(gfx::Presenter& presenter, const Settings& s) {
    presenter.set_display_mode(s.display_mode == "fullscreen" ? 2 : s.display_mode == "borderless" ? 1 : 0);
    presenter.set_sharp_scaling(s.sharp_scaling);
    if (int w = 0, h = 0; s.display_mode == "windowed" && sscanf_s(s.window_size.c_str(), "%dx%d", &w, &h) == 2)
        presenter.set_window_size(w, h);
    game::set_filters(s.bloom, s.depth_of_field != "off");
    apply_effects(s);
}

GraphicsMenu::GraphicsMenu(const AppPaths& paths, gfx::Presenter* presenter, Settings settings, std::function<void(const Settings&)> on_saved)
    : paths_(paths), presenter_(presenter), on_saved_(std::move(on_saved)), settings_(std::move(settings)) {}

game::SettingsPage GraphicsMenu::page() {
    game::SettingsPage p;
    p.title = "Graphics Settings";
    p.page_starts = {kDisplayMode, kLook, kModernCamera, kModsFirst};
    p.page_titles = {"Display Settings", "Graphics Settings", "Camera Settings", "Mods"};
    p.rows = [this] { return rows(); };
    p.change = [this](int row, int value) { change(row, value); };
    return p;
}

// One page of the rows [first, end): the same settings, rows renumbered from 0.
game::SettingsPage GraphicsMenu::part(const char* title, int first, int end) {
    game::SettingsPage p;
    p.title = title;
    p.page_starts = {0};
    p.rows = [this, first, end] {
        std::vector<game::SettingRow> all = rows();
        return std::vector<game::SettingRow>(all.begin() + first, all.begin() + std::min(end, static_cast<int>(all.size())));
    };
    p.change = [this, first](int row, int value) { change(first + row, value); };
    return p;
}

game::SettingsPage GraphicsMenu::display_page() { return part("Display Settings", kDisplayMode, kLook); }
game::SettingsPage GraphicsMenu::graphics_page() { return part("Graphics Settings", kLook, kModernCamera); }
game::SettingsPage GraphicsMenu::camera_page() { return part("Camera Settings", kModernCamera, kRowCount); }
game::SettingsPage GraphicsMenu::mods_page() { return part("Mods", kModsFirst, kModsFirst + kModCount); }

std::vector<game::SettingRow> GraphicsMenu::rows() {
    std::lock_guard g(m_);
    const Settings& s = settings_;
    std::vector<std::string> scales;
    int scale = 2;
    for (size_t i = 0; i < kScales.size(); ++i) {
        scales.push_back(scale_name(kScales[i]));
        if (std::abs(kScales[i] - s.render_scale) < 0.01f) scale = static_cast<int>(i);
    }
    const std::vector<std::string> sizes = window_sizes(presenter_);
    std::vector<game::SettingRow> r(kRowCount + kModCount);
    r[kDisplayMode] = {"Display Mode", "Play in a window, borderless at desktop\nresolution, or in exclusive fullscreen.", kDisplayNames,
                       index_of(kDisplayModes, s.display_mode)};
    r[kWindowSize] = {"Window Size", "The size of the window in windowed mode.\nThe picture is scaled to fit it.", sizes,
                      index_of(sizes, s.window_size)};
    r[kResolution] = {"Internal Resolution", "The resolution the game is drawn at.\nTakes effect the next time you start.", scales, scale};
    r[kScaling] = {"Scaling", "How the picture fits the screen: Smooth\nblends pixels, Sharp keeps them crisp.", {"Smooth", "Sharp"},
                   s.sharp_scaling ? 1 : 0};
    r[kFrameRate] = {"Frame Rate Limit", "The most frames per second the game\ndraws (Monitor: its refresh rate). Takes\neffect the next time you start.",
                     kFpsNames, index_of(kFps, s.fps_limit)};
    r[kVSync] = {"VSync", "Waits for the display: no tearing, a\nlittle more delay. Takes effect the next\ntime you start.", kOffOn,
                 s.vsync ? 1 : 0};
    r[kLook] = {"Look", "Original is the game as released. Tri\ngives the world the softer look of\nMonster Hunter Tri.",
                {"Original", "Tri"}, s.look == "tri" ? 1 : 0};
    r[kBloom] = {"Bloom", "The game's glow around bright light.", kOffOn, s.bloom ? 1 : 0};
    r[kDepthOfField] = {"Depth of Field", "Original: the game's own, in cutscenes.\nEnhanced also softens the distance\nwhile you play.",
                        kDofNames, index_of(kDofModes, s.depth_of_field)};
    r[kAmbientOcclusion] = {"Ambient Occlusion", "Soft shadows where surfaces meet.\nNot in the original game.", kOffOn,
                            s.ambient_occlusion ? 1 : 0};
    r[kGlobalIllumination] = {"Global Illumination", "Sunlit ground and walls tint what is\nnext to them. Lite is lighter on the\nGPU. Not in the original game.",
                              kLevelNames, index_of(kLevels, s.global_illumination)};
    r[kReflections] = {"Reflections", "Smooth surfaces mirror what is around\nthem when seen low. Lite is lighter on\nthe GPU. Not in the original game.",
                       kLevelNames, index_of(kLevels, s.reflections)};
    r[kVignette] = {"Vignette", "Gently darker screen corners.\nNot in the original game.", kOffOn, s.vignette ? 1 : 0};
    r[kModernCamera] = {"Modern Camera", "On quests the camera orbits freely: the\nmouse moves it directly, the stick turns\nand tilts at once, and it drifts back\nbehind you as you run. L works as before.",
                        kOffOn, s.modern_camera ? 1 : 0};
    std::vector<std::string> distances;
    for (int d : kDistances) distances.push_back(d == 100 ? "Default" : std::to_string(d) + "%");
    r[kCameraDistance] = {"Camera Distance", "How far the camera sits from your hunter\nout in the field: further back keeps big\nmonsters on screen. Towns keep their own.",
                          distances, index_of(kDistances, s.camera_distance)};
    for (int m = 0; m < kModCount; ++m) {
        const game::ModInfo& info = game::mod_info(static_cast<game::Mod>(m));
        r[static_cast<size_t>(kModsFirst + m)] = {info.name, std::string(info.goal[0]) + "\n" + info.goal[1], kOffOn,
                                                  game::mod_enabled(static_cast<game::Mod>(m)) ? 1 : 0};
    }
    return r;
}

void GraphicsMenu::change(int row, int value) {
    if (row >= kModsFirst && row < kModsFirst + kModCount) {  // a mod: mods.ini, not config.ini
        game::set_mod_enabled(static_cast<game::Mod>(row - kModsFirst), value != 0);
        if (mods_saved_) mods_saved_();
        std::printf("mods: %s %s\n", game::mod_info(static_cast<game::Mod>(row - kModsFirst)).key, value ? "on" : "off");
        return;
    }
    {
        std::lock_guard g(m_);
        Settings& s = settings_;
        const auto at = [&](const auto& v) { return v[static_cast<size_t>(std::clamp(value, 0, static_cast<int>(v.size()) - 1))]; };
        switch (row) {
        case kDisplayMode: s.display_mode = at(kDisplayModes); break;
        case kWindowSize: s.window_size = at(window_sizes(presenter_)); break;
        case kResolution: s.render_scale = at(kScales); break;
        case kScaling: s.sharp_scaling = value != 0; break;
        case kFrameRate: s.fps_limit = at(kFps); break;
        case kVSync: s.vsync = value != 0; break;
        case kLook: s.look = value ? "tri" : "original"; break;
        case kBloom: s.bloom = value != 0; break;
        case kDepthOfField: s.depth_of_field = at(kDofModes); break;
        case kAmbientOcclusion: s.ambient_occlusion = value != 0; break;
        case kGlobalIllumination: s.global_illumination = at(kLevels); break;
        case kReflections: s.reflections = at(kLevels); break;
        case kVignette: s.vignette = value != 0; break;
        case kModernCamera:
            s.modern_camera = value != 0;
            game::set_modern_camera(s.modern_camera);
            break;
        case kCameraDistance:
            s.camera_distance = at(kDistances);
            game::set_camera_distance(static_cast<float>(s.camera_distance) / 100.0f);
            break;
        default: return;
        }
        save_settings(paths_, s);
        pending_ = true;
    }
    std::printf("graphics settings: row %d -> %d\n", row, value);
}

void GraphicsMenu::apply_pending() {
    Settings s;
    {
        std::lock_guard g(m_);
        if (!pending_) return;
        pending_ = false;
        s = settings_;
    }
    if (on_saved_) on_saved_(s);
    if (presenter_) apply_live_settings(*presenter_, s);
    else {
        game::set_filters(s.bloom, s.depth_of_field != "off");
        apply_effects(s);
    }
}

}  // namespace mh3u::boot
