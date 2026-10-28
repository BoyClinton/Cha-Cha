// The launcher (setup.h): Dear ImGui (SDL3 events, Vulkan drawing) recorded into the game window's frames by the
// presenter. Mouse, keyboard and controllers all navigate it.
#include "setup.h"

#include "filters.h"
#include "gx2.h"
#include "host_input.h"
#include "presenter.h"
#include "guest_memory.h"
#include "launcher_image.h"
#include "quests.h"
#include "save_reads.h"
#include "translate.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <thread>


#ifndef MH3U_VERSION
#define MH3U_VERSION "dev"
#endif

namespace mh3u::boot {
namespace {

namespace stdfs = std::filesystem;

// Unoptimised SPIR-V: the driver optimises when it builds pipelines anyway, and spirv-opt over the translated
// shaders takes minutes (360 s vs 4 s for all 668 on a 12-thread CPU).
constexpr bool kOptimizeSpirv = false;

stdfs::path from_utf8(const char* s) {
    const std::string str(s);
    return stdfs::path(std::u8string(str.begin(), str.end()));
}

latte::ContentReader content_reader(const fs::OverlayFs& vfs) {
    latte::ContentReader c;
    c.read = [&vfs](const std::string& p) -> std::optional<std::vector<uint8_t>> {
        auto f = vfs.open("/vol/content/" + p);
        if (!f) return std::nullopt;
        std::vector<uint8_t> d(static_cast<size_t>(f->size()));
        if (f->read(0, d.data(), d.size()) != d.size()) return std::nullopt;
        return d;
    };
    c.list_files = [&vfs](const std::string& dir) {
        std::vector<std::string> names;
        for (const auto& e : vfs.list("/vol/content/" + dir))
            if (!e.is_dir) names.push_back(e.name);
        return names;
    };
    return c;
}

// Shader translation on a worker thread (with its own copy of the game handles), polled by the UI.
struct ShaderJob {
    latte::Progress progress;
    std::atomic<bool> finished{false};
    latte::TranslateResult result;
    std::thread thread;
    std::chrono::steady_clock::time_point start;
    fs::GameImage game;

    void run(const fs::GameImage& g, const stdfs::path& out, const std::string& stamp) {
        game = g;
        start = std::chrono::steady_clock::now();
        thread = std::thread([this, out, stamp] {
            const fs::OverlayFs vfs = game.make_fs({});
            result = latte::translate_all(content_reader(vfs), out, stamp, &progress, false, kOptimizeSpirv);
            finished = true;
        });
    }
    bool ok() const { return finished && result.shaders > 0 && result.compiled == result.shaders; }
    double seconds() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); }
    ~ShaderJob() {
        if (thread.joinable()) thread.join();
    }
};

// Results of SDL's native file/folder dialogs (their callbacks may run on another thread).
struct DialogResult {
    std::mutex m;
    std::vector<stdfs::path> picked;
    bool pending = false;
    static void SDLCALL callback(void* self, const char* const* files, int) {
        auto* d = static_cast<DialogResult*>(self);
        std::lock_guard g(d->m);
        d->pending = false;
        for (; files && *files; ++files) d->picked.push_back(from_utf8(*files));
    }
};

#ifndef _WIN32
// Bahnschrift is Windows' own and cannot be shipped: elsewhere the first of these common sans fonts that is installed
stdfs::path windows_font(const char*) {
    std::error_code ec;
    for (const char* f : {"/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf",
                          "/usr/share/fonts/google-noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                          "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
                          "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"})
        if (stdfs::exists(f, ec)) return f;
    return {};
}
#else
stdfs::path windows_font(const char* file) {
    wchar_t* windir = nullptr;
    size_t len = 0;
    stdfs::path p;
    if (_wdupenv_s(&windir, &len, L"WINDIR") == 0 && windir) p = stdfs::path(windir) / "Fonts" / file;
    std::free(windir);
    std::error_code ec;
    return stdfs::exists(p, ec) ? p : stdfs::path{};
}
#endif

struct Fonts {
    ImFont *body = nullptr, *menu = nullptr, *note = nullptr;
};

// System fonts only (nothing bundled): Bahnschrift.
Fonts load_fonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    Fonts f;
    const auto add = [&](const char* file, float px) -> ImFont* {
        const stdfs::path p = windows_font(file);
        ImFont* font = p.empty() ? nullptr : io.Fonts->AddFontFromFileTTF(p.string().c_str(), px * scale);
        if (!font) {
            ImFontConfig cfg;
            cfg.SizePixels = px * scale;
            font = io.Fonts->AddFontDefault(&cfg);
        }
        return font;
    };
    f.body = add("bahnschrift.ttf", 20);
    f.menu = add("bahnschrift.ttf", 30);
    f.note = add("bahnschrift.ttf", 16);
    return f;
}

void style() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0;
    s.WindowBorderSize = 0;
    s.FrameRounding = 4;
    s.FramePadding = ImVec2(14, 8);
    s.ItemSpacing = ImVec2(10, 10);
    s.Colors[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
    s.Colors[ImGuiCol_Button] = ImVec4(0, 0, 0, 0);
    s.Colors[ImGuiCol_ButtonHovered] = ImVec4(1, 1, 1, 0.06f);
    s.Colors[ImGuiCol_ButtonActive] = ImVec4(1, 1, 1, 0.12f);
    s.Colors[ImGuiCol_PlotHistogram] = ImVec4(0.95f, 0.66f, 0.20f, 1);
    s.Colors[ImGuiCol_FrameBg] = ImVec4(1, 1, 1, 0.10f);
    s.Colors[ImGuiCol_NavHighlight] = ImVec4(0, 0, 0, 0);  // the rows draw their own selection
    // a thin rounded scroll bar in the panel's warm tones
    s.ScrollbarSize = 8;
    s.ScrollbarRounding = 4;
    s.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    s.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(1.0f, 0.93f, 0.78f, 0.16f);
    s.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1.0f, 0.93f, 0.78f, 0.28f);
    s.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.93f, 0.70f, 0.33f, 0.9f);
}

// The backdrop: a dusky sky going from near-black on the left to a warm amber glow top-right, darker ground, vignette.
void draw_backdrop(ImDrawList* dl, ImVec2 size, float t) {
    const ImU32 deep = IM_COL32(14, 10, 8, 255), ember = IM_COL32(120, 64, 26, 255), glow = IM_COL32(214, 146, 70, 255);
    const float horizon = size.y * 0.78f;
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(size.x, horizon), deep, glow, ember, deep);
    dl->AddRectFilledMultiColor(ImVec2(0, horizon), size, IM_COL32(40, 24, 14, 255), IM_COL32(70, 42, 24, 255),
                                IM_COL32(24, 14, 9, 255), IM_COL32(10, 7, 5, 255));
    // drifting haze bands
    for (int i = 0; i < 3; ++i) {
        const float y = horizon - size.y * (0.06f + 0.05f * i) + std::sin(t * 0.2f + i) * 6.0f;
        dl->AddRectFilledMultiColor(ImVec2(0, y), ImVec2(size.x, y + size.y * 0.05f), IM_COL32(255, 190, 120, 0),
                                    IM_COL32(255, 190, 120, 22), IM_COL32(255, 190, 120, 0), IM_COL32(255, 190, 120, 0));
    }
    // left vignette behind the menu
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(size.x * 0.55f, size.y), IM_COL32(0, 0, 0, 170), IM_COL32(0, 0, 0, 0),
                                IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 170));
}

// (InvisibleButton is left out of keyboard / controller navigation unless it has ImGuiButtonFlags_EnableNav.)
// One menu entry: plain text, the selected (focused or hovered) one framed by < > arrows. Returns true when chosen.
// Menus are laid out from this x (set per page), so every row starts at the same place.
float g_menu_x = 0;

bool menu_item(const char* label, ImFont* font, float width, bool enabled = true) {
    ImGui::PushFont(font);
    ImGui::BeginDisabled(!enabled);
    ImGui::SetCursorScreenPos(ImVec2(g_menu_x, ImGui::GetCursorScreenPos().y));
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float h = font->FontSize + 14;
    const bool chosen = ImGui::InvisibleButton(label, ImVec2(width, h), ImGuiButtonFlags_EnableNav);
    const bool selected = ImGui::IsItemHovered() || (ImGui::IsItemFocused() && !ImGui::IsAnyItemHovered());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = !enabled ? IM_COL32(255, 255, 255, 70) : selected ? IM_COL32(255, 236, 200, 255) : IM_COL32(230, 220, 205, 210);
    dl->AddText(font, font->FontSize, ImVec2(at.x + 36, at.y + 7), col, label);
    if (selected && enabled) {
        const float cy = at.y + h * 0.5f;
        dl->AddText(font, font->FontSize, ImVec2(at.x, at.y + 7), col, "<");
        dl->AddText(font, font->FontSize, ImVec2(at.x + width - 16, at.y + 7), col, ">");
        dl->AddLine(ImVec2(at.x + 36, cy + font->FontSize * 0.62f), ImVec2(at.x + 36 + ImGui::CalcTextSize(label).x, cy + font->FontSize * 0.62f),
                    IM_COL32(240, 170, 70, 200), 2.0f);
    }
    ImGui::EndDisabled();
    ImGui::PopFont();
    return chosen && enabled;
}

void headless_shaders(ShaderJob& job, const fs::GameImage& game, const stdfs::path& out, const std::string& stamp) {
    std::printf("translating the game's shaders into %s\n", out.string().c_str());
    job.run(game, out, stamp);
    for (int last = -1; !job.finished;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const int done = job.progress.done, total = job.progress.total;
        if (total && done / 100 != last) {
            last = done / 100;
            std::printf("  shaders %d/%d\n", done, total);
        }
    }
}

// internal resolution: a multiple of the game's native 1280x720
const std::vector<float> kScales = {0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f};
const std::vector<std::string> kScaleNames = {"0.5x", "0.75x", "1x (native)", "1.5x", "2x", "3x", "4x"};
const std::vector<std::string> kDisplayModes = {"windowed", "borderless", "fullscreen"};
const std::vector<std::string> kDisplayNames = {"Windowed", "Borderless", "Fullscreen"};

// A frame's draw lists, copied (the overlay is recorded on whichever thread presents, while the host main thread
// builds the next frame).
struct DrawSnapshot {
    ImDrawData data;
    ~DrawSnapshot() {
        for (ImDrawList* l : data.CmdLists) IM_DELETE(l);
    }
};
std::unique_ptr<DrawSnapshot> snapshot(const ImDrawData& src) {
    auto s = std::make_unique<DrawSnapshot>();
    s->data = src;
    s->data.CmdLists.resize(0);
    for (ImDrawList* l : src.CmdLists) s->data.CmdLists.push_back(l->CloneOutput());
    return s;
}

}  // namespace

SetupResult run_setup(const AppPaths& paths, std::optional<fs::GameImage> game, std::string error) {
    SetupResult res;
    res.settings = load_settings(paths);
    res.shader_dir = paths.cache() / "shaders";
    if (!game) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return res;
    }
    const std::string stamp = latte::cache_stamp(game->rpx_sha1);
    if (!latte::cache_is_current(res.shader_dir, stamp)) {
        ShaderJob job;
        headless_shaders(job, *game, res.shader_dir, stamp);
        for (const auto& e : job.result.errors) std::fprintf(stderr, "  %s\n", e.c_str());
        std::printf("shaders: %d/%d ready in %.1f s\n", job.result.compiled, job.result.shaders, job.seconds());
        if (job.result.compiled == 0) return res;
    }
    res.game = std::move(game);
    return res;
}

