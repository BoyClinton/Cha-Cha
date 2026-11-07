#include "game_skin.h"

#include "overlay_fs.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace mh3u::boot {

// ---- the sheets ----------------------------------------------------------------------------------------------------
namespace {

constexpr uint32_t kTextureType = 0x241F5DEB;  // rTexture (docs/formats/arc.md: type hash)

struct SheetSource {
    const char* arc;    // under /vol/content
    const char* entry;  // path in the archive
    int level;          // mip level to load
};
// The smallest archives holding each sheet (US English); the update's copy wins where it has one.
constexpr SheetSource kSources[GameSkin::SheetCount] = {
    {"nativeCafe/arc/ID/ID_cock_eng.arc", "GUI\\Texture\\common\\td_com_ID_eng", 0},
    {"nativeCafe/arc/ID/ID_cock_eng.arc", "GUI\\Texture\\common\\td_bu_ID", 0},
    {"nativeCafe/arc/ID/ID_dataTrans_eng.arc", "GUI\\Texture\\startmenu\\st_M_menu_01_ID_eng", 0},
    {"nativeCafe/arc/ID/ID_dataTrans_eng.arc", "GUI\\Texture\\startmenu\\st_menu_04", 1},  // 2048x1024 -> 1024x512
};

bool read_file(const fs::OverlayFs& vfs, const std::string& path, std::vector<uint8_t>& out) {
    auto f = vfs.open("/vol/content/" + path);
    if (!f) return false;
    out.resize(static_cast<size_t>(f->size()));
    return f->read(0, out.data(), out.size()) == out.size();
}

}  // namespace

bool GameSkin::load(const fs::GameImage& game, const gfx::VulkanContext& vk, VkSampler sampler) {
    if (ok_) return true;
    const auto t0 = std::chrono::steady_clock::now();
    const fs::OverlayFs vfs = game.make_fs({});
    std::vector<uint8_t> arc, tex, rgba;
    std::string arc_name;
    std::array<LauncherImage, SheetCount> loaded{};
    bool all = true;
    for (int i = 0; i < SheetCount && all; ++i) {
        const SheetSource& s = kSources[i];
        uint32_t w = 0, h = 0;
        if (arc_name != s.arc) {
            arc_name = s.arc;
            if (!read_file(vfs, s.arc, arc)) arc.clear();
        }
        all = skin_detail::arc_entry(arc, s.entry, kTextureType, tex) && skin_detail::tex_rgba(tex, s.level, rgba, w, h) &&
              upload_image(vk, rgba, w, h, sampler, loaded[static_cast<size_t>(i)]);
        if (!all) std::fprintf(stderr, "menus: could not read %s from %s; drawing them without the game's textures\n", s.entry, s.arc);
    }
    if (!all) {
        for (auto& img : loaded) destroy_image(vk.device, img);
        return false;
    }
    sheets_ = loaded;
    ok_ = true;
    std::printf("menus: the game's GUI textures loaded in %.0f ms\n",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    return true;
}

void GameSkin::destroy(VkDevice dev) {
    for (auto& img : sheets_) destroy_image(dev, img);
    ok_ = false;
}

void GameSkin::uv(const Sprite& s, float x0, float y0, float x1, float y1, ImVec2& u0, ImVec2& u1) const {
    const LauncherImage& img = sheets_[s.sheet];
    u0 = ImVec2(x0 / img.w, y0 / img.h);
    u1 = ImVec2(x1 / img.w, y1 / img.h);
}

void GameSkin::image(ImDrawList* dl, const Sprite& s, ImVec2 a, ImVec2 b, ImU32 tint, bool flip_x) const {
    if (!ok_) return;
    ImVec2 u0, u1;
    uv(s, s.x0, s.y0, s.x1, s.y1, u0, u1);
    if (flip_x) std::swap(u0.x, u1.x);
    dl->AddImage(sheets_[s.sheet].id(), a, b, u0, u1, tint);
}

void GameSkin::bar(ImDrawList* dl, const Sprite& s, ImVec2 a, ImVec2 b, float cap_l, float cap_r, ImU32 tint) const {
    if (!ok_) return;
    const float k = (b.y - a.y) / s.h();
    float l = cap_l * k, r = cap_r * k;
    if (l + r > b.x - a.x) {  // too short for both caps: shrink them
        const float f = (b.x - a.x) / (l + r);
        l *= f;
        r *= f;
    }
    const ImTextureID id = sheets_[s.sheet].id();
    ImVec2 u0, u1;
    uv(s, s.x0, s.y0, s.x0 + cap_l, s.y1, u0, u1);
    dl->AddImage(id, a, ImVec2(a.x + l, b.y), u0, u1, tint);
    uv(s, s.x0 + cap_l, s.y0, s.x1 - cap_r, s.y1, u0, u1);
    dl->AddImage(id, ImVec2(a.x + l, a.y), ImVec2(b.x - r, b.y), u0, u1, tint);
    uv(s, s.x1 - cap_r, s.y0, s.x1, s.y1, u0, u1);
    dl->AddImage(id, ImVec2(b.x - r, a.y), b, u0, u1, tint);
}

void GameSkin::frame(ImDrawList* dl, const Sprite& s, ImVec2 a, ImVec2 b, float border, float edge, ImU32 tint, bool centre) const {
    if (!ok_) return;
    const ImTextureID id = sheets_[s.sheet].id();
    const float sx[4] = {s.x0, s.x0 + border, s.x1 - border, s.x1}, sy[4] = {s.y0, s.y0 + border, s.y1 - border, s.y1};
    const float dx[4] = {a.x, a.x + edge, b.x - edge, b.x}, dy[4] = {a.y, a.y + edge, b.y - edge, b.y};
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i) {
            if (i == 1 && j == 1 && !centre) continue;
            ImVec2 u0, u1;
            uv(s, sx[i], sy[j], sx[i + 1], sy[j + 1], u0, u1);
            dl->AddImage(id, ImVec2(dx[i], dy[j]), ImVec2(dx[i + 1], dy[j + 1]), u0, u1, tint);
        }
}

