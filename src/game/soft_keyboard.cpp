// The on-screen keyboard (soft_keyboard.h). Built like the settings pages (settings_page.cpp): cGUIObjPolygon copies
// for the panel and the keys (vertex colours AABBGGRR), cGUIObjMessage copies for the texts, drawn through the last
// GUI instance the game draws in a frame (so the keyboard is over everything), each with its own copy (draws are
// deferred). The copies' templates come from the screen under it: a polygon, and the message lowest on the screen
// (the help line, plain white text). Input: the pad words of sAppPad right after the pad update (then cleared, so the
// screen under the keyboard sees no presses) and the menu pointer (menu_mouse.h, set_overlay_hook).
#include "soft_keyboard.h"

#include "gui_util.h"
#include "menu_mouse.h"
#include "guest_memory.h"
#include "ppc_context.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace mh3u::game {
namespace {

constexpr uint32_t kPolygonVtable = 0x100A7E50, kPolygonSize = 0xE0;
// title-menu sounds (the start menu's screens, as settings_page.cpp plays them): cursor move, choose
constexpr uint32_t kMoveSound = 2, kDecideSound = 0xD, kSoundF1 = 0x1006EBA0, kSoundF2 = 0x1006EB54;
// the buttons (VPAD bits, as the game would have read them: host input's bindings and the input script)
constexpr uint32_t kBtnA = 0x8000, kBtnB = 0x4000, kBtnX = 0x2000, kBtnY = 0x1000, kBtnPlus = 0x8;
constexpr uint32_t kBtnUp = 0x200 | 0x10000000, kBtnDown = 0x100 | 0x08000000, kBtnRight = 0x400 | 0x20000000,
                   kBtnLeft = 0x800 | 0x40000000;
constexpr int kRepeatDelay = 18, kRepeatEvery = 5;  // frames

// layout, TV pixels (1920 x 1080): the lower third, ten keys a row. The game's notice over the text entry (drawn after
// every GUI instance, so over anything of ours) ends at y 714 on the name entry: the keyboard stays under it.
constexpr float kPanelX0 = 300, kPanelX1 = 1620, kPanelY0 = 726, kPad = 14, kGap = 7;
constexpr float kFieldH = 48, kKeyH = 46;
constexpr float kKeyW = (kPanelX1 - kPanelX0 - 2 * kPad - 9 * kGap) / 10;
constexpr float kFieldY0 = kPanelY0 + kPad, kKeysY0 = kFieldY0 + kFieldH + 2 * kGap;
constexpr float kPanelY1 = kKeysY0 + 5 * kKeyH + 4 * kGap + kPad;
// the template text: the width of one character and where its top sits against the object's y (measured)
float g_char_w = 15.0f, g_text_dy = -4.0f;

constexpr const char* kRows[4] = {"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.?"};
enum Special { kShift = 40, kSpace, kDelete, kOk, kKeys };
// the bottom row in key units: Shift 2, Space 4, Delete 2, OK 2
constexpr float kBottomUnits[4] = {2, 4, 2, 2};

// AABBGGRR
constexpr uint32_t kDim = 0x70000000, kShadow = 0x40000000, kOutline = 0xFF000000, kRim = 0xFF26374B;
constexpr uint32_t kFaceTop = 0xF61E2A36, kFaceBottom = 0xF60E161E, kFieldFill = 0xC0000000;
constexpr uint32_t kKeyTop = 0xFF2C3A48, kKeyBottom = 0xFF1C2630, kKeySelTop = 0xFF2C9FE8, kKeySelBottom = 0xFF1A78C0;
constexpr uint32_t kCaret = 0xFFE0E0E0;

TextIo g_io;
bool g_open = false, g_shift = false;
std::u16string g_text;
size_t g_max = 0;
int g_cursor = 11;  // 'q'
HoverWalk g_walk;
bool g_probe = false;

uint32_t g_prev_buttons = 0;
int g_held_frames = 0;
// GUI copies: made again for every entry (the screen under it, and the objects the copies point into, change), but
// the polygon template, a copy of the first polygon the game draws anywhere (not every screen has one), is kept
uint32_t g_poly_src = 0;
uint32_t g_msg_src = 0;                  // the template message: the lowest one of the last whole frame drawn
uint32_t g_msg_best = 0;                 // this frame's lowest so far
uint32_t g_msg_inst = 0, g_msg_best_inst = 0;  // their instances: the keyboard draws through it (on the TV)
float g_msg_best_y = -1;
uint32_t g_msg_frame = 0;
std::vector<uint32_t> g_rects;           // polygon copies, by slot
std::array<uint32_t, kKeys> g_labels{};  // key label copies
uint32_t g_field = 0, g_count = 0;
std::string g_field_shown, g_count_shown;
bool g_labels_shift = false;
std::map<std::string, uint32_t> g_strings;

uint32_t str(const std::string& s) {
    auto it = g_strings.find(s);
    if (it == g_strings.end()) it = g_strings.emplace(s, gui::guest_string(s)).first;
    return it->second;
}

std::string utf8(const std::u16string& t) {
    std::string s;
    for (char16_t c : t) {
        const uint32_t u = c;
        if (u < 0x80) s += static_cast<char>(u);
        else if (u < 0x800) s += {static_cast<char>(0xC0 | (u >> 6)), static_cast<char>(0x80 | (u & 0x3F))};
        else s += {static_cast<char>(0xE0 | (u >> 12)), static_cast<char>(0x80 | ((u >> 6) & 0x3F)), static_cast<char>(0x80 | (u & 0x3F))};
    }
    return s;
}

std::string key_label(int k, bool shift) {
    if (k < 40) {
        char c = kRows[k / 10][k % 10];
        if (shift && c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        return std::string(1, c);
    }
    switch (k) {
        case kShift: return shift ? "X  abc" : "X  ABC";  // with the button that does it
        case kSpace: return "Y  Space";
        case kDelete: return "B  Delete";
        default: return "+  OK";
    }
}

struct Box {
    float x0, y0, x1, y1;
    bool has(float x, float y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
};
Box key_box(int k) {
    if (k < 40) {
        const float x = kPanelX0 + kPad + static_cast<float>(k % 10) * (kKeyW + kGap), y = kKeysY0 + static_cast<float>(k / 10) * (kKeyH + kGap);
        return {x, y, x + kKeyW, y + kKeyH};
    }
    float x = kPanelX0 + kPad;
    for (int i = 0; i < k - 40; ++i) x += kBottomUnits[i] * kKeyW + kBottomUnits[i] * kGap;
    const float w = kBottomUnits[k - 40] * kKeyW + (kBottomUnits[k - 40] - 1) * kGap, y = kKeysY0 + 4 * (kKeyH + kGap);
    return {x, y, x + w, y + kKeyH};
}
int key_at(float x, float y) {
    for (int k = 0; k < kKeys; ++k)
        if (key_box(k).has(x, y)) return k;
    return -1;
}

// the d-pad over the keys: rows of 10, the bottom row's 4 keys under their columns
int step_cursor(int k, uint32_t dirs) {
    const auto col_of = [](int key) {  // the column a key covers (its left one)
        if (key < 40) return key % 10;
        int c = 0;
        for (int i = 0; i < key - 40; ++i) c += static_cast<int>(kBottomUnits[i]);
        return c;
    };
    const auto bottom_at = [](int col) {
        int c = 0;
        for (int i = 0; i < 4; ++i) {
            c += static_cast<int>(kBottomUnits[i]);
            if (col < c) return 40 + i;
        }
        return 43;
    };
    if (dirs & 4) k = k < 40 ? k / 10 * 10 + (k % 10 + 9) % 10 : 40 + (k - 40 + 3) % 4;
    else if (dirs & 8) k = k < 40 ? k / 10 * 10 + (k % 10 + 1) % 10 : 40 + (k - 40 + 1) % 4;
    else if (dirs & 1) k = k < 10 ? bottom_at(k) : k < 40 ? k - 10 : 30 + col_of(k);
    else if (dirs & 2) k = k < 30 ? k + 10 : k < 40 ? bottom_at(k % 10) : col_of(k);
    return k;
}

void press(int k) {
    if (k < 40) {
        if (!g_max || g_text.size() < g_max) g_text += static_cast<char16_t>(key_label(k, g_shift)[0]);
        if (g_io.set) g_io.set(g_text);
    } else if (k == kShift) {
        g_shift = !g_shift;
    } else if (k == kSpace) {
        if (!g_max || g_text.size() < g_max) g_text += u' ';
        if (g_io.set) g_io.set(g_text);
    } else if (k == kDelete) {
        if (!g_text.empty()) g_text.pop_back();
        if (g_io.set) g_io.set(g_text);
    } else if (g_io.finish) {
        g_io.finish(true);
    }
}

bool active() {
    std::u16string text;
    size_t max = 0;
    const bool waiting = g_io.waiting && g_io.waiting(text, max);
    if (waiting != g_open) {
        g_open = waiting;
        // copies of the last entry's screen are not used again (leaked: a few KB per entry)
        g_msg_src = g_msg_best = g_msg_inst = g_msg_best_inst = 0;
        g_prev_buttons = g_io.buttons ? g_io.buttons() : 0;  // the press that opened it is not a key press
        g_msg_best_y = -1;
        g_rects.clear();
        g_labels.fill(0);
        g_field = g_count = 0;
        g_field_shown.clear(), g_count_shown.clear();
        g_labels_shift = false;
        g_shift = false;
        g_cursor = 11;
        g_walk.reset();
        if (g_io.own_controller) g_io.own_controller(waiting);
        std::printf("soft keyboard: %s (frame %u)\n", waiting ? "up" : "down", menu_frame());
    }
    if (waiting) g_text = text, g_max = max;
    return waiting;
}

// the copies' texts (outside the draw: set here, in the step)
void update_texts(ppc::Context& ctx, uint8_t* base) {
    if (!g_msg_src) return;
    for (int k = 0; k < kKeys; ++k)
        if (!g_labels[static_cast<size_t>(k)]) g_labels[static_cast<size_t>(k)] = gui::clone_message(ctx, base, g_msg_src, str(key_label(k, g_shift)));
    if (g_labels_shift != g_shift) {
        g_labels_shift = g_shift;
        for (int k = 0; k <= kShift; ++k) gui::set_clone_text(ctx, base, g_labels[static_cast<size_t>(k)], str(key_label(k, g_shift)));
    }
    const std::string field = utf8(g_text), count = g_max ? std::to_string(g_text.size()) + "/" + std::to_string(g_max) : "";
    if (!g_field) g_field = gui::clone_message(ctx, base, g_msg_src, str(field.empty() ? " " : field)), g_field_shown = field;
    else if (field != g_field_shown) gui::set_clone_text(ctx, base, g_field, str(field.empty() ? " " : field)), g_field_shown = field;
    if (!g_count) g_count = gui::clone_message(ctx, base, g_msg_src, str(count.empty() ? " " : count)), g_count_shown = count;
    else if (count != g_count_shown) gui::set_clone_text(ctx, base, g_count, str(count.empty() ? " " : count)), g_count_shown = count;
}

void step(ppc::Context& ctx, uint8_t* base) {
    const uint32_t held = g_io.buttons ? g_io.buttons() : 0, pressed = held & ~g_prev_buttons;
    g_prev_buttons = held;
    if (g_probe && pressed) std::printf("soft keyboard: pressed %08X\n", pressed);
    // directions: on the press, then repeating while held
    const uint32_t dir_bits = held & (kBtnUp | kBtnDown | kBtnLeft | kBtnRight);
    g_held_frames = dir_bits ? g_held_frames + 1 : 0;
    const bool repeat = g_held_frames > kRepeatDelay && (g_held_frames - kRepeatDelay) % kRepeatEvery == 0;
    const uint32_t moving = pressed & dir_bits ? pressed & dir_bits : repeat ? dir_bits : 0;
    const int before = g_cursor;
    if (moving)
        g_cursor = step_cursor(g_cursor, (moving & kBtnUp ? 1u : 0u) | (moving & kBtnDown ? 2u : 0u) | (moving & kBtnLeft ? 4u : 0u) |
                                             (moving & kBtnRight ? 8u : 0u));
    bool chosen = pressed & kBtnA;
    if (float x = 0, y = 0; menu_pointer(x, y)) {
        const int hit = key_at(x, y);
        if (const int d = g_walk.step(x, y, hit, g_cursor)) g_cursor += d;
        while (menu_click())
            if (hit >= 0) g_cursor = hit, chosen = true;
    }
    if (g_cursor != before) gui::play_sound(ctx, base, 8, kMoveSound, kSoundF1, kSoundF2);
    if (chosen) {
        gui::play_sound(ctx, base, 8, kDecideSound, kSoundF1, kSoundF2);
        press(g_cursor);
    } else if (pressed & kBtnB) {
        gui::play_sound(ctx, base, 8, kMoveSound, kSoundF1, kSoundF2);
        press(kDelete);
    } else if (pressed & kBtnX) {
        gui::play_sound(ctx, base, 8, kMoveSound, kSoundF1, kSoundF2);
        press(kShift);
    } else if (pressed & kBtnY) {
        gui::play_sound(ctx, base, 8, kMoveSound, kSoundF1, kSoundF2);
        press(kSpace);
    } else if (pressed & kBtnPlus) {
        gui::play_sound(ctx, base, 8, kDecideSound, kSoundF1, kSoundF2);
        press(kOk);
    }
    update_texts(ctx, base);
}

// one of our rects, slot i
void rect(ppc::Context& ctx, uint8_t* base, uint32_t carrier, size_t i, float x0, float y0, float x1, float y1, uint32_t top, uint32_t bottom) {
    if (i >= g_rects.size()) g_rects.resize(i + 1, 0);
    if (!g_rects[i]) g_rects[i] = gui::clone_object(base, g_poly_src, kPolygonSize);
    const uint32_t r = g_rects[i];
    if (!r) return;
    ppc::st32(base, r + 0x10, std::bit_cast<uint32_t>(1.0f));  // no scale / rotation of the original
    ppc::st32(base, r + 0x24, std::bit_cast<uint32_t>(1.0f));
    ppc::st32(base, r + 0x40, std::bit_cast<uint32_t>(x0));
    ppc::st32(base, r + 0x44, std::bit_cast<uint32_t>(y0));
    ppc::st32(base, r + 0xC0, std::bit_cast<uint32_t>(x1 - x0));
    ppc::st32(base, r + 0xC4, std::bit_cast<uint32_t>(y1 - y0));
    ppc::st32(base, r + 0xB0, top), ppc::st32(base, r + 0xB4, top);
    ppc::st32(base, r + 0xB8, bottom), ppc::st32(base, r + 0xBC, bottom);
    gui::draw_object(ctx, base, carrier, r);
}

// a text copy at x, y (its top left), scale times the template's size
float g_base_sx = 1, g_base_sy = 1;  // the template's own scale
constexpr float kTextH = 24;         // the template's capital height at scale 1 (measured), for centring
void text(ppc::Context& ctx, uint8_t* base, uint32_t carrier, uint32_t m, float x, float y, float scale = 1) {
    if (!m) return;
    ppc::st32(base, m + 0x10, std::bit_cast<uint32_t>(g_base_sx * scale));
    ppc::st32(base, m + 0x24, std::bit_cast<uint32_t>(g_base_sy * scale));
    ppc::st32(base, m + 0x40, std::bit_cast<uint32_t>(x));
    ppc::st32(base, m + 0x44, std::bit_cast<uint32_t>(y + g_text_dy * scale));
    gui::draw_object(ctx, base, carrier, m);
}
float text_w(const std::string& s, float scale = 1) { return static_cast<float>(s.size()) * g_char_w * scale; }
constexpr float kKeyScale = 1.2f, kFieldScale = 1.35f;

void draw_keyboard(ppc::Context& ctx, uint8_t* base, uint32_t carrier) {
    size_t n = 0;
    rect(ctx, base, carrier, n++, 0, 0, 1920, 1080, kDim, kDim);  // the screen under it, dimmed
    rect(ctx, base, carrier, n++, kPanelX0 + 6, kPanelY0 + 10, kPanelX1 + 12, kPanelY1 + 16, kShadow, kShadow);
    rect(ctx, base, carrier, n++, kPanelX0 - 6, kPanelY0 - 6, kPanelX1 + 6, kPanelY1 + 6, kOutline, kOutline);
    rect(ctx, base, carrier, n++, kPanelX0 - 2, kPanelY0 - 2, kPanelX1 + 2, kPanelY1 + 2, kRim, kRim);
    rect(ctx, base, carrier, n++, kPanelX0 + 2, kPanelY0 + 2, kPanelX1 - 2, kPanelY1 - 2, kOutline, kOutline);
    rect(ctx, base, carrier, n++, kPanelX0 + 4, kPanelY0 + 4, kPanelX1 - 4, kPanelY1 - 4, kFaceTop, kFaceBottom);
    // the text field, its text, a caret and the count
    const float fx0 = kPanelX0 + kPad, fx1 = kPanelX1 - kPad;
    rect(ctx, base, carrier, n++, fx0, kFieldY0, fx1, kFieldY0 + kFieldH, kFieldFill, kFieldFill);
    rect(ctx, base, carrier, n++, fx0, kFieldY0 + kFieldH - 2, fx1, kFieldY0 + kFieldH, kRim, kRim);
    const float ty = kFieldY0 + (kFieldH - kTextH * kFieldScale) / 2;
    text(ctx, base, carrier, g_field, fx0 + 20, ty, kFieldScale);
    const float caret_x = fx0 + 24 + text_w(g_field_shown, kFieldScale);
    const bool blink = (menu_frame() / 20) % 2 == 0;
    if (blink) rect(ctx, base, carrier, n, caret_x, ty - 2, caret_x + 3, ty + kTextH * kFieldScale + 2, kCaret, kCaret);
    ++n;
    text(ctx, base, carrier, g_count, fx1 - 20 - text_w(g_count_shown), kFieldY0 + (kFieldH - kTextH) / 2);
    // the keys
    for (int k = 0; k < kKeys; ++k) {
        const Box b = key_box(k);
        const bool sel = k == g_cursor;
        rect(ctx, base, carrier, n++, b.x0, b.y0, b.x1, b.y1, kOutline, kOutline);
        // a face each for normal and selected: a polygon copy keeps the colours it was first drawn with
        const size_t face = n + (sel ? 1 : 0);
        n += 2;
        rect(ctx, base, carrier, face, b.x0 + 2, b.y0 + 2, b.x1 - 2, b.y1 - 2, sel ? kKeySelTop : kKeyTop, sel ? kKeySelBottom : kKeyBottom);
        const std::string label = key_label(k, g_shift);
        text(ctx, base, carrier, g_labels[static_cast<size_t>(k)], (b.x0 + b.x1 - text_w(label, kKeyScale)) / 2,
             (b.y0 + b.y1 - kTextH * kKeyScale) / 2, kKeyScale);
    }
}

bool draw_hook(ppc::Context& ctx, uint8_t* base, uint32_t inst) {
    if (!g_poly_src)
        gui::each_object(base, ppc::ld32(base, inst + 0xFC), [&](uint32_t o) {
            if (!g_poly_src && ppc::ld32(base, o) == kPolygonVtable) g_poly_src = gui::clone_object(base, o, kPolygonSize);
        });
    if (!g_open) return false;
    const uint32_t frame = menu_frame();
    // templates from the screen under the keyboard (until the copies are made): a polygon, and the message lowest on
    // the screen over a whole frame
    if (!g_count) {
        if (g_msg_frame != frame) {
            if (g_msg_best) g_msg_src = g_msg_best, g_msg_inst = g_msg_best_inst;
            g_msg_frame = frame, g_msg_best = 0, g_msg_best_y = -1;
        }
        gui::each_object(base, ppc::ld32(base, inst + 0xFC), [&](uint32_t o) {
            if (ppc::ld32(base, o) != gui::kMessageVtable) return;
            const float y = std::bit_cast<float>(ppc::ld32(base, o + 0x44));
            if (y > g_msg_best_y) g_msg_best_y = y, g_msg_best = o, g_msg_best_inst = inst;
        });
    }
    // the keyboard goes with the template's instance (the game's last ones are the GamePad's, or hidden: drawn through
    // them, nothing shows on the TV)
    if (inst != g_msg_inst || !g_poly_src || !g_msg_src || !g_count) return false;
    g_base_sx = std::bit_cast<float>(ppc::ld32(base, g_msg_src + 0x10));
    g_base_sy = std::bit_cast<float>(ppc::ld32(base, g_msg_src + 0x24));
    gui::draw_original(ctx, base, inst);
    draw_keyboard(ctx, base, inst);
    return true;
}

}  // namespace

void register_soft_keyboard(TextIo io) {
    g_io = std::move(io);
    if (char* v = nullptr; _dupenv_s(&v, nullptr, "MH3U_KBD_PROBE") == 0 && v) {
        g_probe = *v == '1';
        std::free(v);
    }
    set_overlay_hook(&active, &step);
    gui::add_draw_hook(&draw_hook);
}

}  // namespace mh3u::game