namespace {

// ---- launcher sounds: short tones made here (nothing shipped), mixed by SDL with the game's own audio ---------------
enum class Sfx { Move, Select, Back, Tab, Apply, Key, Count };

class SoundBank {
public:
    bool open() {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return false;
        const SDL_AudioSpec spec{SDL_AUDIO_F32, 1, kRate};
        stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!stream_) return false;
        // notes: (frequency Hz, length s); each with a quick attack and an exponential fade
        const auto tone = [](std::vector<float>& out, std::initializer_list<std::pair<float, float>> notes, float volume) {
            for (const auto& [f, len] : notes) {
                const int n = static_cast<int>(len * kRate);
                for (int i = 0; i < n; ++i) {
                    const float t = static_cast<float>(i) / kRate;
                    const float env = std::min(1.0f, t * 400.0f) * std::exp(-t * 5.0f / len);
                    // a soft square-ish timbre: fundamental plus a little of the octave
                    out.push_back(volume * env * (std::sin(6.2831853f * f * t) + 0.25f * std::sin(12.566371f * f * t)));
                }
            }
        };
        tone(clips_[static_cast<int>(Sfx::Move)], {{1760.0f, 0.035f}}, 0.10f);
        tone(clips_[static_cast<int>(Sfx::Select)], {{988.0f, 0.05f}, {1480.0f, 0.09f}}, 0.16f);
        tone(clips_[static_cast<int>(Sfx::Back)], {{740.0f, 0.05f}, {494.0f, 0.09f}}, 0.16f);
        tone(clips_[static_cast<int>(Sfx::Tab)], {{1319.0f, 0.04f}}, 0.12f);
        tone(clips_[static_cast<int>(Sfx::Apply)], {{784.0f, 0.06f}, {988.0f, 0.06f}, {1568.0f, 0.14f}}, 0.16f);
        tone(clips_[static_cast<int>(Sfx::Key)], {{2349.0f, 0.025f}}, 0.08f);
        SDL_ResumeAudioStreamDevice(stream_);
        return true;
    }
    void close() {
        if (stream_) SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
    void play(Sfx s) {
        if (!stream_) return;
        SDL_ClearAudioStream(stream_);  // the newest sound replaces one still playing
        const std::vector<float>& c = clips_[static_cast<int>(s)];
        SDL_PutAudioStreamData(stream_, c.data(), static_cast<int>(c.size() * sizeof(float)));
    }

private:
    static constexpr int kRate = 48000;
    SDL_AudioStream* stream_ = nullptr;
    std::vector<float> clips_[static_cast<int>(Sfx::Count)];
};
SoundBank g_sound;

// ---- the settings panel: grouped rows on rounded cards (after GNOME's Adwaita), in Cha-Cha's warm browns + amber ---
const ImU32 kPanelBg = IM_COL32(24, 21, 16, 255), kPanelEdge = IM_COL32(255, 236, 200, 24), kAmber = IM_COL32(236, 178, 84, 255);
const ImU32 kText = IM_COL32(236, 228, 214, 255), kDim = IM_COL32(170, 160, 145, 255), kSoft = IM_COL32(208, 198, 183, 255);
const ImU32 kCard = IM_COL32(39, 34, 27, 255), kHairline = IM_COL32(255, 236, 200, 20), kHover = IM_COL32(255, 236, 200, 14);
const ImU32 kFill = IM_COL32(255, 236, 200, 26), kOnAmber = IM_COL32(32, 24, 14, 255), kOff = IM_COL32(255, 255, 255, 90);

struct PanelLayout {
    ImFont *label = nullptr, *value = nullptr, *small = nullptr;
    float x = 0, w = 0, row_h = 0, scale = 1, indent = 0;
    const char *sel_title = nullptr, *sel_desc = nullptr;  // the selected row, for the description pane
    stdfs::path sel_image;                                  // and its picture (MODS -> QUESTS)
    bool sel_changes = false;                               // left / right change the selected row (control guide)
    // the open group: its card is drawn under its rows when it closes (draw-list channel 0), with the hairlines
    // between rows and the selected row's highlight, rounded where it meets the card's corners
    bool in_group = false, focus_ring = false;
    float group_top = 0;
    std::vector<float> seps;
    ImVec2 hi0, hi1;
    ImU32 hi_col = 0;
};

// The controller / keyboard cursor is showing (not only the mouse): the focused row gets an amber ring.
bool nav_cursor_visible() { return ImGui::GetCurrentContext()->NavCursorVisible; }

void group_begin(PanelLayout& p, const char* title) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::SetCursorScreenPos(ImVec2(p.x, ImGui::GetCursorScreenPos().y));
    if (title) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        dl->AddText(p.small, p.small->FontSize, ImVec2(at.x + 16 * p.scale, at.y), kDim, title);  // on the labels' line
        ImGui::Dummy(ImVec2(p.w, p.small->FontSize + 8 * p.scale));
    }
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    // rows touch: ImGui's item spacing would leave a gap under each row (the highlight short of the hairline, dead
    // space at the card's bottom)
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0));
    p.in_group = true;
    p.focus_ring = false;
    p.group_top = ImGui::GetCursorScreenPos().y;
    p.seps.clear();
    p.hi_col = 0;
}

void group_end(PanelLayout& p) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float bottom = ImGui::GetCursorScreenPos().y, r = 12 * p.scale;
    dl->ChannelsSetCurrent(0);
    dl->AddRectFilled(ImVec2(p.x, p.group_top), ImVec2(p.x + p.w, bottom), kCard, r);
    if (p.hi_col) {
        ImDrawFlags f = 0;
        if (p.hi0.y <= p.group_top + 0.5f) f |= ImDrawFlags_RoundCornersTop;
        if (p.hi1.y >= bottom - 0.5f) f |= ImDrawFlags_RoundCornersBottom;
        // a row between others: square fill, but the ring keeps small round corners
        const float ring_r = f ? r - 1 : 6 * p.scale;
        const ImDrawFlags ring_f = f ? f : 0;
        if (!f) f = ImDrawFlags_RoundCornersNone;
        dl->AddRectFilled(p.hi0, p.hi1, p.hi_col, r, f);
        if (p.focus_ring)
            dl->AddRect(ImVec2(p.hi0.x + 1, p.hi0.y + 1), ImVec2(p.hi1.x - 1, p.hi1.y - 1), kAmber, ring_r, ring_f, 2.0f * p.scale);
    }
    for (float y : p.seps)
        if (y > p.group_top + 1 && y < bottom - 1) dl->AddLine(ImVec2(p.x + 16 * p.scale, y), ImVec2(p.x + p.w - 16 * p.scale, y), kHairline);
    dl->ChannelsMerge();
    ImGui::PopStyleVar();
    p.in_group = false;
    ImGui::Dummy(ImVec2(1, 24 * p.scale));  // three times the room above the next group's title as below it
}

// a small filled triangle pointing left (dir -1) or right (+1), centred on c
void arrow(ImDrawList* dl, ImVec2 c, float s, int dir, ImU32 col) {
    dl->AddTriangleFilled(ImVec2(c.x + dir * s, c.y), ImVec2(c.x - dir * s * 0.6f, c.y - s), ImVec2(c.x - dir * s * 0.6f, c.y + s), col);
}

// Value: the value cycles (left / right, or a click on the row's left / right half); Switch: on / off; Button: an
// action (a chevron); Info: shown only.
enum class Row { Value, Switch, Button, Info };

// One row of the open group. Returns what the user asked for: -1 / +1 (left / right: the d-pad, the stick, the arrow
// keys), 2 (activated: A / Enter / a click; a click on a Value row's left half is -1), 0. The focused row owns left /
// right, so ImGui's navigation does not take them (it would jump to APPLY once the value changed).
int panel_row(PanelLayout& p, Row kind, const char* label, const std::string& value, const char* desc, bool enabled = true, bool on = false) {
    ImGui::PushID(label);
    const ImVec2 at(p.x, ImGui::GetCursorScreenPos().y);
    if (p.in_group && at.y > p.group_top + 0.5f) p.seps.push_back(at.y);
    ImGui::SetCursorScreenPos(at);
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton("row", ImVec2(p.w, p.row_h), ImGuiButtonFlags_EnableNav);
    ImGui::EndDisabled();
    const ImGuiID id = ImGui::GetItemID();
    const bool focused = ImGui::IsItemFocused(), hovered = ImGui::IsItemHovered();
    const bool sel = hovered || (focused && !ImGui::IsAnyItemHovered());
    const bool changes = enabled && (kind == Row::Value || kind == Row::Switch);
    if (sel) {
        p.sel_title = label;
        p.sel_desc = desc;
        p.sel_changes = changes;
    }
    int step = 0;
    if (changes && focused) {
        static const ImGuiKey kLeft[] = {ImGuiKey_LeftArrow, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft};
        static const ImGuiKey kRight[] = {ImGuiKey_RightArrow, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight};
        for (ImGuiKey k : kLeft) {
            ImGui::SetKeyOwner(k, id);
            if (ImGui::IsKeyPressed(k, ImGuiInputFlags_Repeat, id)) step = -1;
        }
        for (ImGuiKey k : kRight) {
            ImGui::SetKeyOwner(k, id);
            if (ImGui::IsKeyPressed(k, ImGuiInputFlags_Repeat, id)) step = 1;
        }
    }
    if (enabled && clicked && kind != Row::Info)
        step = kind == Row::Value && ImGui::IsMouseReleased(ImGuiMouseButton_Left) ? (ImGui::GetIO().MousePos.x < at.x + p.w * 0.5f ? -1 : 1) : 2;
    const ImVec2 b(at.x + p.w, at.y + p.row_h);
    if (sel && p.in_group) {
        p.hi0 = at;
        p.hi1 = b;
        p.hi_col = kHover;
        p.focus_ring = focused && enabled && nav_cursor_visible();
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float s = p.scale, cy = at.y + p.row_h * 0.5f, right = b.x - 16 * s;
    float label_end = right;
    if (kind == Row::Switch) {  // a track with a knob that slides (150 ms) to the new state
        const float tw = 46 * s, th = 26 * s, x0 = right - tw;
        float& t = *ImGui::GetStateStorage()->GetFloatRef(id, on ? 1.0f : 0.0f);
        t += ((on ? 1.0f : 0.0f) - t) * std::min(1.0f, ImGui::GetIO().DeltaTime * 20.0f);
        const ImVec4 off = ImGui::ColorConvertU32ToFloat4(IM_COL32(255, 236, 200, 40)), amber = ImGui::ColorConvertU32ToFloat4(kAmber);
        ImVec4 track(off.x + (amber.x - off.x) * t, off.y + (amber.y - off.y) * t, off.z + (amber.z - off.z) * t, off.w + (amber.w - off.w) * t);
        if (!enabled) track.w *= 0.4f;
        dl->AddRectFilled(ImVec2(x0, cy - th * 0.5f), ImVec2(right, cy + th * 0.5f), ImGui::ColorConvertFloat4ToU32(track), th * 0.5f);
        const float kr = th * 0.5f - 3 * s, kx = x0 + th * 0.5f + (tw - th) * t;
        dl->AddCircleFilled(ImVec2(kx, cy + 1.5f * s), kr, IM_COL32(0, 0, 0, 60));  // its shadow
        dl->AddCircleFilled(ImVec2(kx, cy), kr, enabled ? IM_COL32(250, 244, 232, 255) : IM_COL32(150, 144, 135, 255));
        label_end = x0 - 16 * s;
    } else if (kind == Row::Button) {  // a chevron: this row does something
        const ImU32 col = enabled ? (sel ? kText : kDim) : kOff;
        const float h = 6 * s;
        dl->AddPolyline(std::array{ImVec2(right - h, cy - h * 1.2f), ImVec2(right, cy), ImVec2(right - h, cy + h * 1.2f)}.data(), 3, col, 0, 2.0f * s);
        if (!value.empty()) {
            const ImVec2 vs = p.value->CalcTextSizeA(p.value->FontSize, FLT_MAX, 0, value.c_str());
            dl->AddText(p.value, p.value->FontSize, ImVec2(right - 16 * s - vs.x, cy - vs.y * 0.5f), enabled ? kSoft : kOff, value.c_str());
            label_end = right - 32 * s - vs.x;
        } else {
            label_end = right - 24 * s;
        }
    } else if (!value.empty()) {  // Value: < value >, the arrows lit while selected; Info: the value alone
        const ImVec2 vs = p.value->CalcTextSizeA(p.value->FontSize, FLT_MAX, 0, value.c_str());
        const bool arrows = kind == Row::Value;
        const float vx = right - (arrows ? 20 * s : 0) - vs.x;
        dl->AddText(p.value, p.value->FontSize, ImVec2(vx, cy - vs.y * 0.5f), !enabled ? kOff : sel && arrows ? kText : kSoft, value.c_str());
        if (arrows) {
            const ImU32 col = !enabled ? IM_COL32(255, 255, 255, 40) : sel ? kAmber : IM_COL32(170, 160, 145, 150);
            arrow(dl, ImVec2(vx - 14 * s, cy), 5 * s, -1, col);
            arrow(dl, ImVec2(right - 4 * s, cy), 5 * s, 1, col);
        }
        label_end = vx - (arrows ? 28 * s : 16 * s);
    }
    dl->PushClipRect(at, ImVec2(std::max(at.x, label_end), b.y), true);
    dl->AddText(p.label, p.label->FontSize, ImVec2(at.x + 16 * s + p.indent, cy - p.label->FontSize * 0.5f), enabled ? kText : kOff, label);
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(at.x, b.y));
    ImGui::Dummy(ImVec2(1, 0));
    ImGui::PopID();
    return step;
}

int value_row(PanelLayout& p, const char* label, const std::string& value, const char* desc, bool enabled = true) {
    return panel_row(p, Row::Value, label, value, desc, enabled);
}
void info_row(PanelLayout& p, const char* label, const std::string& value, const char* desc) { panel_row(p, Row::Info, label, value, desc); }
bool button_row(PanelLayout& p, const char* label, const char* desc, bool enabled = true, const std::string& value = {}) {
    return panel_row(p, Row::Button, label, value, desc, enabled) == 2;
}
// on / off: A or a click toggles, left / right set off / on; true if it changed
bool switch_row(PanelLayout& p, const char* label, bool& on, const char* desc, bool enabled = true) {
    const int step = panel_row(p, Row::Switch, label, {}, desc, enabled, on);
    const bool want = step == 2 ? !on : step == 1 ? true : step == -1 ? false : on;
    if (want == on) return false;
    g_sound.play(Sfx::Move);
    on = want;
    return true;
}

// cycles index through n values by step (-1 / +1, 2 = activated: next); true if it changed
bool cycle(int step, int& index, int n) {
    if (!step) return false;
    g_sound.play(Sfx::Move);
    index = (index + (step < 0 ? -1 : 1) + n) % n;
    return true;
}