void GameSkin::cover(ImDrawList* dl, const Sprite& s, ImVec2 a, ImVec2 b, ImU32 tint) const {
    if (!ok_ || b.x <= a.x || b.y <= a.y) return;
    const float want = (b.x - a.x) / (b.y - a.y), have = s.w() / s.h();
    float x0 = s.x0, x1 = s.x1, y0 = s.y0, y1 = s.y1;
    if (want > have) {  // wider than the sprite: crop its height
        const float ch = s.w() / want, my = (s.y0 + s.y1) * 0.5f;
        y0 = my - ch * 0.5f;
        y1 = my + ch * 0.5f;
    } else {
        const float cw = s.h() * want, mx = (s.x0 + s.x1) * 0.5f;
        x0 = mx - cw * 0.5f;
        x1 = mx + cw * 0.5f;
    }
    ImVec2 u0, u1;
    uv(s, x0, y0, x1, y1, u0, u1);
    dl->AddImage(sheets_[s.sheet].id(), a, b, u0, u1, tint);
}

float GameSkin::glyph(ImDrawList* dl, const std::string& name, ImVec2 at, float height, bool draw) const {
    if (!ok_) return 0;
    // GUI\Texture\common\td_bu_ID: the Pro Controller row (capital letters); the names are input::pad_prompt's for
    // Xbox-style and Nintendo pads (PlayStation symbols and Xbox's LB / Menu... have no glyph here)
    struct G {
        const char* name;
        Sprite s;
    };
    static constexpr G kGlyphs[] = {
        {"A", {Buttons, 76, 124, 108, 156}},  {"B", {Buttons, 134, 124, 168, 156}}, {"X", {Buttons, 194, 124, 228, 156}},
        {"Y", {Buttons, 254, 124, 288, 156}}, {"-", {Buttons, 314, 124, 348, 156}}, {"+", {Buttons, 374, 124, 408, 156}},
        {"L", {Buttons, 124, 168, 176, 196}}, {"R", {Buttons, 184, 168, 236, 196}}, {"ZL", {Buttons, 254, 164, 288, 196}},
        {"ZR", {Buttons, 314, 164, 348, 196}}, {"DPAD", {Buttons, 12, 124, 48, 160}},
    };
    for (const G& g : kGlyphs) {
        if (name != g.name) continue;
        const float w = height * g.s.w() / g.s.h();
        if (draw) image(dl, g.s, ImVec2(at.x, at.y - height * 0.5f), ImVec2(at.x + w, at.y + height * 0.5f));
        return w;
    }
    return 0;
}

}  // namespace mh3u::boot
