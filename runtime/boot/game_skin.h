#pragma once
// The game's own GUI textures for Cha-Cha's menus: a few sheets read from the player's dump at start (nothing is
// shipped or written to disk), decoded and uploaded as ImGui textures, and the pieces of them the menus are drawn with
// (frames, bars, banners, the cursor, the parchment page, the controller glyphs). Without a game (first start) or if a
// sheet cannot be read, ok() is false and the menus draw themselves as before.
#include <imgui.h>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "game_image.h"
#include "launcher_image.h"
#include "skin_decode.h"
#include "presenter.h"

namespace mh3u::boot {


class GameSkin {
public:
    enum Sheet : uint8_t { Common, Buttons, StartMenu, Wood, SheetCount };
    // A piece of a sheet, in the sheet's pixels (level 0; Wood is loaded at half size, its rectangles are in those).
    struct Sprite {
        Sheet sheet;
        float x0, y0, x1, y1;
        float w() const { return x1 - x0; }
        float h() const { return y1 - y0; }
    };

    // Reads the sheets from the game's content; all or nothing. Needs the ImGui Vulkan backend.
    bool load(const fs::GameImage& game, const gfx::VulkanContext& vk, VkSampler sampler);
    void destroy(VkDevice dev);  // after the GPU is done with them
    bool ok() const { return ok_; }

    // drawing (no-ops while !ok())
    void image(ImDrawList* dl, const Sprite& s, ImVec2 a, ImVec2 b, ImU32 tint = IM_COL32_WHITE, bool flip_x = false) const;
    // a horizontal bar: the end caps keep their aspect (cap widths in sheet pixels, scaled with the bar's height),
    // the middle stretches
    void bar(ImDrawList* dl, const Sprite& s, ImVec2 a, ImVec2 b, float cap_l, float cap_r, ImU32 tint = IM_COL32_WHITE) const;
    // a frame: corners of `border` sheet pixels drawn `edge` pixels big, edges stretched, the middle drawn if centre
    void frame(ImDrawList* dl, const Sprite& s, ImVec2 a, ImVec2 b, float border, float edge, ImU32 tint = IM_COL32_WHITE,
               bool centre = false) const;
    // fills a..b with the sprite cropped to that aspect (no stretching), anchored in the middle
    void cover(ImDrawList* dl, const Sprite& s, ImVec2 a, ImVec2 b, ImU32 tint = IM_COL32_WHITE) const;

    // A controller button drawn with the game's glyph (A B X Y + - L R ZL ZR, d-pad, sticks) if the name is one of
    // those; returns the drawn width, 0 if there is no glyph for it (draw=false measures).
    float glyph(ImDrawList* dl, const std::string& name, ImVec2 left_centre, float height, bool draw = true) const;

private:
    void uv(const Sprite& s, float x0, float y0, float x1, float y1, ImVec2& u0, ImVec2& u1) const;
    std::array<LauncherImage, SheetCount> sheets_{};
    bool ok_ = false;
};

// The pieces Cha-Cha's menus use (rectangles measured on the US v32 sheets).
namespace skin {
using S = GameSkin::Sprite;
// GUI\Texture\common\td_com_ID_eng (1024x1024)
inline constexpr S kBandCross{GameSkin::Common, 4, 4, 464, 52};          // dark band, woven diagonal pattern
inline constexpr S kBandDiamond{GameSkin::Common, 4, 112, 464, 160};     // dark band, diamonds
inline constexpr S kBandShort{GameSkin::Common, 416, 688, 640, 728};     // dark band, diamonds, rounded right end
inline constexpr S kBannerTan{GameSkin::Common, 8, 332, 252, 376};       // tan bar, red left edge, scroll right end
inline constexpr S kBarWeave{GameSkin::Common, 26, 740, 212, 780};       // brown cross-hatched bar
inline constexpr S kFrameGold{GameSkin::Common, 472, 149, 556, 228};     // gold-trimmed box (black inside)
inline constexpr S kFrameOrnate{GameSkin::Common, 656, 853, 1016, 916};  // white outline, scroll ends (tinted)
inline constexpr S kCursor{GameSkin::Common, 476, 109, 516, 144};        // white chevron with a black rim
inline constexpr S kChevron{GameSkin::Common, 532, 109, 560, 144};       // thin white chevron
inline constexpr S kGauge{GameSkin::Common, 320, 389, 504, 448};         // light gauge bar in a dark rim
inline constexpr S kParchmentWide{GameSkin::Common, 632, 414, 1020, 616};
// GUI\Texture\startmenu\st_M_menu_01_ID_eng (2048x2048)
inline constexpr S kTitleScroll{GameSkin::StartMenu, 4, 0, 500, 192};    // scroll with a tan diamond band
inline constexpr S kPage{GameSkin::StartMenu, 1677, 6, 2036, 628};       // parchment page, red leather right / bottom
// GUI\Texture\startmenu\st_menu_04 at half size (1024x512): carved wood
inline constexpr S kWood{GameSkin::Wood, 0, 0, 1024, 512};
}  // namespace skin

}  // namespace mh3u::boot