std::string short_path(const stdfs::path& p, size_t max = 34) {
    std::string s = p.string();
    return s.size() <= max ? s : "..." + s.substr(s.size() - max + 3);
}

}  // namespace

struct Launcher::Impl {
    const AppPaths& paths;
    gfx::Presenter& presenter;
    SDL_Window* window = nullptr;
    std::vector<stdfs::path> sources;
    std::optional<fs::GameImage> game;
    std::string error;
    Settings settings, boot_settings;
    std::vector<stdfs::path> boot_sources;
    stdfs::path shader_dir;
    // Title: the launcher over the game's own intro / title screen (menu or a panel); Hidden: the game is played;
    // InGame: the settings panel over the game (F1 / Home)
    enum class Mode { Title, Hidden, InGame } mode = Mode::Title;
    enum class Panel { None, Settings, Mods } panel = Panel::None;
    enum class Tab { General, Video, Input } tab = Tab::General;
    enum class ModsTab { Quests } mods_tab = ModsTab::Quests;  // item sets, HP scaling, hold to carve: always on (no tab)
    // MODS -> QUESTS: the library (<data>/quests), pack modes (<data>/mods.ini) and what the save has installed; re-read
    // (and packs applied) at start and whenever the panel opens or changes something
    quests::Library quest_library;
    std::vector<quests::Quest> quests_installed;
    std::map<std::string, quests::PackMode> pack_modes;
    bool quests_stale = true, stale_in_game = false;
    stdfs::path quest_dir() const;
    stdfs::path save_dir() const;
    quests::PackMode pack_mode(const quests::Pack& p) const;
    void load_pack_modes();
    void save_pack_modes() const;
    int sync_live(const quests::SlotChange& c);
    std::map<std::string, std::vector<uint32_t>> live_copies;  // per quest file: the game's copies (first slot)
    std::map<std::string, size_t> live_missed;                 // reads of the file when a search found no copy
    bool change_quest(const quests::Quest& q, bool remove, std::string& err);
    void refresh_quests();
    // quest / pack pictures, uploaded when first shown
    VkSampler image_sampler = VK_NULL_HANDLE;
    std::map<stdfs::path, LauncherImage> images;
    std::set<stdfs::path> bad_images;
    const LauncherImage* image(const stdfs::path& file);
    void draw_tab_quests(PanelLayout& p);
    bool show_menu = true, booted = false, translating = false, refocus = true;
    ShaderJob job;
    DialogResult dialog;
    // the settings panel edits a copy; APPLY makes it the settings (closing without it drops the changes)
    Settings draft;
    bool draft_open = false;
    std::string scale_desc;  // the internal resolution row's description (depends on the value)
    // window sizes offered in windowed mode: common 16:9 sizes that fit the screen the window is on
    std::vector<std::string> window_sizes() const {
        SDL_Rect r{0, 0, 1 << 16, 1 << 16};
        SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(window), &r);
        std::vector<std::string> out;
        for (auto [w, h] : {std::pair{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3200, 1800}, {3840, 2160}})
            if (w <= r.w && h <= r.h) out.push_back(std::to_string(w) + "x" + std::to_string(h));
        if (out.empty()) out.push_back("1280x720");
        return out;
    }
    void apply() {
        g_sound.play(Sfx::Apply);
        settings = draft;
        save_settings(paths, settings);
        apply_live();
    }
    float scale = 1.0f;
    Fonts fonts;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;  // of the swapchain, for the overlay pipeline
    std::mutex snap_m;
    std::unique_ptr<DrawSnapshot> snap;  // null: nothing to draw
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now(), last;
    // INPUT tab: the binding waiting for a press (Esc cancels, Delete clears), and since when
    bool capturing = false, capture_pad = false;
    std::string capture_target;
    std::chrono::steady_clock::time_point capture_since;
    // the on-screen keyboard is up (the game asked for text), and its shift state
    bool keyboard = false, kb_shift = false;
    ImGuiID last_selected = 0;  // for the move sound
    // toasts over anything, the game included (top right; the game keeps its input)
    std::string toast_text;
    std::chrono::steady_clock::time_point toast_until{};
    bool toast_drawn = false;
    void toast(std::string text) {
        toast_text = std::move(text);
        toast_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(3500);
    }
    bool toasting() const { return std::chrono::steady_clock::now() < toast_until; }
    void draw_toast(ImVec2 size);
    std::mutex posted_m;  // post_toast (game thread) -> frame()
    std::vector<std::string> posted;
    // DUMP TEXTURES (this session only, not saved): files written since it was switched on
    bool dumping = false;
    uint64_t dump_base = 0, dumps_shown = 0;
    std::string dump_folder() const {
        const auto u = (paths.data / "texture_dump").u8string();
        return std::string(u.begin(), u.end());
    }

    Impl(const AppPaths& p, gfx::Presenter& pr) : paths(p), presenter(pr) {}
    std::string stamp() const { return latte::cache_stamp(game->rpx_sha1); }
    void start_shaders() {
        if (game && !translating && !latte::cache_is_current(shader_dir, stamp())) {
            job.run(*game, shader_dir, stamp());
            translating = true;
        }
    }
    void record(VkCommandBuffer cmd) {
        std::lock_guard g(snap_m);
        if (snap) ImGui_ImplVulkan_RenderDrawData(&snap->data, cmd);
    }
    // settings that take effect while the game runs
    void apply_live() {
        presenter.set_display_mode(settings.display_mode == "fullscreen" ? 2 : settings.display_mode == "borderless" ? 1 : 0);
        if (int w = 0, h = 0; settings.display_mode == "windowed" && sscanf_s(settings.window_size.c_str(), "%dx%d", &w, &h) == 2)
            presenter.set_window_size(w, h);
        game::set_filters(settings.bloom, settings.depth_of_field != "off");
        gfx::set_effects(settings.look == "tri", settings.ambient_occlusion, settings.depth_of_field == "enhanced", settings.vignette,
                         settings.global_illumination);
        gfx::set_texture_packs(settings.texture_packs);
    }
    void hide() {
        save_settings(paths, settings);
        mode = Mode::Hidden;
        panel = Panel::None;
        capturing = draft_open = false;
        std::lock_guard g(snap_m);
        snap.reset();
    }
    void close_panel() {  // the panel's close button / Esc / B
        g_sound.play(Sfx::Back);
        save_settings(paths, settings);
        capturing = draft_open = false;
        if (mode == Mode::InGame) {
            hide();
            return;
        }
        panel = Panel::None;
        refocus = true;
    }
    // title-overlay layout, bottom up so nothing overlaps at any window size: the status block (toast, status, detail)
    // in the bottom-left corner, the menu right above it, CHA-CHA PRESENTS above the menu
    struct Layout {
        float left = 0, toast_y = 0, status_y = 0, detail_y = 0, menu_max_y = 0;
    } lay;
    void layout(ImVec2 size) {
        const float body = fonts.body->FontSize, note = fonts.note->FontSize, menu_item_h = fonts.menu->FontSize + 14 + ImGui::GetStyle().ItemSpacing.y;
        lay.left = std::max(24 * scale, size.x * 0.045f);
        lay.detail_y = size.y - 22 * scale - note;
        lay.status_y = lay.detail_y - 6 * scale - body;
        lay.toast_y = lay.status_y - 14 * scale - 42 * scale;  // the toast's top (it is 42 high)
        lay.menu_max_y = lay.toast_y - 16 * scale - 4 * menu_item_h;
    }
    bool draw(const LaunchState& st);
    void draw_title(const LaunchState& st, ImVec2 size, bool& play, bool& quit);
    void draw_panel(ImVec2 size);
    float draw_guide(ImDrawList* dl, ImVec2 at, float h, bool changes);
    float key_cap(ImDrawList* dl, ImVec2 at, const std::string& key, bool draw);
    void toggle_panel(Panel which);
    void draw_tab_general(PanelLayout& p);
    void draw_tab_video(PanelLayout& p);
    void draw_tab_input(PanelLayout& p, float table_w);
    void draw_status(const LaunchState& st, ImVec2 size);
    void draw_keyboard(ImVec2 size);
};

Launcher::Launcher(const AppPaths& paths, gfx::Presenter& presenter, std::vector<stdfs::path> sources,
                   std::optional<fs::GameImage> game, std::string error, Settings settings, bool show_menu)
    : impl_(std::make_unique<Impl>(paths, presenter)) {
    Impl& d = *impl_;
    d.window = static_cast<SDL_Window*>(presenter.window_handle());
    d.sources = std::move(sources);
    d.game = std::move(game);
    d.error = d.sources.empty() ? std::string() : std::move(error);  // first start: nothing chosen yet is not an error
    d.settings = settings;
    d.show_menu = show_menu;
    d.shader_dir = paths.cache() / "shaders";
    for (size_t i = 0; i < kScales.size(); ++i)
        d.start_shaders();

    d.scale = SDL_GetWindowDisplayScale(d.window) > 0 ? SDL_GetWindowDisplayScale(d.window) : 1.0f;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    style();
    ImGui::GetStyle().ScaleAllSizes(d.scale);
    d.fonts = load_fonts(d.scale);
    ImGui_ImplSDL3_InitForVulkan(d.window);
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, nullptr, 0);  // controllers: handle_event
    const gfx::VulkanContext& vk = presenter.vk();
    const VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64};  // fonts + quest pictures
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pci.maxSets = 64;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    vkCreateDescriptorPool(vk.device, &pci, nullptr, &d.pool);
    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = vk.instance;
    ii.PhysicalDevice = vk.phys;
    ii.Device = vk.device;
    ii.QueueFamily = vk.queue_family;
    ii.Queue = vk.queue;
    ii.DescriptorPool = d.pool;
    ii.MinImageCount = ii.ImageCount = 3;  // vertex buffers cycled over 3 frames; the presenter keeps 2 in flight
    ii.UseDynamicRendering = true;
    d.format = presenter.surface_format();
    ii.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ii.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineRenderingCreateInfo.pColorAttachmentFormats = &d.format;
    ImGui_ImplVulkan_Init(&ii);
    {
        std::lock_guard g(*vk.queue_mutex);  // the font upload submits on the shared queue
        ImGui_ImplVulkan_CreateFontsTexture();
    }
    // MH3U_LAUNCHER_OPEN=settings[:general|video|input] or mods: start on that panel (checking the UI)
    char* open = nullptr;
    size_t open_len = 0;
    if (_dupenv_s(&open, &open_len, "MH3U_LAUNCHER_OPEN") == 0 && open) {
        const std::string o = open;
        d.panel = o.rfind("mods", 0) == 0 ? Impl::Panel::Mods : Impl::Panel::Settings;
        d.tab = o.find("video") != std::string::npos ? Impl::Tab::Video : o.find("input") != std::string::npos ? Impl::Tab::Input : Impl::Tab::General;
    }
    std::free(open);
    if (!g_sound.open()) std::fprintf(stderr, "launcher: no sound (%s)\n", SDL_GetError());
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(vk.device, &sci, nullptr, &d.image_sampler);
    d.refresh_quests();  // quest packs set to All install themselves (the official DLC by default)
    presenter.set_overlay([&d](VkCommandBuffer cmd, VkExtent2D) { d.record(cmd); });
}

Launcher::~Launcher() {
    Impl& d = *impl_;
    d.presenter.set_overlay(nullptr);
    const VkDevice dev = d.presenter.vk().device;
    {
        std::lock_guard g(*d.presenter.vk().queue_mutex);
        vkDeviceWaitIdle(dev);
    }
    d.snap.reset();
    for (auto& [f, img] : d.images) destroy_image(dev, img);
    if (d.image_sampler) vkDestroySampler(dev, d.image_sampler, nullptr);
    g_sound.close();
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    vkDestroyDescriptorPool(dev, d.pool, nullptr);
}

