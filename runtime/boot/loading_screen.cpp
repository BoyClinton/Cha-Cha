// The loading screen (loading_screen.h): a brown box with a thin gold frame in the middle of the screen (the game's
// "Saving..." box, 730 x 280 of a 2000 x 1125 screen, in brown), holding "Compiling shaders...", the progress bar in
// the pink-purple of the game's spinner dots, and the percentage. The text is a small built-in bitmap font (5 x 7, with
// descenders), drawn 3x: the game's own font is not loaded yet.
#include "loading_screen.h"

#include "presenter.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace mh3u::boot {
namespace {

constexpr int kW = 1280, kH = 720;  // the image, scaled to the window by the presenter
constexpr float kBoxX0 = 406, kBoxX1 = 874, kBoxY0 = 280, kBoxY1 = 440;
constexpr float kCx = kW * 0.5f;
constexpr float kTextY = 310, kBarY = 357, kPercentY = 386;  // tops
constexpr float kBarW = 300, kBarH = 6;
constexpr float kScale = 3;  // font pixels

constexpr uint32_t kPurple[3] = {205, 110, 212};   // sampled from the game's spinner dots
constexpr uint32_t kGold[3] = {150, 120, 54};      // the box's frame
constexpr uint32_t kBrown[3] = {62, 44, 29};       // the box
constexpr uint32_t kCream[3] = {240, 228, 200};    // the text

// 5 x 9 glyphs: rows top to bottom, bit 4 = left column; rows 7-8 are below the baseline
struct Glyph {
    char c;
    uint8_t rows[9];
};
constexpr Glyph kFont[] = {
    {'C', {14, 17, 16, 16, 16, 17, 14, 0, 0}},  {'o', {0, 0, 14, 17, 17, 17, 14, 0, 0}},
    {'m', {0, 0, 26, 21, 21, 21, 21, 0, 0}},    {'p', {0, 0, 30, 17, 17, 17, 30, 16, 16}},
    {'i', {4, 0, 12, 4, 4, 4, 14, 0, 0}},       {'l', {12, 4, 4, 4, 4, 4, 14, 0, 0}},
    {'n', {0, 0, 22, 25, 17, 17, 17, 0, 0}},    {'g', {0, 0, 15, 17, 17, 17, 15, 1, 14}},
    {'s', {0, 0, 15, 16, 14, 1, 30, 0, 0}},     {'h', {16, 16, 22, 25, 17, 17, 17, 0, 0}},
    {'a', {0, 0, 14, 1, 15, 17, 15, 0, 0}},     {'d', {1, 1, 13, 19, 17, 17, 15, 0, 0}},
    {'e', {0, 0, 14, 17, 31, 16, 14, 0, 0}},    {'r', {0, 0, 22, 25, 16, 16, 16, 0, 0}},
    {'.', {0, 0, 0, 0, 0, 12, 12, 0, 0}},       {'%', {24, 25, 2, 4, 8, 19, 3, 0, 0}},
    {'0', {14, 17, 19, 21, 25, 17, 14, 0, 0}},  {'1', {4, 12, 4, 4, 4, 4, 14, 0, 0}},
    {'2', {14, 17, 1, 2, 4, 8, 31, 0, 0}},      {'3', {31, 2, 4, 2, 1, 17, 14, 0, 0}},
    {'4', {2, 6, 10, 18, 31, 2, 2, 0, 0}},      {'5', {31, 16, 30, 1, 1, 17, 14, 0, 0}},
    {'6', {6, 8, 16, 30, 17, 17, 14, 0, 0}},    {'7', {31, 1, 2, 4, 8, 8, 8, 0, 0}},
    {'8', {14, 17, 17, 14, 17, 17, 14, 0, 0}},  {'9', {14, 17, 17, 15, 1, 2, 12, 0, 0}},
};
constexpr float kAdvance = 6 * kScale;

// RGBA8 in memory (R first), as present_image takes it
void blend(uint32_t& px, const uint32_t c[3], float a) {
    if (a <= 0) return;
    a = std::min(a, 1.0f);
    const auto mix = [&](int shift, uint32_t to) {
        const float from = static_cast<float>((px >> shift) & 0xFF);
        return static_cast<uint32_t>(from + (static_cast<float>(to) - from) * a + 0.5f) << shift;
    };
    px = mix(0, c[0]) | mix(8, c[1]) | mix(16, c[2]) | 0xFF000000u;
}

void rect(std::vector<uint32_t>& img, float x0, float y0, float x1, float y1, const uint32_t c[3], float alpha) {
    for (int y = std::max(0, static_cast<int>(y0)); y < std::min(kH, static_cast<int>(std::ceil(y1))); ++y)
        for (int x = std::max(0, static_cast<int>(x0)); x < std::min(kW, static_cast<int>(std::ceil(x1))); ++x) {
            const float cover = std::clamp(std::min(static_cast<float>(x) + 1 - x0, x1 - static_cast<float>(x)), 0.0f, 1.0f);
            blend(img[static_cast<size_t>(y) * kW + static_cast<size_t>(x)], c, cover * alpha);
        }
}

// one line of text centred on kCx, its top at y
void text(std::vector<uint32_t>& img, const std::string& s, float y) {
    float x = std::round(kCx - (static_cast<float>(s.size()) * kAdvance - kScale) / 2);
    for (const char ch : s) {
        for (const Glyph& g : kFont) {
            if (g.c != ch) continue;
            for (int row = 0; row < 9; ++row)
                for (int col = 0; col < 5; ++col)
                    if (g.rows[row] & (16 >> col))
                        rect(img, x + col * kScale, y + row * kScale, x + (col + 1) * kScale, y + (row + 1) * kScale, kCream, 1.0f);
        }
        x += kAdvance;
    }
}

}  // namespace

LoadingScreen::LoadingScreen(gfx::Presenter& presenter)
    : presenter_(presenter), pixels_(static_cast<size_t>(kW) * kH), start_(std::chrono::steady_clock::now()) {}

void LoadingScreen::frame(float progress) {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_ < std::chrono::milliseconds(16)) return;
    last_ = now;
    std::fill(pixels_.begin(), pixels_.end(), 0xFF000000u);
    // the box: gold frame, brown inside
    rect(pixels_, kBoxX0, kBoxY0, kBoxX1, kBoxY1, kGold, 1.0f);
    rect(pixels_, kBoxX0 + 2, kBoxY0 + 2, kBoxX1 - 2, kBoxY1 - 2, kBrown, 1.0f);
    text(pixels_, "Compiling shaders...", kTextY);
    // the bar: a dim track, filled from the left; the percentage under it (none while the total is unknown)
    const float x0 = kCx - kBarW / 2;
    rect(pixels_, x0, kBarY, x0 + kBarW, kBarY + kBarH, kPurple, 0.22f);
    if (progress >= 0) {
        const float p = std::clamp(progress, 0.0f, 1.0f);
        rect(pixels_, x0, kBarY, x0 + kBarW * p, kBarY + kBarH, kPurple, 1.0f);
        text(pixels_, std::to_string(static_cast<int>(p * 100)) + "%", kPercentY);
    }
    presenter_.present_image(reinterpret_cast<const uint8_t*>(pixels_.data()), kW, kH);
}

}  // namespace mh3u::boot