void Launcher::handle_event(const void* ev) {
    Impl& d = *impl_;
    const SDL_Event& e = *static_cast<const SDL_Event*>(ev);
    const bool device = e.type == SDL_EVENT_GAMEPAD_ADDED || e.type == SDL_EVENT_GAMEPAD_REMOVED;
    // controllers connected while hidden must still reach ImGui's list
    if (d.mode == Impl::Mode::Hidden && !d.keyboard && !device) return;
    if (d.keyboard && (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP || e.type == SDL_EVENT_TEXT_INPUT)) return;
    if (d.capturing && !device) {
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_ESCAPE) {
            d.capturing = false;
        } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_DELETE) {
            input::clear_binding(d.capture_target, d.capture_pad);
            input::save_bindings();
            d.capturing = false;
        } else if (input::bind_from_event(d.capture_target, d.capture_pad, &e)) {
            input::save_bindings();
            d.capturing = false;
        }
        d.last = {};  // show the result at once
        return;  // the press is the binding, not navigation
    }
    if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || e.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
        // the controller drives the launcher as it drives the game: whatever is bound to the game's A selects, B goes
        // back, L / R switch tabs, the d-pad moves (input.ini / the INPUT tab)
        const uint32_t game = input::pad_buttons_for(e.gbutton.button);
        const bool down = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        static const std::pair<uint32_t, ImGuiKey> kNav[] = {
            {input::kA, ImGuiKey_GamepadFaceDown},   {input::kB, ImGuiKey_GamepadFaceRight}, {input::kX, ImGuiKey_GamepadFaceLeft},
            {input::kY, ImGuiKey_GamepadFaceUp},     {input::kL, ImGuiKey_GamepadL1},        {input::kR, ImGuiKey_GamepadR1},
            {input::kUp, ImGuiKey_GamepadDpadUp},    {input::kDown, ImGuiKey_GamepadDpadDown}, {input::kLeft, ImGuiKey_GamepadDpadLeft},
            {input::kRight, ImGuiKey_GamepadDpadRight}, {input::kPlus, ImGuiKey_GamepadStart}};
        for (const auto& [mask, key] : kNav)
            if (game & mask) ImGui::GetIO().AddKeyEvent(key, down);
        return;
    }
    if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX || e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY)) {
        const float v = std::clamp(static_cast<float>(e.gaxis.value) / 32767.0f, -1.0f, 1.0f);
        const auto analog = [](ImGuiKey key, float x) {  // past a deadzone, 0..1
            const bool on = x > 0.25f;
            ImGui::GetIO().AddKeyAnalogEvent(key, on, on ? (x - 0.25f) / 0.75f : 0.0f);
        };
        const bool x_axis = e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX;
        analog(x_axis ? ImGuiKey_GamepadLStickLeft : ImGuiKey_GamepadLStickUp, -v);
        analog(x_axis ? ImGuiKey_GamepadLStickRight : ImGuiKey_GamepadLStickDown, v);
        return;
    }
    ImGui_ImplSDL3_ProcessEvent(&e);
    if (e.type == SDL_EVENT_DROP_FILE && e.drop.data) {
        std::lock_guard g(d.dialog.m);
        d.dialog.picked.push_back(from_utf8(e.drop.data));
    }
}

bool Launcher::visible() const { return impl_->mode != Impl::Mode::Hidden || impl_->keyboard; }
void Launcher::toggle_settings() { impl_->toggle_panel(Impl::Panel::Settings); }
void Launcher::toggle_mods() { impl_->toggle_panel(Impl::Panel::Mods); }
void Launcher::Impl::toggle_panel(Panel which) {
    Impl& d = *this;
    if (d.keyboard) return;  // finish the text first
    if (d.mode == Impl::Mode::InGame && d.panel != which) {  // F1 over MODS / F2 over the settings: switch
        d.panel = which;
        d.quests_stale = true;
        d.refocus = true;
        g_sound.play(Sfx::Tab);
    } else if (d.mode == Impl::Mode::InGame) {
        d.hide();
    } else if (d.mode == Impl::Mode::Hidden) {
        // ImGui saw no events while hidden: a button that closed the panel is still "held" there, and its key repeat
        // would close the panel again at once
        ImGui::GetIO().ClearInputKeys();
        d.mode = Impl::Mode::InGame;
        d.panel = which;
        d.quests_stale = true;
        d.refocus = true;
        d.last = {};  // draw at once
    } else if (d.panel == which) {  // the title overlay: the same keys open and close its panels
        d.close_panel();
    } else {
        if (d.panel == Panel::None) g_sound.play(Sfx::Select);
        else g_sound.play(Sfx::Tab);
        d.panel = which;
        d.quests_stale = true;
        d.refocus = true;
    }
}
bool Launcher::ready_to_boot() const {
    const Impl& d = *impl_;
    return d.game && !d.translating && !d.booted;
}
fs::GameImage Launcher::take_game() {
    Impl& d = *impl_;
    d.booted = true;
    d.boot_settings = d.settings;
    d.boot_sources = d.sources;
    save_settings(d.paths, d.settings);
    return *d.game;
}
const Settings& Launcher::settings() const { return impl_->settings; }
stdfs::path Launcher::shader_dir() const { return impl_->shader_dir; }

void Launcher::post_toast(std::string text) {
    std::lock_guard g(impl_->posted_m);
    impl_->posted.push_back(std::move(text));
}

bool Launcher::frame(const LaunchState& st) {
    Impl& d = *impl_;
    {
        std::lock_guard g(d.posted_m);
        for (auto& t : d.posted) d.toast(std::move(t));
        d.posted.clear();
    }
    const input::TextEntry text = input::text_state();
    const bool kb = text.active && !text.done && !text.cancelled;
    if (kb != d.keyboard) {  // the game opened / closed its keyboard
        d.keyboard = kb;
        d.kb_shift = false;
        d.refocus = true;
        d.last = {};
        input::set_soft_keyboard(kb);
        if (kb && d.mode == Impl::Mode::Hidden) ImGui::GetIO().ClearInputKeys();  // see toggle_panel
        if (!kb && d.mode == Impl::Mode::Hidden) {
            std::lock_guard g(d.snap_m);
            d.snap.reset();
        }
    }
    if (d.dumping)
        if (const uint64_t n = gfx::texture_dumps(); n != d.dumps_shown) {
            d.dumps_shown = n;
            d.toast("Dumped " + std::to_string(n - d.dump_base) + " textures to " + d.dump_folder());
        }
    if (d.mode == Impl::Mode::Hidden && !d.keyboard && !d.toasting()) {
        if (d.toast_drawn) {  // the last toast faded: nothing of the overlay stays on screen
            d.toast_drawn = false;
            std::lock_guard g(d.snap_m);
            d.snap.reset();
        }
        return true;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - d.last < std::chrono::milliseconds(15)) return true;
    d.last = now;
    // --play: straight into the game once it can be played
    if (!d.show_menu && d.mode == Impl::Mode::Title && st.game_running && st.pipelines_ready) {
        d.hide();
        return true;
    }
    return d.draw(st);
}

bool Launcher::Impl::draw(const LaunchState& st) {
    {  // newly chosen sources: tried together with the ones chosen before
        std::lock_guard g(dialog.m);
        if (!dialog.picked.empty()) {
            for (auto& p : dialog.picked)
                if (std::find(sources.begin(), sources.end(), p) == sources.end()) sources.push_back(p);
            dialog.picked.clear();
            if (!booted) {
                game = fs::open_game(sources, &error);
                if (game) {
                    error.clear();
                    save_game_sources(paths, sources);
                    start_shaders();
                }
            } else if (auto other = fs::open_game(sources, &error)) {  // a different game: used from the next start
                error.clear();
                save_game_sources(paths, sources);
            }
        }
    }
    if (translating && job.finished) {
        translating = false;
        if (!job.ok())
            error = std::to_string(job.result.shaders - job.result.compiled) + " of " + std::to_string(job.result.shaders) +
                    " shaders could not be prepared; some effects may be missing.";
        std::printf("shaders: %d/%d ready in %.1f s\n", job.result.compiled, job.result.shaders, job.seconds());
    }
    bool quit = false, play = false;
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_HasGamepad;  // fed by handle_event (the backend clears it in manual mode)
    ImGui::NewFrame();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 size = vp->WorkSize;
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(size);
    ImGui::Begin("launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                          ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground);
    layout(size);
    if (keyboard && (mode == Mode::Hidden || mode == Mode::InGame)) {
        draw_keyboard(size);
    } else if (mode != Mode::Hidden) {
        if (mode == Mode::InGame) {  // over the game being played: dimmed
            ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), size, IM_COL32(0, 0, 0, 140));
        } else {
            draw_title(st, size, play, quit);
        }
        if (panel != Panel::None) draw_panel(size);
        if (mode != Mode::InGame) draw_status(st, size);
    }
    toast_drawn = toasting();
    if (toast_drawn) draw_toast(size);
    {
        const ImGuiContext& g = *ImGui::GetCurrentContext();
        const ImGuiID selected = g.HoveredId ? g.HoveredId : g.NavId;
        if (selected && selected != last_selected && last_selected) g_sound.play(Sfx::Move);
        last_selected = selected;
        if (g.ActiveId && g.ActiveIdIsJustActivated) g_sound.play(keyboard ? Sfx::Key : Sfx::Select);
    }
    ImGui::End();
    ImGui::Render();
    // closed during this frame (the panel's close button / Esc / B in game): nothing of it may stay on screen
    auto next = (mode == Mode::Hidden && !keyboard && !toast_drawn) || play ? nullptr : snapshot(*ImGui::GetDrawData());
    {
        std::lock_guard g(snap_m);
        snap = std::move(next);
    }
    if (play) hide();
    if (quit) save_settings(paths, settings);
    return !quit;
}

// The title overlay: the game's own intro and title screen play behind it (the drawn backdrop until the game shows
// frames). "CHA-CHA PRESENTS..." and the menu bottom-left, over a soft shade so they read on any picture.
void Launcher::Impl::draw_title(const LaunchState& st, ImVec2 size, bool& play, bool& quit) {
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
    if (!st.game_frames) draw_backdrop(bg, size, t);
    // a vignette on every border (each band fades out towards the middle, so no edge shows) plus one whole-screen
    // gradient darkest in the bottom-left corner, under the menu
    const ImU32 edge = IM_COL32(0, 0, 0, 150), clear = IM_COL32(0, 0, 0, 0);
    const float bx = size.x * 0.22f, by = size.y * 0.22f;
    bg->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(bx, size.y), edge, clear, clear, edge);                       // left
    bg->AddRectFilledMultiColor(ImVec2(size.x - bx, 0), size, clear, edge, edge, clear);                            // right
    bg->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(size.x, by), edge, edge, clear, clear);                        // top
    bg->AddRectFilledMultiColor(ImVec2(0, size.y - by), size, clear, clear, edge, edge);                            // bottom
    bg->AddRectFilledMultiColor(ImVec2(0, 0), size, clear, clear, clear, IM_COL32(0, 0, 0, 170));                   // menu corner
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float left = lay.left, menu_y = lay.menu_max_y;
    dl->AddText(fonts.body, fonts.body->FontSize, ImVec2(left + 2, menu_y - fonts.body->FontSize - 14 * scale), IM_COL32(236, 178, 84, 255),
                "CHA-CHA  PRESENTS...");
    if (mode != Mode::Title || panel != Panel::None) return;  // a panel has the focus
    const float menu_w = std::max(360.0f * scale, size.x * 0.22f);
    ImGui::SetCursorScreenPos(ImVec2(left - 6, menu_y));
    g_menu_x = left - 6;
    if (refocus) {
        ImGui::SetWindowFocus();
        ImGui::SetKeyboardFocusHere();
    }
    refocus = false;
    if (game) {
        if (menu_item("PLAY", fonts.menu, menu_w, st.game_running && st.pipelines_ready)) play = true;
    } else if (menu_item("CHOOSE GAME", fonts.menu, menu_w, !dialog.pending)) {
        static const SDL_DialogFileFilter filters[] = {{"Wii U archive (*.wua)", "wua"}, {"All files", "*"}};
        dialog.pending = true;
        SDL_ShowOpenFileDialog(&DialogResult::callback, &dialog, window, filters, 2, nullptr, false);
    }
    if (menu_item("SETTINGS", fonts.menu, menu_w)) {
        panel = Panel::Settings;
        refocus = true;
    }
    if (menu_item("MODS", fonts.menu, menu_w)) {
        panel = Panel::Mods;
        quests_stale = true;
        refocus = true;
    }
    if (menu_item("QUIT", fonts.menu, menu_w)) quit = true;
}

// Status bottom-left (the game, or what is missing), toasts (shader translation, pipeline prebuild), version
// bottom-right.
void Launcher::Impl::draw_status(const LaunchState& st, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float left = lay.left, bottom = size.y - 22 * scale;
    const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
    if (mode == Mode::Title) {
        const char* status = game ? "Game ready." : dialog.pending ? "Choose your game..." : "No game chosen.";
        const ImU32 status_col = game ? IM_COL32(170, 230, 140, 255) : IM_COL32(255, 160, 120, 255);
        dl->AddText(fonts.body, fonts.body->FontSize, ImVec2(left + 4, lay.status_y), status_col, status);
        if (game) {  // check mark, drawn (the UI font has no such glyph)
            const float x = left - 18 * scale, y = lay.status_y + fonts.body->FontSize * 0.55f;
            const ImVec2 pts[3] = {ImVec2(x, y), ImVec2(x + 5 * scale, y + 5 * scale), ImVec2(x + 13 * scale, y - 6 * scale)};
            dl->AddPolyline(pts, 3, status_col, 0, 2.0f * scale);
        }
        const std::string detail = game ? "Wii U  \xC2\xB7  USA  \xC2\xB7  update v" + std::to_string(game->update.version)
                                        : error.empty() ? "A .wua file or the game + update folders (code, content, meta)" : error;
        dl->AddText(fonts.note, fonts.note->FontSize, ImVec2(left + 4, lay.detail_y), IM_COL32(200, 190, 175, 220), detail.c_str(),
                    nullptr, size.x * 0.5f);
    }
    int done = 0, total = 0;
    const char* what = nullptr;
    if (translating) {
        done = job.progress.done;
        total = job.progress.total;
        what = total ? "Compiling %d shaders" : "Reading the game files";
    } else if (game && !st.pipelines_ready) {
        done = static_cast<int>(st.pipelines_done);
        total = static_cast<int>(st.pipelines_total);
        what = total ? "Compiling %d pipelines" : "Starting the game";
    }
    if (what) {
        const float ty = lay.toast_y;  // above the status lines
        const ImVec2 a(left - 10, ty), b(left + 250 * scale, ty + 42 * scale);
        dl->AddRectFilled(a, b, IM_COL32(20, 16, 12, 220), 4.0f);
        dl->AddRect(a, b, IM_COL32(255, 255, 255, 40), 4.0f);
        const float ang = t * 6.0f, cx = a.x + 18 * scale, cy = (a.y + b.y) * 0.5f - 2;
        dl->PathArcTo(ImVec2(cx, cy), 7 * scale, ang, ang + 4.4f, 16);
        dl->PathStroke(IM_COL32(240, 180, 90, 255), 0, 2.0f);
        char label[64];
        std::snprintf(label, sizeof label, what, total);
        dl->AddText(fonts.note, fonts.note->FontSize, ImVec2(a.x + 34 * scale, cy - fonts.note->FontSize * 0.5f - 2), kText, label);
        const float frac = total ? static_cast<float>(done) / static_cast<float>(total) : 0.0f;
        dl->AddRectFilled(ImVec2(a.x + 2, b.y - 4), ImVec2(a.x + 2 + (b.x - a.x - 4) * frac, b.y - 2), IM_COL32(240, 170, 60, 255));
    }
    const std::string version = std::string("Cha-Cha ") + MH3U_VERSION;
    const ImVec2 vs = fonts.body->CalcTextSizeA(fonts.body->FontSize, FLT_MAX, 0, version.c_str());
    dl->AddText(fonts.body, fonts.body->FontSize, ImVec2(size.x - vs.x - 40 * scale, bottom - vs.y), IM_COL32(235, 228, 215, 230), version.c_str());
}

void Launcher::Impl::draw_toast(ImVec2 size) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float wrap = std::min(520 * scale, size.x * 0.5f);
    const ImVec2 ts = fonts.note->CalcTextSizeA(fonts.note->FontSize, FLT_MAX, wrap, toast_text.c_str());
    const float pad = 14 * scale;
    const ImVec2 b(size.x - 30 * scale, 30 * scale + ts.y + 2 * pad), a(b.x - ts.x - 2 * pad - 6 * scale, 30 * scale);
    // fades out over its last half second
    const float left = std::chrono::duration<float>(toast_until - std::chrono::steady_clock::now()).count();
    const float alpha = std::clamp(left * 2.0f, 0.0f, 1.0f);
    const auto fade = [&](int a8) { return static_cast<ImU32>(static_cast<float>(a8) * alpha) << 24; };
    dl->AddRectFilled(a, b, IM_COL32(20, 16, 12, 0) | fade(225), 4.0f);
    dl->AddRect(a, b, IM_COL32(255, 255, 255, 0) | fade(40), 4.0f);
    dl->AddRectFilled(ImVec2(a.x, a.y + 4), ImVec2(a.x + 4 * scale, b.y - 4), IM_COL32(240, 170, 60, 0) | fade(255));
    dl->AddText(fonts.note, fonts.note->FontSize, ImVec2(a.x + pad + 6 * scale, a.y + pad), (kText & 0x00FFFFFFu) | fade(255),
                toast_text.c_str(), nullptr, wrap);
}

// The settings window: a header bar with the tab switcher (LB / RB or Q / E switch) and the close button, grouped
// rows on the left, the selected row's description on the right, and a footer with the control guide (the buttons
// as bound right now) and APPLY. Esc / B close it. MODS uses the same frame.
void Launcher::Impl::draw_panel(ImVec2 size) {
    const float s = scale;
    const float pw = std::min(size.x - 60 * s, 1240 * s), ph = std::min(size.y - 80 * s, 720 * s);
    const float split = 0.58f;  // rows on the left, the description pane on the right
    const ImVec2 a((size.x - pw) * 0.5f, (size.y - ph) * 0.5f), b(a.x + pw, a.y + ph);
    const float r = 14 * s;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 1; i <= 8; ++i) {  // a soft shadow, offset downwards
        const float g = i * 3.0f * s;
        dl->AddRectFilled(ImVec2(a.x - g * 0.5f, a.y - g * 0.5f + 8 * s), ImVec2(b.x + g * 0.5f, b.y + g * 0.5f + 8 * s), IM_COL32(0, 0, 0, 14), r + g);
    }
    dl->AddRectFilled(a, b, kPanelBg, r);
    dl->AddRect(a, b, kPanelEdge, r);

    // header bar: the tabs as one pill of segments in the middle, the bumpers' prompts beside it, close on the right
    struct TabDef {
        Tab tab;
        const char* name;
    };
    static const TabDef kSettingsTabs[] = {{Tab::General, "General"}, {Tab::Video, "Video"}, {Tab::Input, "Input"}};
    static const char* const kModsTabs[] = {"Quests"};
    const bool mods = panel == Panel::Mods;
    if (mods && quests_stale) refresh_quests();  // also applies the pack modes
    const int ntabs = mods ? 1 : 3;
    const float header_h = 72 * s, seg_h = 38 * s, seg_pad = 24 * s;
    float seg_total = 0;
    for (int i = 0; i < ntabs; ++i)
        seg_total += fonts.body->CalcTextSizeA(fonts.body->FontSize, FLT_MAX, 0, mods ? kModsTabs[i] : kSettingsTabs[i].name).x + 2 * seg_pad;
    const float sy = a.y + (header_h - seg_h) * 0.5f;
    float tx = (a.x + b.x) * 0.5f - seg_total * 0.5f;
    const float seg_x0 = tx;
    if (ntabs > 1) dl->AddRectFilled(ImVec2(tx - 3 * s, sy - 3 * s), ImVec2(tx + seg_total + 3 * s, sy + seg_h + 3 * s), IM_COL32(0, 0, 0, 70), seg_h);
    for (int i = 0; i < ntabs; ++i) {
        const char* name = mods ? kModsTabs[i] : kSettingsTabs[i].name;
        const bool on = mods ? static_cast<int>(mods_tab) == i : tab == kSettingsTabs[i].tab;
        const ImVec2 ts = fonts.body->CalcTextSizeA(fonts.body->FontSize, FLT_MAX, 0, name);
        const float w = ts.x + 2 * seg_pad;
        ImGui::SetCursorScreenPos(ImVec2(tx, sy));
        ImGui::PushID(name);
        if (ImGui::InvisibleButton("tab", ImVec2(w, seg_h), ImGuiButtonFlags_NoNavFocus) && !on) {
            if (mods) mods_tab = static_cast<ModsTab>(i);
            else tab = kSettingsTabs[i].tab;
            g_sound.play(Sfx::Tab);
            refocus = true;
        }
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        if (on && ntabs > 1) dl->AddRectFilled(ImVec2(tx, sy), ImVec2(tx + w, sy + seg_h), IM_COL32(255, 236, 200, 34), seg_h * 0.5f);
        else if (hov && ntabs > 1) dl->AddRectFilled(ImVec2(tx, sy), ImVec2(tx + w, sy + seg_h), kHover, seg_h * 0.5f);
        dl->AddText(fonts.body, fonts.body->FontSize, ImVec2(tx + seg_pad, sy + (seg_h - ts.y) * 0.5f), on ? kText : hov ? kSoft : kDim, name);
        tx += w;
    }
    if (ntabs > 1) {  // which buttons switch tabs
        const std::string lp = input::pad_prompt("l"), rp = input::pad_prompt("r");
        const bool pad = !lp.empty() && !rp.empty();
        const float cy = sy + seg_h * 0.5f;
        const float lw = key_cap(dl, ImVec2(0, 0), pad ? lp : "Q", false);
        key_cap(dl, ImVec2(seg_x0 - 16 * s - lw, cy), pad ? lp : "Q", true);
        key_cap(dl, ImVec2(seg_x0 + seg_total + 16 * s, cy), pad ? rp : "E", true);
    }
    if (!capturing) {  // LB / RB, Q / E
        int step = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1) || ImGui::IsKeyPressed(ImGuiKey_Q, false)) step = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1) || ImGui::IsKeyPressed(ImGuiKey_E, false)) step = 1;
        if (step && !mods) {
            tab = static_cast<Tab>((static_cast<int>(tab) + step + 3) % 3);
            g_sound.play(Sfx::Tab);
            refocus = true;
        }
    }
    // close button: a round header button
    const float cr = 18 * s;
    const ImVec2 cc(b.x - 32 * s - cr, a.y + header_h * 0.5f);  // on the content's right edge, like Apply
    ImGui::SetCursorScreenPos(ImVec2(cc.x - cr, cc.y - cr));
    const bool close = ImGui::InvisibleButton("close", ImVec2(2 * cr, 2 * cr), ImGuiButtonFlags_NoNavFocus);
    {
        const bool hov = ImGui::IsItemHovered();
        dl->AddCircleFilled(cc, cr, hov ? IM_COL32(255, 236, 200, 40) : kFill);
        const float x = 6 * s;
        const ImU32 col = hov ? kText : kSoft;
        dl->AddLine(ImVec2(cc.x - x, cc.y - x), ImVec2(cc.x + x, cc.y + x), col, 2.0f * s);
        dl->AddLine(ImVec2(cc.x - x, cc.y + x), ImVec2(cc.x + x, cc.y - x), col, 2.0f * s);
    }
    if (close || (!capturing && (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)))) {
        close_panel();
        return;
    }

    if (!draft_open) {
        draft = settings;
        draft_open = true;
    }
    // body: groups of rows left (scrolling), the description right; INPUT uses the full width for its table
    const bool full = !mods && tab == Tab::Input;
    const float footer_h = 64 * s;
    // one spacing scale (4 / 8 / 16 / 24 / 32): the content sits 32 in from the panel's edges, rows end 16 before the
    // scroll bar, the description pane starts 32 after the rows
    const float pad = 32 * s, top = a.y + header_h, body_h = b.y - top - footer_h - 16 * s;
    // the cards keep the same 32 margin on both sides: to the panel's edge on the left, to the panel's edge or the
    // description pane on the right; the scroll bar sits in the middle of that right margin
    const float cards_w = full ? pw - 2 * pad : pw * split - pad * 1.5f;
    const float col_w = cards_w + pad * 0.5f + ImGui::GetStyle().ScrollbarSize * 0.5f;
    PanelLayout p;
    p.label = fonts.body;
    p.value = fonts.body;
    p.small = fonts.note;
    p.scale = s;
    p.row_h = fonts.body->FontSize + 28 * s;
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, top));
    ImGui::BeginChild("rows", ImVec2(col_w, body_h), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoBackground);
    p.x = ImGui::GetCursorScreenPos().x;
    p.w = cards_w;
    ImGui::Dummy(ImVec2(1, 4 * s));
    if (refocus) {
        ImGui::SetKeyboardFocusHere();
        refocus = false;
    }
    if (mods) {
        draw_tab_quests(p);
    } else if (tab == Tab::General) {
        draw_tab_general(p);
    } else if (tab == Tab::Video) {
        draw_tab_video(p);
    } else {
        draw_tab_input(p, p.w);
    }
    ImGui::EndChild();
    if (!full) {  // description of the selected row
        const float dx = a.x + pad + cards_w + pad, wrap = b.x - pad - dx;
        const float ty = top + 4 * s + fonts.note->FontSize + 8 * s;  // level with the first card
        if (p.sel_title) {
            dl->AddText(fonts.menu, fonts.menu->FontSize, ImVec2(dx, ty), kText, p.sel_title);
            const float text_y = ty + fonts.menu->FontSize + 12 * s;
            const char* desc = p.sel_desc ? p.sel_desc : "";
            dl->AddText(fonts.body, fonts.body->FontSize, ImVec2(dx, text_y), kSoft, desc, nullptr, wrap);
            // a quest's or pack's picture below its text, fitted to the space left
            if (const LauncherImage* img = image(p.sel_image)) {
                const float y = text_y + fonts.body->CalcTextSizeA(fonts.body->FontSize, FLT_MAX, wrap, desc).y + 24 * s;
                const float room_h = b.y - footer_h - pad - y;
                const float k = std::min(wrap / img->w, room_h / img->h);
                if (k > 0 && room_h > 40 * s) {
                    const ImVec2 i0(dx, y), i1(i0.x + img->w * k, y + img->h * k);
                    dl->AddImageRounded(img->id(), i0, i1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 10 * s);
                }
            }
        }
        if (!mods && booted && (settings.render_scale != boot_settings.render_scale || settings.fps_limit != boot_settings.fps_limit ||
                       settings.vsync != boot_settings.vsync || (!sources.empty() && sources != boot_sources))) {
            const char* note = "Internal resolution, frame rate, VSync and game changes apply on the next start.";
            const float nh = fonts.note->CalcTextSizeA(fonts.note->FontSize, FLT_MAX, wrap - 40 * s, note).y + 24 * s;
            const ImVec2 n0(dx, b.y - footer_h - pad - nh), n1(b.x - pad, b.y - footer_h - pad);
            dl->AddRectFilled(n0, n1, IM_COL32(236, 178, 84, 26), 10 * s);
            dl->AddCircleFilled(ImVec2(n0.x + 20 * s, n0.y + nh * 0.5f), 4 * s, kAmber);
            dl->AddText(fonts.note, fonts.note->FontSize, ImVec2(n0.x + 36 * s, n0.y + 12 * s), kText, note, nullptr, wrap - 40 * s);
        }
    }
    // footer: the control guide; unapplied changes and APPLY (Start on a controller) on the right
    const float fy = b.y - footer_h;
    dl->AddRectFilled(ImVec2(a.x + 1, fy), ImVec2(b.x - 1, b.y - 1), IM_COL32(0, 0, 0, 46), r - 1, ImDrawFlags_RoundCornersBottom);
    dl->AddLine(ImVec2(a.x + 1, fy), ImVec2(b.x - 1, fy), kHairline);
    draw_guide(dl, ImVec2(a.x + pad, fy + footer_h * 0.5f), footer_h, p.sel_changes);
    if (mods) return;  // the quests apply at once
    const bool dirty = !(draft == settings);
    const float bh = 40 * s;
    const ImVec2 ls = fonts.body->CalcTextSizeA(fonts.body->FontSize, FLT_MAX, 0, "Apply");
    const float bw = ls.x + 48 * s;
    const ImVec2 b0(b.x - pad - bw, fy + (footer_h - bh) * 0.5f), b1(b0.x + bw, b0.y + bh);
    ImGui::SetCursorScreenPos(b0);
    ImGui::BeginDisabled(!dirty);
    const bool clicked = ImGui::InvisibleButton("apply", ImVec2(bw, bh), ImGuiButtonFlags_EnableNav);
    const bool sel = dirty && (ImGui::IsItemHovered() || ImGui::IsItemFocused());
    ImGui::EndDisabled();
    dl->AddRectFilled(b0, b1, dirty ? (sel ? IM_COL32(246, 192, 104, 255) : kAmber) : kFill, bh * 0.5f);
    dl->AddText(fonts.body, fonts.body->FontSize, ImVec2(b0.x + (bw - ls.x) * 0.5f, b0.y + (bh - ls.y) * 0.5f), dirty ? kOnAmber : kOff, "Apply");
    if (dirty) {
        const char* note = "Unapplied changes";
        const ImVec2 ns = fonts.note->CalcTextSizeA(fonts.note->FontSize, FLT_MAX, 0, note);
        dl->AddText(fonts.note, fonts.note->FontSize, ImVec2(b0.x - 16 * s - ns.x, fy + (footer_h - ns.y) * 0.5f), kAmber, note);
    }
    if (dirty && (clicked || ImGui::IsKeyPressed(ImGuiKey_GamepadStart) || (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Enter))))
        apply();
}

// The control guide: each button as a key cap followed by what it does, from the current bindings (the controller's
// own button names, the face buttons drawn as PlayStation symbols on those pads), or the keyboard's keys with no
// controller connected. Returns its width.
float Launcher::Impl::draw_guide(ImDrawList* dl, ImVec2 at, float, bool changes) {
    const std::string sel = input::pad_prompt("a"), back = input::pad_prompt("b"), apply_key = input::pad_prompt("plus");
    const bool pad = !sel.empty() && !back.empty();
    const bool settings_tab = panel == Panel::Settings && !(draft == settings);  // Apply, while there is something to apply
    struct Item {
        std::string key;
        const char* what;
    };
    std::vector<Item> items;
    if (capturing) {
        items = {{"Esc", "Cancel"}, {"Delete", "Clear"}};
    } else {
        items.push_back({pad ? sel : "Enter", panel == Panel::Settings && tab == Tab::Input ? "Rebind" : "Select"});
        if (changes) items.push_back({"<>", "Change"});
        if (settings_tab) items.push_back({pad ? (apply_key.empty() ? "+" : apply_key) : "Ctrl+Enter", "Apply"});
        items.push_back({pad ? back : "Esc", "Close"});
    }
    float x = at.x;
    for (const Item& it : items) {
        x += key_cap(dl, ImVec2(x, at.y), it.key, true) + 8 * scale;
        const ImVec2 ts = fonts.note->CalcTextSizeA(fonts.note->FontSize, FLT_MAX, 0, it.what);
        dl->AddText(fonts.note, fonts.note->FontSize, ImVec2(x, at.y - ts.y * 0.5f), kSoft, it.what);
        x += ts.x + 24 * scale;
    }
    return x - at.x;
}

// One button as a key cap, its left edge at at.x, centred on at.y: face buttons round (PlayStation's drawn as their
// symbols), "<>" the d-pad's left / right, anything else (LB, Options, Esc, Ctrl+Enter...) a rounded key with its
// name. Returns its width; draw false only measures.
float Launcher::Impl::key_cap(ImDrawList* dl, ImVec2 at, const std::string& key, bool draw) {
    const float s = scale, h = 26 * s, cy = at.y, line = 1.8f * s;
    const bool ps = key == "Cross" || key == "Circle" || key == "Square" || key == "Triangle";
    if (ps || key == "A" || key == "B" || key == "X" || key == "Y") {
        if (!draw) return h;
        const ImVec2 c(at.x + h * 0.5f, cy);
        dl->AddCircleFilled(c, h * 0.5f, kFill);
        const float k = 5.5f * s;
        if (key == "Cross") {
            dl->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), kText, line);
            dl->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), kText, line);
        } else if (key == "Circle") {
            dl->AddCircle(c, k + 0.5f * s, kText, 0, line);
        } else if (key == "Square") {
            dl->AddRect(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), kText, 0, 0, line);
        } else if (key == "Triangle") {
            dl->AddTriangle(ImVec2(c.x, c.y - k * 1.1f), ImVec2(c.x + k * 1.15f, c.y + k * 0.85f), ImVec2(c.x - k * 1.15f, c.y + k * 0.85f), kText, line);
        } else {
            const ImVec2 ts = fonts.note->CalcTextSizeA(fonts.note->FontSize, FLT_MAX, 0, key.c_str());
            dl->AddText(fonts.note, fonts.note->FontSize, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), kText, key.c_str());
        }
        return h;
    }
    if (key == "<>") {
        const float w = 44 * s;
        if (!draw) return w;
        dl->AddRectFilled(ImVec2(at.x, cy - h * 0.5f), ImVec2(at.x + w, cy + h * 0.5f), kFill, 7 * s);
        arrow(dl, ImVec2(at.x + 14 * s, cy), 5 * s, -1, kText);
        arrow(dl, ImVec2(at.x + w - 14 * s, cy), 5 * s, 1, kText);
        return w;
    }
    const ImVec2 ts = fonts.note->CalcTextSizeA(fonts.note->FontSize, FLT_MAX, 0, key.c_str());
    const float w = std::max(h, ts.x + 18 * s);
    if (!draw) return w;
    dl->AddRectFilled(ImVec2(at.x, cy - h * 0.5f), ImVec2(at.x + w, cy + h * 0.5f), kFill, 7 * s);
    dl->AddText(fonts.note, fonts.note->FontSize, ImVec2(at.x + (w - ts.x) * 0.5f, cy - ts.y * 0.5f), kText, key.c_str());
    return w;
}

void Launcher::Impl::draw_tab_general(PanelLayout& p) {
    const std::string game_name = game ? "Monster Hunter 3 Ultimate (v" + std::to_string(game->update.version) + ")" : "None";
    group_begin(p, "Game");
    info_row(p, "Game", game_name, "Your own dump of the game: a .wua file, or the game and update folders. Nothing is copied.");
    info_row(p, "Data folder", short_path(paths.data), "Where settings, saves, caches and input.ini are kept.");
    if (button_row(p, "Change game...", "Pick a .wua file. Takes effect on the next start once the game runs.", !dialog.pending)) {
        sources.clear();
        static const SDL_DialogFileFilter filters[] = {{"Wii U archive (*.wua)", "wua"}, {"All files", "*"}};
        dialog.pending = true;
        SDL_ShowOpenFileDialog(&DialogResult::callback, &dialog, window, filters, 2, nullptr, false);
    }
    if (button_row(p, "Choose a game folder...", "Pick a dumped game or update folder (code, content, meta).", !dialog.pending)) {
        dialog.pending = true;
        SDL_ShowOpenFolderDialog(&DialogResult::callback, &dialog, window, nullptr, false);
    }
    group_end(p);
    group_begin(p, "Startup");
    switch_row(p, "Skip boot notices", draft.skip_boot_notices,
               "Answers the controller notice and the create-save prompts before the title screen by itself.");
    group_end(p);
}

// Window size and internal resolution are independent: the game renders at its internal resolution and the picture
// is scaled to whatever the window (or screen) is.
void Launcher::Impl::draw_tab_video(PanelLayout& p) {
    group_begin(p, "Display");
    int mode_i = static_cast<int>(std::find(kDisplayModes.begin(), kDisplayModes.end(), draft.display_mode) - kDisplayModes.begin()) % 3;
    if (cycle(value_row(p, "Display mode", kDisplayNames[static_cast<size_t>(mode_i)],
                        "Windowed, borderless (fills the screen at desktop resolution) or exclusive fullscreen."),
              mode_i, 3))
        draft.display_mode = kDisplayModes[static_cast<size_t>(mode_i)];
    const std::vector<std::string> sizes = window_sizes();
    int size_i = static_cast<int>(std::find(sizes.begin(), sizes.end(), draft.window_size) - sizes.begin());
    if (size_i >= static_cast<int>(sizes.size())) size_i = 0;
    if (cycle(value_row(p, "Window size", sizes[static_cast<size_t>(size_i)],
                        "The size of the window in windowed mode (you can also drag its edges). It does not change the internal "
                        "resolution: the picture is scaled to fit.",
                        draft.display_mode == "windowed"),
              size_i, static_cast<int>(sizes.size())))
        draft.window_size = sizes[static_cast<size_t>(size_i)];
    int scale_i = 2;
    for (size_t i = 0; i < kScales.size(); ++i)
        if (std::abs(kScales[i] - draft.render_scale) < 0.01f) scale_i = static_cast<int>(i);
    const float s = kScales[static_cast<size_t>(scale_i)];
    scale_desc = "The resolution the game renders at, independent of the window: " + std::to_string(static_cast<int>(1280 * s)) + "x" +
                 std::to_string(static_cast<int>(720 * s)) + " (" + kScaleNames[static_cast<size_t>(scale_i)] +
                 " the native 1280x720). Higher than your screen supersamples (sharper, slower); lower saves GPU time.";
    if (cycle(value_row(p, "Internal resolution", kScaleNames[static_cast<size_t>(scale_i)], scale_desc.c_str()), scale_i,
              static_cast<int>(kScales.size())))
        draft.render_scale = kScales[static_cast<size_t>(scale_i)];
    static const std::vector<int> kFps = {0, 30, 60, 120, 144, 240, -1};
    static const std::vector<std::string> kFpsNames = {"Display refresh", "30", "60", "120", "144", "240", "Unlimited"};
    int fps = static_cast<int>(std::find(kFps.begin(), kFps.end(), draft.fps_limit) - kFps.begin());
    if (fps >= static_cast<int>(kFps.size())) fps = 0;
    if (cycle(value_row(p, "Frame rate limit", kFpsNames[static_cast<size_t>(fps)], "The most frames per second the game draws."), fps,
              static_cast<int>(kFps.size())))
        draft.fps_limit = kFps[static_cast<size_t>(fps)];
    switch_row(p, "VSync", draft.vsync, "Waits for the display's refresh: no tearing, a little more latency.");
    group_end(p);

    group_begin(p, "Picture");
    int look = draft.look == "tri" ? 1 : 0;
    if (cycle(value_row(p, "Look", look ? "Tri (Wii)" : "Original",
                        "Original is MH3U as released. Tri gives the world the look of Monster Hunter Tri on the Wii: softer, "
                        "muted colour, milky shadows, a gentle glow and haze in the distance. Menus and the HUD stay as they are."),
              look, 2))
        draft.look = look ? "tri" : "original";
    switch_row(p, "Bloom", draft.bloom, "The game's glow around bright light.");
    int dof = draft.depth_of_field == "enhanced" ? 2 : draft.depth_of_field == "off" ? 1 : 0;
    static const std::vector<std::string> kDof = {"Original", "Off", "Enhanced"};
    if (cycle(value_row(p, "Depth of field", kDof[static_cast<size_t>(dof)],
                        "Original: the game's own (cutscenes). Enhanced also softens the distance during play, focused on your hunter."),
              dof, 3))
        draft.depth_of_field = dof == 2 ? "enhanced" : dof == 1 ? "off" : "original";
    switch_row(p, "Ambient occlusion", draft.ambient_occlusion, "Soft contact shadows where surfaces meet. Not in the original game.");
    switch_row(p, "Global illumination", draft.global_illumination,
               "Light bounces between surfaces: sunlit ground and walls tint what is next to them. Not in the original game.");
    switch_row(p, "Vignette", draft.vignette, "Gently darker screen corners, drawing the eye to the middle. Not in the original game.");
    group_end(p);

    group_begin(p, "Textures");
    switch_row(p, "Texture packs", draft.texture_packs,
               "Replacement textures from the texture_replacements folder (DDS files named after the game texture).");
    // applies at once, without APPLY (a one-off action, not a saved setting)
    if (switch_row(p, "Dump textures", dumping,
                   "Saves every texture the game shows as a DDS file in the texture_dump folder, named so that a copy in "
                   "texture_replacements replaces it - the way to make a texture pack. Starts at once; off again next start.")) {
        gfx::set_texture_dump(dumping);
        dump_base = dumps_shown = gfx::texture_dumps();
        toast(dumping ? "Dumping textures to " + dump_folder() : "Texture dump off - " + std::to_string(dumps_shown) + " files so far in " + dump_folder());
    }
    group_end(p);
}

// MODS -> QUESTS. Packs (top-level folders of <data>/quests) have a mode: All keeps every quest of the pack installed
// (new ones too, applied whenever the list is read), Choose shows the pack's quests to pick one by one, Off removes
// them. Quests straight in the folder are picked one by one. The official DLC (quests from Wii U save files) starts as
// All. A change also updates the copy of the quest file the game already loaded, so the Gathering Hall lists it the
// next time it is opened.
stdfs::path Launcher::Impl::quest_dir() const { return paths.data / "quests"; }
stdfs::path Launcher::Impl::save_dir() const { return paths.data / "save" / "80000001"; }

quests::PackMode Launcher::Impl::pack_mode(const quests::Pack& p) const {
    auto it = pack_modes.find(p.key);
    return it != pack_modes.end() ? it->second : quests::default_mode(p);
}

void Launcher::Impl::load_pack_modes() {
    pack_modes.clear();
    std::ifstream f(paths.data / "mods.ini");
    for (std::string line; std::getline(f, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.rfind('=');
        if (line.rfind("pack.", 0) != 0 || eq == std::string::npos) continue;
        pack_modes[line.substr(5, eq - 5)] = quests::mode_from_name(line.substr(eq + 1), quests::PackMode::Choose);
    }
}

void Launcher::Impl::save_pack_modes() const {
    std::ofstream f(paths.data / "mods.ini", std::ios::trunc);
    f << "# Cha-Cha mods. pack.<folder in quests> = all | choose | off\n";
    for (const auto& [k, m] : pack_modes) f << "pack." << k << '=' << quests::mode_name(m) << '\n';
}

// The game reads a quest file in 64 KB pieces into one scratch buffer and copies them on, so the copy it keeps is
// found by content: a slot of the same file that holds a quest (found by its 16-byte file name, then compared whole)
// gives the copy's first slot, and the changed slot is written there if it still holds the old bytes. Found copies
// are remembered per file and checked again before each use. -1: the game has not read the file yet (it reads the
// changed one later); else how many copies were updated.
int Launcher::Impl::sync_live(const quests::SlotChange& c) {
    core::GuestMemory* gm = core::GuestMemory::current();
    const size_t reads = gm ? os::save_reads(c.file).size() : 0;
    if (!reads) return -1;
    uint8_t* const base = gm->base();
    const auto fits = [&](uint32_t first_slot) {
        const uint8_t* m = base + first_slot + (c.offset - quests::kFirstSlot);
        return gm->committed(first_slot) && (std::memcmp(m, c.before.data(), c.before.size()) == 0 ||
                                             std::memcmp(m, c.after.data(), c.after.size()) == 0);
    };
    std::vector<uint32_t>& copies = live_copies[c.file];
    copies.erase(std::remove_if(copies.begin(), copies.end(), [&](uint32_t b) { return !fits(b); }), copies.end());
    if (copies.empty() && live_missed[c.file] == reads) return 0;  // searched since the game last read it: not there
    if (copies.empty()) {
        // anchors: the file's other filled slots (unchanged) and the changed slot's old content
        std::ifstream f(save_dir() / c.file, std::ios::binary);
        const std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::vector<std::pair<size_t, std::vector<uint8_t>>> anchors;
        if (c.before.size() >= 4 && std::memcmp(c.before.data(), "QTDS", 4) == 0) anchors.emplace_back(c.offset, c.before);
        for (size_t o = quests::kFirstSlot; o + quests::kSlotSize <= file.size() && anchors.size() < 3; o += quests::kSlotSize)
            if (o != c.offset && std::memcmp(&file[o], "QTDS", 4) == 0)
                anchors.emplace_back(o, std::vector<uint8_t>(file.begin() + static_cast<std::ptrdiff_t>(o),
                                                             file.begin() + static_cast<std::ptrdiff_t>(o + quests::kSlotSize)));
        for (const auto& [o, bytes] : anchors) {
            for (uint32_t at : gm->find(&bytes[quests::kSlotData], 16, core::layout::kMem2HeapBase, core::layout::kMem2HeapEnd)) {
                const uint32_t slot = at - static_cast<uint32_t>(quests::kSlotData);
                const uint32_t first = slot - static_cast<uint32_t>(o - quests::kFirstSlot);
                if (std::memcmp(base + slot, bytes.data(), bytes.size()) == 0 && fits(first) &&
                    std::find(copies.begin(), copies.end(), first) == copies.end())
                    copies.push_back(first);
            }
            if (!copies.empty()) break;
        }
    }
    if (copies.empty()) live_missed[c.file] = reads;
    int updated = 0;
    for (uint32_t first : copies) {
        std::memcpy(base + first + (c.offset - quests::kFirstSlot), c.after.data(), c.after.size());
        ++updated;
    }
    std::printf("quests: %s slot 0x%zX: %d copies in game memory updated\n", c.file.c_str(), c.offset, updated);
    std::fflush(stdout);
    return updated;
}

bool Launcher::Impl::change_quest(const quests::Quest& q, bool remove, std::string& err) {
    quests::SlotChange c;
    if (!(remove ? quests::uninstall(save_dir(), q.name, &err, &c) : quests::install(save_dir(), q, &err, &c))) return false;
    if (sync_live(c) == 0) stale_in_game = true;  // the game holds the list elsewhere: reloading the save shows it
    return true;
}

void Launcher::Impl::refresh_quests() {
    std::error_code ec;
    stdfs::create_directories(quest_dir(), ec);
    quest_library = quests::scan_library(quest_dir());
    load_pack_modes();
    quests_installed = quests::installed(save_dir());
    // packs set to All / Off: bring the save in line
    int added = 0, removed = 0;
    std::string err;
    stale_in_game = false;
    for (const auto& pk : quest_library.packs) {
        const quests::PackMode m = pack_mode(pk);
        if (m == quests::PackMode::Choose) continue;
        for (size_t i : pk.quests) {
            const quests::Quest& q = quest_library.quests[i];
            const bool inst = std::any_of(quests_installed.begin(), quests_installed.end(), [&](const quests::Quest& s) { return s.name == q.name; });
            if (m == quests::PackMode::All && !inst) added += change_quest(q, false, err) ? 1 : 0;
            if (m == quests::PackMode::Off && inst) removed += change_quest(q, true, err) ? 1 : 0;
        }
    }
    if (added || removed) {
        quests_installed = quests::installed(save_dir());
        std::string t = "Quest packs: ";
        if (added) t += std::to_string(added) + " installed";
        if (added && removed) t += ", ";
        if (removed) t += std::to_string(removed) + " removed";
        toast(t + (stale_in_game ? " - reload your save to see them" : ""));
    } else if (!err.empty() && err.find("no save") == std::string::npos) {
        toast("Quest packs: " + err);
    }
    quests_stale = false;
}

const LauncherImage* Launcher::Impl::image(const stdfs::path& file) {
    if (file.empty() || bad_images.count(file)) return nullptr;
    if (auto it = images.find(file); it != images.end()) return &it->second;
    std::vector<uint8_t> px;
    uint32_t w = 0, h = 0;
    LauncherImage img;
    if (images.size() >= 48 || !decode_image(file, px, w, h) || !upload_image(presenter.vk(), px, w, h, image_sampler, img)) {
        bad_images.insert(file);
        return nullptr;
    }
    return &images.emplace(file, img).first->second;
}

void Launcher::Impl::draw_tab_quests(PanelLayout& p) {
    const auto u = quest_dir().u8string();
    const std::string folder(u.begin(), u.end());
    static std::string folder_desc, empty_desc;
    folder_desc = "Official DLC and custom quests. Put each pack in its own folder here: q_<number>.quest files (with "
                  "q_<number>.png and .txt for a picture and a description), or the quest1-quest4 files from a Wii U "
                  "save. A pack.json (name, author, description) and cover.png describe a pack.\n\n" + folder;
    group_begin(p, "Library");
    if (button_row(p, "Quest folder", folder_desc.c_str(), true, "Open")) {
        std::string url = "file:///" + folder;
        std::replace(url.begin(), url.end(), '\\', '/');
        SDL_OpenURL(url.c_str());
    }
    if (button_row(p, "Refresh", "Read the quest folder again (after adding files while Cha-Cha runs).", true, "Rescan")) {
        quests_stale = true;
        toast("Quest folder read again");
    }
    int ev = 0, ch = 0;
    for (const auto& q : quests_installed) ++(q.challenge() ? ch : ev);
    char used[64];
    std::snprintf(used, sizeof used, "%d / 60 event, %d / 20 challenge", ev, ch);
    info_row(p, "Installed", used, "Quests in your save. They are listed at the Port Tanzia Gathering Hall with the event and challenge quests.");
    if (quest_library.quests.empty()) {
        empty_desc = "No quests in " + folder + " yet.";
        info_row(p, "No quests in the folder yet", "", empty_desc.c_str());
    }
    group_end(p);
    const auto is_installed = [&](const quests::Quest& q) {
        return std::any_of(quests_installed.begin(), quests_installed.end(), [&](const quests::Quest& s) { return s.name == q.name; });
    };
    const quests::Quest* act = nullptr;
    bool act_remove = false;
    // a quest: a switch, on while it is in the save
    const auto quest_row = [&](const quests::Quest& q, float indent) {
        const bool inst = is_installed(q);
        bool on = inst;
        ImGui::PushID(q.name.c_str());
        p.indent = indent;
        if (switch_row(p, q.title.c_str(), on, q.description.c_str())) {
            act = &q;
            act_remove = inst;
        }
        p.indent = 0;
        if (p.sel_title == q.title.c_str()) p.sel_image = q.image;
        ImGui::PopID();
    };
    // packs: official first, then by folder name
    std::vector<size_t> order(quest_library.packs.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return quest_library.packs[a].official > quest_library.packs[b].official; });
    if (!order.empty()) group_begin(p, "Packs");
    for (size_t pi : order) {
        const quests::Pack& pk = quest_library.packs[pi];
        const quests::PackMode pm = pack_mode(pk);
        int m = static_cast<int>(pm);
        static const char* const kModes[] = {"All", "Choose", "Off"};
        std::string value = kModes[m];
        if (pm == quests::PackMode::All) value += " (" + std::to_string(pk.quests.size()) + ")";
        ImGui::PushID(pk.key.c_str());
        if (cycle(value_row(p, pk.name.c_str(), value, pk.description.c_str()), m, 3)) {
            pack_modes[pk.key] = static_cast<quests::PackMode>(m);
            save_pack_modes();
            quests_stale = true;  // applied when the list is read again (next frame)
            toast(pk.name + (m == 0 ? ": every quest installed" : m == 2 ? ": removed" : ": choose quests one by one"));
        }
        if (p.sel_title == pk.name.c_str()) p.sel_image = pk.image;
        ImGui::PopID();
        if (pm == quests::PackMode::Choose)
            for (size_t i : pk.quests) quest_row(quest_library.quests[i], 24 * scale);
    }
    if (!order.empty()) group_end(p);
    bool loose_head = false;
    for (const auto& q : quest_library.quests)
        if (q.pack < 0) {
            if (!loose_head) group_begin(p, "Single quests");
            loose_head = true;
            quest_row(q, 0);
        }
    if (loose_head) group_end(p);
    bool save_head = false;
    for (const auto& q : quests_installed)  // in the save but not in the folder: can still be removed
        if (std::none_of(quest_library.quests.begin(), quest_library.quests.end(), [&](const quests::Quest& l) { return l.name == q.name; })) {
            if (!save_head) group_begin(p, "Only in your save");
            save_head = true;
            ImGui::PushID(q.name.c_str());
            if (button_row(p, q.title.c_str(), q.description.c_str(), true, "Remove")) {
                act = &q;
                act_remove = true;
            }
            ImGui::PopID();
        }
    if (save_head) group_end(p);
    if (!act) return;
    std::string err;
    const std::string title = act->title;
    stale_in_game = false;
    if (change_quest(*act, act_remove, err)) {
        g_sound.play(Sfx::Apply);
        toast((act_remove ? "Removed " : "Installed ") + title + (stale_in_game ? " - reload your save to see it" : ""));
    } else {
        g_sound.play(Sfx::Back);
        toast("Couldn't " + std::string(act_remove ? "remove " : "install ") + title + ": " + err);
    }
    quests_stale = true;  // re-read next frame (the rows above point into the lists)
}

// INPUT: layout and mouse options, then one row per game button / stick direction with what the keyboard + mouse and
// the controller have bound to it. Choosing a cell waits for the next press (Esc cancels, Delete clears); every change
// is saved to input.ini at once and used by the game straight away.
void Launcher::Impl::draw_tab_input(PanelLayout& p, float table_w) {
    input::ControlOptions o = input::control_options();
    static const std::vector<float> kSens = {0.025f, 0.05f, 0.08f, 0.12f};
    static const std::vector<std::string> kSensNames = {"Low", "Medium", "High", "Very high"};
    int layout = o.nintendo_layout ? 1 : 0, sens = 0;
    for (size_t i = 0; i < kSens.size(); ++i)
        if (std::abs(kSens[i] - o.mouse_sensitivity) < std::abs(kSens[static_cast<size_t>(sens)] - o.mouse_sensitivity)) sens = static_cast<int>(i);
    group_begin(p, "Controller and mouse");
    bool changed = cycle(value_row(p, "Controller layout", layout ? "Nintendo (by position)" : "Xbox (A / Cross confirms)", nullptr), layout, 2);
    changed |= cycle(value_row(p, "Mouse sensitivity", kSensNames[static_cast<size_t>(sens)], nullptr), sens, 4);
    changed |= switch_row(p, "Invert camera Y", o.invert_y, nullptr);
    group_end(p);
    if (changed) {
        o.nintendo_layout = layout == 1;
        o.mouse_sensitivity = kSens[static_cast<size_t>(sens)];
        input::set_control_options(o);
        input::save_bindings();
    }
    const bool waiting = capturing && std::chrono::steady_clock::now() - capture_since < std::chrono::seconds(6);
    if (!waiting) capturing = false;
    const float s = scale, col_key = table_w * 0.34f, col_pad = table_w * 0.67f;
    group_begin(p, "Bindings");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    {  // the column titles, inside the card
        const float y = ImGui::GetCursorScreenPos().y + 14 * s;
        dl->AddText(fonts.note, fonts.note->FontSize, ImVec2(p.x + 16 * s, y), kDim, "Game button");
        dl->AddText(fonts.note, fonts.note->FontSize, ImVec2(p.x + col_key, y), kDim, "Keyboard / mouse");
        dl->AddText(fonts.note, fonts.note->FontSize, ImVec2(p.x + col_pad, y), kDim, "Controller");
        ImGui::Dummy(ImVec2(1, fonts.note->FontSize + 22 * s));
    }
    for (const input::BindingRow& row : input::binding_rows()) {
        ImGui::PushID(row.target);
        const ImVec2 r(p.x, ImGui::GetCursorScreenPos().y);
        p.seps.push_back(r.y);
        dl->AddText(fonts.body, fonts.body->FontSize, ImVec2(r.x + 16 * s, r.y + (p.row_h - fonts.body->FontSize) * 0.5f), kText, row.label);
        const auto cell = [&](const char* id, float x, float cw, bool pad, bool enabled) {
            const ImVec2 c0(r.x + x - 12 * s, r.y + 5 * s), c1(c0.x + cw, r.y + p.row_h - 5 * s);
            ImGui::SetCursorScreenPos(c0);
            ImGui::BeginDisabled(!enabled);
            const bool chosen = ImGui::InvisibleButton(id, ImVec2(cw, c1.y - c0.y), ImGuiButtonFlags_EnableNav);
            const bool focused = ImGui::IsItemFocused();
            const bool sel = enabled && (ImGui::IsItemHovered() || (focused && !ImGui::IsAnyItemHovered()));
            ImGui::EndDisabled();
            const bool this_one = capturing && capture_target == row.target && capture_pad == pad;
            if (sel || this_one) {
                dl->AddRectFilled(c0, c1, this_one ? IM_COL32(236, 178, 84, 60) : kHover, 8 * s);
                if (this_one || (focused && nav_cursor_visible())) dl->AddRect(c0, c1, kAmber, 8 * s, 0, 2.0f * s);
            }
            std::string text = this_one ? (pad ? "Press a button..." : "Press a key...") : pad ? input::pad_names(row.target) : input::key_names(row.target);
            if (text.empty()) text = "-";
            dl->AddText(fonts.body, fonts.body->FontSize, ImVec2(c0.x + 12 * s, r.y + (p.row_h - fonts.body->FontSize) * 0.5f),
                        this_one ? IM_COL32(255, 220, 150, 255) : enabled ? kSoft : kOff, text.c_str());
            if (chosen && enabled && !capturing) {
                capturing = true;
                capture_pad = pad;
                capture_target = row.target;
                capture_since = std::chrono::steady_clock::now();
            }
        };
        cell("key", col_key, col_pad - col_key - 8 * s, false, true);
        cell("pad", col_pad, table_w - col_pad + 4 * s, true, row.pad);
        ImGui::SetCursorScreenPos(ImVec2(r.x, r.y + p.row_h));
        ImGui::Dummy(ImVec2(1, 0));
        ImGui::PopID();
    }
    group_end(p);
    group_begin(p, nullptr);
    if (button_row(p, "Reset controls to defaults", nullptr)) {
        input::reset_bindings();
        input::save_bindings();
    }
    group_end(p);
    ImGui::PushFont(fonts.note);
    ImGui::SetCursorScreenPos(ImVec2(p.x + 6 * s, ImGui::GetCursorScreenPos().y - 14 * s));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kDim));
    ImGui::TextUnformatted(capturing ? "Esc cancels, Delete clears this binding." : "Choose a cell, then press the key or button for it. F1 and Home stay reserved.");
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

// The on-screen keyboard: whenever the game asks for text (swkbd: character name, chat). A panel at the bottom of the
// window with the text, a counter and the keys; the controller moves over the keys (A types, B deletes, Y space,
// X shift, Start OK), the mouse clicks them, and the physical keyboard types into the same text (host_input).
void Launcher::Impl::draw_keyboard(ImVec2 size) {
    const input::TextEntry ts = input::text_state();
    const size_t max = input::text_max();
    std::u16string text = ts.text;
    const auto type = [&](char16_t c) {
        if (!max || text.size() < max) text += c;
    };
    bool backspace = false, confirm = false, cancel = false;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) backspace = true;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp)) type(u' ');
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft)) kb_shift = !kb_shift;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadStart)) confirm = true;

    static const char* kRows[] = {"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.?"};
    const float gap = 6 * scale, pad = 16 * scale;
    const float kw = std::min(size.x - 40 * scale, 860 * scale), key = (kw - 2 * pad - 9 * gap) / 10.0f;
    const float kh = std::clamp(key * 0.7f, 30 * scale, 52 * scale), field_h = fonts.body->FontSize + 22 * scale;
    const float ph = pad * 2 + field_h + gap * 2 + 5 * kh + 4 * gap + fonts.note->FontSize + gap;
    const ImVec2 a((size.x - kw) * 0.5f, size.y - ph - 24 * scale), b(a.x + kw, a.y + ph);
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilled(ImVec2(0, 0), size, IM_COL32(0, 0, 0, 90));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, kPanelBg, 8.0f);
    dl->AddRect(a, b, kPanelEdge, 8.0f, 0, 1.5f);
    // the text, with a caret and the game's length limit
    const ImVec2 f0(a.x + pad, a.y + pad), f1(b.x - pad, a.y + pad + field_h);
    dl->AddRectFilled(f0, f1, IM_COL32(0, 0, 0, 120), 4.0f);
    dl->AddRect(f0, f1, IM_COL32(236, 178, 84, 120), 4.0f);
    std::string shown;
    for (char16_t c : text) {  // UTF-8 for ImGui (the game's text is within the BMP)
        const uint32_t u = c;
        if (u < 0x80) shown += static_cast<char>(u);
        else if (u < 0x800) shown += {static_cast<char>(0xC0 | (u >> 6)), static_cast<char>(0x80 | (u & 0x3F))};
        else shown += {static_cast<char>(0xE0 | (u >> 12)), static_cast<char>(0x80 | ((u >> 6) & 0x3F)), static_cast<char>(0x80 | (u & 0x3F))};
    }
    const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
    const ImVec2 ts_size = fonts.body->CalcTextSizeA(fonts.body->FontSize, FLT_MAX, 0, shown.c_str());
    const float text_y = f0.y + (field_h - fonts.body->FontSize) * 0.5f;
    dl->AddText(fonts.body, fonts.body->FontSize, ImVec2(f0.x + 12 * scale, text_y), kText, shown.c_str());
    if (std::fmod(t, 1.0f) < 0.6f)
        dl->AddLine(ImVec2(f0.x + 14 * scale + ts_size.x, text_y), ImVec2(f0.x + 14 * scale + ts_size.x, text_y + fonts.body->FontSize), kAmber,
                    2.0f * scale);
    if (max) {
        const std::string count = std::to_string(text.size()) + " / " + std::to_string(max);
        const ImVec2 cs = fonts.note->CalcTextSizeA(fonts.note->FontSize, FLT_MAX, 0, count.c_str());
        dl->AddText(fonts.note, fonts.note->FontSize, ImVec2(f1.x - 12 * scale - cs.x, f0.y + (field_h - cs.y) * 0.5f), kDim, count.c_str());
    }
    // keys
    // the controller button for a key, as printed on the pad (input bindings), drawn as a badge in its corner
    const auto badge = [&](ImVec2 at, float w, const std::string& name) {
        if (name.empty()) return;
        const ImVec2 bs = fonts.note->CalcTextSizeA(fonts.note->FontSize, FLT_MAX, 0, name.c_str());
        const ImVec2 b1(at.x + w - 4 * scale, at.y + 4 * scale + bs.y + 2 * scale), b0(b1.x - bs.x - 10 * scale, at.y + 4 * scale);
        dl->AddRectFilled(b0, b1, IM_COL32(236, 178, 84, 230), (b1.y - b0.y) * 0.5f);
        dl->AddText(fonts.note, fonts.note->FontSize, ImVec2(b0.x + 5 * scale, b0.y + 1 * scale), IM_COL32(28, 22, 14, 255), name.c_str());
    };
    const std::string p_type = input::pad_prompt("a"), p_del = input::pad_prompt("b"), p_space = input::pad_prompt("y"),
                      p_shift = input::pad_prompt("x"), p_ok = input::pad_prompt("plus");
    const auto key_button = [&](const char* id, const char* label, ImVec2 at, float w) {
        ImGui::SetCursorScreenPos(at);
        const bool hit = ImGui::InvisibleButton(id, ImVec2(w, kh), ImGuiButtonFlags_EnableNav);
        const bool sel = ImGui::IsItemHovered() || (ImGui::IsItemFocused() && !ImGui::IsAnyItemHovered());
        dl->AddRectFilled(at, ImVec2(at.x + w, at.y + kh), sel ? IM_COL32(236, 178, 84, 70) : IM_COL32(255, 255, 255, 14), 5.0f);
        dl->AddRect(at, ImVec2(at.x + w, at.y + kh), sel ? IM_COL32(236, 178, 84, 200) : IM_COL32(255, 255, 255, 30), 5.0f);
        const ImVec2 ls = fonts.body->CalcTextSizeA(fonts.body->FontSize, FLT_MAX, 0, label);
        dl->AddText(fonts.body, fonts.body->FontSize, ImVec2(at.x + (w - ls.x) * 0.5f, at.y + (kh - ls.y) * 0.5f), kText, label);
        return hit;
    };
    float y = f1.y + gap * 2;
    if (refocus) {
        ImGui::SetKeyboardFocusHere();
        refocus = false;
    }
    for (int r = 0; r < 4; ++r) {
        for (int i = 0; i < 10; ++i) {
            char c = kRows[r][i];
            if (kb_shift && c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
            const char label[2] = {c, 0};
            char id[8];
            std::snprintf(id, sizeof id, "k%d_%d", r, i);
            if (key_button(id, label, ImVec2(a.x + pad + i * (key + gap), y), key)) type(static_cast<char16_t>(c));
        }
        y += kh + gap;
    }
    // bottom row in key units: SHIFT 2, SPACE 3, DELETE 2, CANCEL 1.5, OK 1.5
    const float unit = (kw - 2 * pad - 9 * gap) / 10.0f;
    float x = a.x + pad;
    const auto wide = [&](float units) { return unit * units + gap * (units - 1); };
    if (key_button("shift", kb_shift ? "abc" : "ABC", ImVec2(x, y), wide(2))) kb_shift = !kb_shift;
    badge(ImVec2(x, y), wide(2), p_shift);
    x += wide(2) + gap;
    if (key_button("space", "SPACE", ImVec2(x, y), wide(3))) type(u' ');
    badge(ImVec2(x, y), wide(3), p_space);
    x += wide(3) + gap;
    if (key_button("delete", "DELETE", ImVec2(x, y), wide(2))) backspace = true;
    badge(ImVec2(x, y), wide(2), p_del);
    x += wide(2) + gap;
    if (key_button("cancel", "CANCEL", ImVec2(x, y), wide(1.5f))) cancel = true;
    x += wide(1.5f) + gap;
    if (key_button("ok", "OK", ImVec2(x, y), b.x - pad - x)) confirm = true;
    badge(ImVec2(x, y), b.x - pad - x, p_ok);
    y += kh + gap;
    // the hint line, in the controller's own button names (keyboard / mouse when none is connected)
    std::string hint;
    for (const auto& [name, what] : {std::pair{p_type, "type"}, {p_del, "delete"}, {p_space, "space"}, {p_shift, "shift"}, {p_ok, "OK"}})
        if (!name.empty()) hint += (hint.empty() ? "" : "     ") + name + "  " + what;
    if (hint.empty()) hint = "Type on your keyboard or click the keys.   Enter OK   Esc cancel";
    dl->AddText(fonts.note, fonts.note->FontSize, ImVec2(a.x + pad, y), kDim, hint.c_str());

    if (backspace && !text.empty()) text.pop_back();
    if (text != ts.text) input::set_text(text);
    if (confirm) input::finish_text(true);
    else if (cancel) input::finish_text(false);
}

}  // namespace mh3u::boot
