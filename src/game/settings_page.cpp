// A settings page of Cha-Cha's in the game's own Options UI: the Game Settings page's layout (start menu state 20,
// uIDOption page 0x14) with our rows - label left, value between the bouncing arrows right, the highlight bar on the
// selected row, its help in the help box, the page's title in the header plate. Rows go on over pages (the header
// says which); up / down walk them, left / right change the value at once. Action rows (no arrows) take A and
// clicks themselves - to wait for a key to bind, say - where the game's A would leave the page.
//
// Replaces: FUN_0275B3DC (the Game Settings page step, r3 = uIDOption; called every frame by FUN_0275BD84 while the
// page shows) while one of our pages is open. The game's page: cursor +0x3BC, values +0x3B0 / +0x3CC, highlight
// variables +0x3B4[row] (FUN_02B03DBC: 1 selected, 3 not), texts +0x3A8 / +0x3AC (row 0 label / value) and +0x3C4 /
// +0x3C8 (row 1), help +0x328, arrows bounced with FUN_02035D00 (left) / FUN_02035D64 (right) on +0x430 + 0x30 * row;
// sound 2 of bank 8 (floats [0x1006EBA0], [0x1006EB54]) on every move and change. Its layout instances (uIDOption
// +0xF8 array): row 0 = 18 (bar, label, value; y 365), arrows 32 / 33; row 1 = 42 (y 465), arrows 43 / 44; the header
// is Game Options' plate 0. A and B leave the page through the game's own state step (back to the list). Our rows are
// row 0's and row 1's instances drawn again (gui::draw_moved): the selected row from row 0 (highlighted, its texts
// set in the step), every other row from row 1 with label and value copies of its own, drawn first. Status: wip -
// checked headless with frame dumps.
#include "settings_page.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <map>

#include "boot_prompts.h"
#include "functions.h"
#include "guest_memory.h"
#include "gui_util.h"
#include "menu_mouse.h"

namespace mh3u::game::settings_page {
namespace {

constexpr uint32_t kOption = 0x1717C;  // uIDStartMenu -> uIDOption
constexpr uint32_t kHelp = 0x328, kTextA = 0x3A8, kValueA = 0x3AC, kTextB = 0x3C4, kValueB = 0x3C8, kVars = 0x3B4;
constexpr int kRowA = 18, kArrowsA[2] = {32, 33}, kRowB = 42, kArrowsB[2] = {43, 44};
constexpr float kRowAy = 365, kRowBy = 465;
// where the page arrows go (inside the page plaque's ends): the row arrows sit at x 1040 / 1472
constexpr float kArrowX[2] = {1040, 1472}, kPageArrowL = 840, kPageArrowR = 1104;
constexpr float kArrowDrop = 29;  // the arrows sit this much below their row's B position (measured)
// TV pixels: our rows from y 354, 60 apart (seven rows sit between the page plaque and the panel's bottom with the same
// margin above and below); the value area (clicks) x 1000-1520 as on the game's page, its middle 1260
constexpr float kTop = 354, kStep = 60, kBarHeight = 59, kRowX0 = 354, kRowX1 = 1590, kValueX0 = 1000, kValueMid = 1260;
constexpr uint32_t kSound = 2, kDecideSound = 0xD, kSoundF1 = 0x1006EBA0, kSoundF2 = 0x1006EB54;
// The box behind the rows (user 2026-11-06: dark brown, about 25 % opaque): a cGUIObjPolygon of our own (matrix
// +0x10..+0x4C, its translation +0x40 / +0x44 the top left in TV pixels; size +0xC0 / +0xC4; vertex colours
// +0xB0..+0xBC).
constexpr uint32_t kPolygonVtable = 0x100A7E50, kPolygonSize = 0xE0;
// AABBGGRR (user 2026-11-06: dark brown at about 50 %, framed with the help box's own frame): a soft shadow, the panel
// dark brown darkening downwards
constexpr uint32_t kShadow = 0x38000000, kFillTop = 0x80152130, kFillBottom = 0x880A121B;
// The page plaque (user 2026-11-06: no L / R letters, the page's name on a plaque of its own with the arrows inside):
// a nameplate set on the middle of the frame's top edge, built like the game's plates: a black outline, a rim in the
// braided strip's brown (sampled on a frame dump: 4B3726), a black inner line, an opaque dark brown face. A fixed width, so it doesn't jump
// between pages.
constexpr float kPlaqueX0 = 812, kPlaqueX1 = 1132, kPlaqueY0 = 280, kPlaqueY1 = 320;
constexpr uint32_t kPlaqueRimTop = 0xFF26374B, kPlaqueRimBottom = 0xFF1A2836, kPlaqueTop = 0xFA1E2A36, kPlaqueBottom = 0xFA0E161E;
// the frame (the help box's, copied): its outer edge just under the title plate and above the help box, the braided
// strip 6 px inside it top and bottom, 60 px in from the corners; the panel inset 12 px; the header between the top
// strip and the rows
constexpr float kFrameX0 = 308, kFrameX1 = 1636, kFrameY0 = 294, kFrameY1 = 818;
constexpr float kBoxX0 = kFrameX0 + 12, kBoxX1 = kFrameX1 - 12, kBoxY0 = kFrameY0 + 12, kBoxY1 = kFrameY1 - 12;
constexpr float kHeaderY = (kPlaqueY0 + kPlaqueY1) / 2 - 1;  // the page name and arrows: the plaque's middle

const SettingsPage* g_page = nullptr;
bool g_shown = false;  // the page has run (its step was called)
std::vector<SettingRow> g_rows;
int g_cursor = 0;
uint32_t g_owner = 0;  // the uIDOption the copies below belong to
std::vector<uint32_t> g_label_copies, g_value_copies;
std::map<std::string, uint32_t> g_strings;
bool g_installed = false;
HoverWalk g_walk;                              // the pointer's hover
// the help box's frame, copied: corner set, top edge, bottom edge (gui objects; per layout)
constexpr uint32_t kTextureVtable = 0x100A7F30, kTextureSetVtable = 0x100A8010;
// The help box's frame is a cGUIObjTextureSet (+0x100 entries, +0x104 their texel rectangles x0 y0 x1 y1, drawn 1:1
// at those coordinates from the set's origin, in a 1200 x 204 frame image: the four 60 x 60 corners - mirrored by
// reversed rectangles - and a left and a right side piece, 60 x 84) and the braided strip (a cGUIObjTexture, 24 high,
// stretched; the bottom one turned 180 degrees). Copies don't work (a piece's draw data are nodes of its layout's pool,
// shared by a copy), so the pieces themselves are drawn again through their own entries of the help box's draw list,
// each draw with the piece's matrix and (the set) its entry table set for that draw and put back after it: the corners
// at our corners, the side pieces down our sides, the strips stretched (matrix scale) along our top and bottom.
uint32_t g_frame_owner = 0, g_help_inst = 0;
uint32_t g_set_src = 0, g_edge_src[2] = {};
constexpr float kCorner = 60, kEdgeW = 1080;

// draws obj (a piece of the help box) once with its matrix's scale (mirroring) and translation set for that draw
void draw_piece(ppc::Context& ctx, uint8_t* base, uint32_t obj, float x, float y, float sx, float sy) {
    const uint32_t keep[4] = {ppc::ld32(base, obj + 0x40), ppc::ld32(base, obj + 0x44), ppc::ld32(base, obj + 0x10), ppc::ld32(base, obj + 0x24)};
    ppc::st32(base, obj + 0x40, std::bit_cast<uint32_t>(x));
    ppc::st32(base, obj + 0x44, std::bit_cast<uint32_t>(y));
    ppc::st32(base, obj + 0x10, std::bit_cast<uint32_t>(sx));
    ppc::st32(base, obj + 0x24, std::bit_cast<uint32_t>(sy));
    gui::draw_object_as(ctx, base, g_help_inst, obj, obj);
    ppc::st32(base, obj + 0x40, keep[0]);
    ppc::st32(base, obj + 0x44, keep[1]);
    ppc::st32(base, obj + 0x10, keep[2]);
    ppc::st32(base, obj + 0x24, keep[3]);
}

// The corners and strips (the help box's own pieces); the sides are drawn with the panel's polygons (rect)
void draw_frame(ppc::Context& ctx, uint8_t* base) {
    if (!g_set_src || !g_edge_src[0] || !g_edge_src[1]) return;
    const uint32_t count = ppc::ld32(base, g_set_src + 0x100);
    if (count < 6) return;
    // the set drawn with one entry draws its first piece, the top left corner; the others are it mirrored
    ppc::st32(base, g_set_src + 0x100, 1);
    draw_piece(ctx, base, g_set_src, kFrameX0, kFrameY0, 1, 1);
    draw_piece(ctx, base, g_set_src, kFrameX1, kFrameY0, -1, 1);
    draw_piece(ctx, base, g_set_src, kFrameX0, kFrameY1, 1, -1);
    draw_piece(ctx, base, g_set_src, kFrameX1, kFrameY1, -1, -1);
    ppc::st32(base, g_set_src + 0x100, count);
    // the braided strips, 6 px inside the frame, between the corners (scaled from their 1080 to our width)
    const float sx = (kFrameX1 - kFrameX0 - 2 * kCorner) / kEdgeW;
    draw_piece(ctx, base, g_edge_src[0], kFrameX0 + kCorner, kFrameY0 + 6, sx, 1);
    draw_piece(ctx, base, g_edge_src[1], kFrameX1 - kCorner, kFrameY1 - 6, -sx, -1);  // turned 180: from its far end
}
uint32_t g_box = 0;                            // a cGUIObjPolygon to copy (the first one drawn)
uint32_t g_rects[24] = {};                     // ours: shadow, fill, the frame's side bands, the page plaque
uint32_t g_header = 0;                         // the page name (a copy of row B's label)
uint32_t g_header_owner = 0;
std::string g_header_text;

std::string header_name(int pg);
void forget_layouts();

uint32_t str(const std::string& s) {
    auto it = g_strings.find(s);
    if (it == g_strings.end()) it = g_strings.emplace(s, gui::guest_string(s)).first;
    return it->second;
}
std::string help_text(const std::string& s) {  // the game's line breaks are \r\n
    std::string out;
    for (char c : s) out += c == '\n' ? std::string("\r\n") : std::string(1, c);
    return out;
}

uint32_t instance(uint8_t* base, uint32_t opt, int index) {
    const uint32_t arr = ppc::ld32(base, opt + 0xF8);
    return arr ? ppc::ld32(base, arr + static_cast<uint32_t>(index) * 4) : 0;
}

int page_of(int row) {
    int p = 0;
    for (size_t i = 0; i < g_page->page_starts.size(); ++i)
        if (row >= g_page->page_starts[i]) p = static_cast<int>(i);
    return p;
}
int page_start(int p) { return g_page->page_starts.empty() ? 0 : g_page->page_starts[static_cast<size_t>(p)]; }
int page_end(int p) {
    return p + 1 < static_cast<int>(g_page->page_starts.size()) ? g_page->page_starts[static_cast<size_t>(p) + 1]
                                                               : static_cast<int>(g_rows.size());
}

const std::string& value_name(const SettingRow& r) {
    static const std::string none;
    return r.value >= 0 && r.value < static_cast<int>(r.values.size()) ? r.values[static_cast<size_t>(r.value)] : none;
}

void call1(ppc::Context& ctx, uint8_t* base, void (*fn)(ppc::Context&, uint8_t*), uint32_t a) {
    const ppc::Context saved = ctx;
    ctx.r[3] = a;
    fn(ctx, base);
    ctx = saved;
}
void set_var(ppc::Context& ctx, uint8_t* base, uint32_t var, uint32_t value) {
    if (!var) return;
    const ppc::Context saved = ctx;
    ctx.r[3] = var;
    ctx.r[4] = value;
    ctx.r[5] = 0;
    sub_02B03DBC(ctx, base);
    ctx = saved;
}

// the copies of row 1's label and value for every row, made for this uIDOption and kept up to date
void update_copies(ppc::Context& ctx, uint8_t* base, uint32_t opt) {
    if (opt != g_owner || g_label_copies.size() != g_rows.size()) {
        g_owner = opt;
        g_label_copies.assign(g_rows.size(), 0);
        g_value_copies.assign(g_rows.size(), 0);
    }
    const uint32_t label_b = ppc::ld32(base, opt + kTextB), value_b = ppc::ld32(base, opt + kValueB);
    for (size_t i = 0; i < g_rows.size(); ++i) {
        if (!g_label_copies[i]) g_label_copies[i] = gui::clone_message(ctx, base, label_b, str(g_rows[i].label));
        if (!g_value_copies[i]) g_value_copies[i] = gui::clone_message(ctx, base, value_b, str(value_name(g_rows[i])));
    }
}

// the rows read again: copies whose text changed get the new one (in the step, never between draws)
void refresh_rows(ppc::Context& ctx, uint8_t* base) {
    if (!g_page->rows) return;
    std::vector<SettingRow> fresh = g_page->rows();
    if (fresh.size() != g_rows.size()) return;
    for (size_t i = 0; i < fresh.size() && i < g_value_copies.size(); ++i) {
        if (value_name(fresh[i]) != value_name(g_rows[i])) gui::set_clone_text(ctx, base, g_value_copies[i], str(value_name(fresh[i])));
        if (fresh[i].label != g_rows[i].label) gui::set_clone_text(ctx, base, g_label_copies[i], str(fresh[i].label));
    }
    g_rows = std::move(fresh);
}

void change(ppc::Context& ctx, uint8_t* base, uint32_t opt, int step) {
    SettingRow& r = g_rows[static_cast<size_t>(g_cursor)];
    if (r.action || r.values.size() < 2) return;
    const int n = static_cast<int>(r.values.size());
    r.value = ((r.value + step) % n + n) % n;
    call1(ctx, base, step < 0 ? sub_02035D00 : sub_02035D64, opt + 0x430);  // row 0's arrow bounces
    gui::play_sound(ctx, base, 8, kSound, kSoundF1, kSoundF2);
    const std::string shown = value_name(r);
    if (g_page->change) g_page->change(g_cursor, r.value);
    if (static_cast<size_t>(g_cursor) < g_value_copies.size())
        gui::set_clone_text(ctx, base, g_value_copies[static_cast<size_t>(g_cursor)], str(shown));
    refresh_rows(ctx, base);  // a change may change other rows (their values, or what is offered)
}

// A (or a click) on an action row: the page's activate instead of the game leaving the page
void activate(ppc::Context& ctx, uint8_t* base) {
    gui::play_sound(ctx, base, 8, kDecideSound, kSoundF1, kSoundF2);
    if (g_page->activate) g_page->activate(g_cursor);
    refresh_rows(ctx, base);
}

// up / down stay on the page (wrapping round it); only L / R turn pages (user 2026-11-06)
void move(ppc::Context& ctx, uint8_t* base, int to) {
    const int p = page_of(g_cursor), first = page_start(p), n = page_end(p) - first;
    if (n <= 0) return;
    g_cursor = first + ((to - first) % n + n) % n;
    gui::play_sound(ctx, base, 8, kSound, kSoundF1, kSoundF2);
}

void page_sound(ppc::Context& ctx, uint8_t* base) {
    static uint32_t handle = 0;
    if (!handle)
        if (core::GuestMemory* gm = core::GuestMemory::current()) handle = gm->alloc_hle(16, 8);
    const ppc::Context saved = ctx;
    ctx.r[3] = handle;
    ctx.r[4] = 10;
    sub_020BFD88(ctx, base);
    ctx = saved;
}

// L / R or a click on the title: the next (+1) or previous (-1) page, its first row selected
void turn_page(ppc::Context& ctx, uint8_t* base, int step) {
    const int pages = static_cast<int>(g_page->page_starts.size());
    if (pages < 2) return;
    g_cursor = page_start((page_of(g_cursor) + (step > 0 ? 1 : pages - 1)) % pages);
    page_sound(ctx, base);
    g_walk.reset();
}

// the Game Settings page step (r3 = uIDOption)
PPC_FUNC(page_step) {
    forget_layouts();
    if (!g_page) {
        sub_0275B3DC__orig(ctx, base);
        return;
    }
    const uint32_t opt = ctx.r[3];
    if (!g_shown) {
        g_shown = true;
        g_rows = g_page->rows ? g_page->rows() : std::vector<SettingRow>{};
        g_cursor = 0;
    }
    if (g_rows.empty()) return;
    update_copies(ctx, base, opt);
    if (g_page->live) refresh_rows(ctx, base);
    const uint32_t pad = ppc::ld32(base, gui::kSAppPad);
    const uint32_t dirs = pad ? ppc::ld32(base, pad + 0x80) | ppc::ld32(base, pad + 0x6C) : 0;
    const uint32_t pressed = pad ? ppc::ld32(base, pad + 0x6C) : 0;
    if ((pressed & gui::kDecide) && g_rows[static_cast<size_t>(g_cursor)].action) {
        // the game's state step (which runs after this one) must not see this A: it would leave the page
        ppc::st32(base, pad + 0x6C, pressed & ~gui::kDecide);
        ppc::st32(base, pad + 0x80, ppc::ld32(base, pad + 0x80) & ~gui::kDecide);
        activate(ctx, base);
    } else if (dirs & gui::kUp) move(ctx, base, g_cursor - 1);
    else if (dirs & gui::kDown) move(ctx, base, g_cursor + 1);
    else if (dirs & gui::kLeft) change(ctx, base, opt, -1);
    else if (dirs & gui::kRight) change(ctx, base, opt, +1);
    // L / R (or ZL / ZR) turn the page, as the game turns its pages, with its page-turn sound (FUN_020BFD88 id 10).
    // Here the pressed word +0x6C has L 0x100 / ZL 0x800, R 0x200 / ZR 0x1000 (logged); the DLC screen's +0x11C
    // bits 0x800 / 0x400 are another layout
    if (pressed & 0x1B00) turn_page(ctx, base, (pressed & 0x1200) ? 1 : -1);
    // the pointer, as on the item pouch (HoverWalk): moving onto a row selects it; a click on another row selects it
    // (and runs an action row); on the selected row's value it changes the value (left half back, right half on)
    if (float x = 0, y = 0; menu_pointer(x, y)) {
        const int p = page_of(g_cursor);
        int hit = -1;
        for (int r = page_start(p); r < page_end(p); ++r) {
            const float top = kTop + static_cast<float>(r - page_start(p)) * kStep;
            if (x >= kRowX0 && x <= kRowX1 && y >= top && y < top + kBarHeight) hit = r;
        }
        if (const int d = g_walk.step(x, y, hit, g_cursor)) move(ctx, base, g_cursor + d);
        if (g_walk.arrived(g_cursor) && g_rows[static_cast<size_t>(g_cursor)].action) activate(ctx, base);
        // the page plaque (drawn when there are pages): a click on its left half (the back arrow) turns back, on its
        // right half (the on arrow) on
        const bool on_header = g_page->page_starts.size() > 1 && x >= kPlaqueX0 - 8 && x <= kPlaqueX1 + 8 &&
                               y >= kPlaqueY0 - 8 && y <= kPlaqueY1 + 8;
        while (menu_click())
            if (on_header) turn_page(ctx, base, x < (kPlaqueX0 + kPlaqueX1) / 2 ? -1 : 1);
            else if (hit >= 0 && hit == g_cursor && g_rows[static_cast<size_t>(hit)].action) activate(ctx, base);
            else if (hit >= 0 && hit == g_cursor && x >= kValueX0) change(ctx, base, opt, x < kValueMid ? -1 : +1);
            else if (hit >= 0 && hit != g_cursor) g_walk.click(hit);
    }
    // row 0 of the game's layout is the selected row: highlighted, our texts
    set_var(ctx, base, ppc::ld32(base, opt + kVars), 1);
    set_var(ctx, base, ppc::ld32(base, opt + kVars + 4), 3);
    set_var(ctx, base, ppc::ld32(base, opt + 0x30C), 0);  // the header plate (Game Options' plate 0) not highlighted
    const SettingRow& sel = g_rows[static_cast<size_t>(g_cursor)];
    gui::set_text(ctx, base, ppc::ld32(base, opt + kTextA), str(sel.label));
    gui::set_text(ctx, base, ppc::ld32(base, opt + kValueA), str(value_name(sel)));
    gui::set_text(ctx, base, ppc::ld32(base, opt + kHelp), str(help_text(sel.help)));
    const std::string title = g_page->title;  // the page's name and number: the box's header (draw)
    if (const std::string name = header_name(page_of(g_cursor)); g_header && name != g_header_text)
        gui::set_clone_text(ctx, base, g_header, str(name)), g_header_text = name;  // (in the step: never between draws)
    const uint32_t plate0 = ppc::ld32(base, opt + 0x2F4), list = plate0 ? ppc::ld32(base, plate0 + 0xF8) : 0;
    if (list) gui::set_text(ctx, base, ppc::ld32(base, list + 8), str(title));
}

// the page's rows: drawn at row 0's draw (the first of the six instances), the others' own draws skipped
// a new start menu (back from the game): everything kept from the old title layouts goes, made again from the new ones
// (the old objects are freed, and the new ones may sit at the same addresses, so owner checks alone don't see it)
uint32_t g_generation = 0;
void forget_layouts() {
    const uint32_t gen = start_menu_generation();
    if (gen == g_generation) return;
    g_generation = gen;
    g_owner = 0, g_label_copies.clear(), g_value_copies.clear();
    g_frame_owner = g_help_inst = g_set_src = 0, g_edge_src[0] = g_edge_src[1] = 0;
    g_box = 0;
    for (uint32_t& r : g_rects) r = 0;
    g_header = g_header_owner = 0;
}

bool draw(ppc::Context& ctx, uint8_t* base, uint32_t inst) {
    forget_layouts();
    if (!g_page || !g_shown || g_rows.empty()) return false;
    const uint32_t sm = start_menu_widget();
    if (!sm || ppc::ld32(base, sm + 0x2A0) != 20) return false;
    const uint32_t opt = ppc::ld32(base, sm + kOption);
    if (!opt || opt != g_owner) return false;
    const uint32_t a = instance(base, opt, kRowA), b = instance(base, opt, kRowB);
    const uint32_t aa[2] = {instance(base, opt, kArrowsA[0]), instance(base, opt, kArrowsA[1])};
    const uint32_t ab[2] = {instance(base, opt, kArrowsB[0]), instance(base, opt, kArrowsB[1])};
    if (inst != a && inst != b && inst != aa[0] && inst != aa[1] && inst != ab[0] && inst != ab[1]) return false;
    if (inst != a) return true;
    const uint32_t label_b = ppc::ld32(base, opt + kTextB), value_b = ppc::ld32(base, opt + kValueB);
    const int p = page_of(g_cursor);
    // a polygon of ours in slot i (each its own copy: draws are deferred); vertex colours +0xB0..+0xBC: top left, top
    // right, bottom left, bottom right
    const auto rect = [&](int i, float x0, float y0, float x1, float y1, uint32_t top, uint32_t bottom) {
        if (!g_rects[i]) g_rects[i] = gui::clone_object(base, g_box, kPolygonSize);
        const uint32_t r = g_rects[i];
        ppc::st32(base, r + 0x40, std::bit_cast<uint32_t>(x0));
        ppc::st32(base, r + 0x44, std::bit_cast<uint32_t>(y0));
        ppc::st32(base, r + 0xC0, std::bit_cast<uint32_t>(x1 - x0));
        ppc::st32(base, r + 0xC4, std::bit_cast<uint32_t>(y1 - y0));
        ppc::st32(base, r + 0xB0, top), ppc::st32(base, r + 0xB4, top);
        ppc::st32(base, r + 0xB8, bottom), ppc::st32(base, r + 0xBC, bottom);
        gui::draw_object(ctx, base, ab[0], r);
    };
    const bool paged = g_page->page_starts.size() > 1;
    if (g_box) {  // the box behind the rows (first: under everything of ours)
        int n = 0;
        // a soft shadow: two layers, offset down-right, the outer one wider and fainter
        rect(n++, kBoxX0 + 4, kBoxY0 + 8, kBoxX1 + 10, kBoxY1 + 14, kShadow, kShadow);
        rect(n++, kBoxX0 + 2, kBoxY0 + 4, kBoxX1 + 5, kBoxY1 + 7, kShadow, kShadow);
        rect(n++, kBoxX0, kBoxY0, kBoxX1, kBoxY1, kFillTop, kFillBottom);
        n = 6;  // fixed slots from here: a copy never changes job between pages (its draw may still be pending)
        // the frame's sides, between the corners, as the help box's own (measured on a frame dump, TV pixels in from
        // the frame's edge): black, dark brown, black, darker brown, black; mirrored on the right
        struct Band {
            float from, to;
            uint32_t colour;
        };
        constexpr Band kBands[] = {{14, 17, 0xFF000000}, {17, 21, 0xFF1C2A38}, {21, 23, 0xFF000000}, {23, 27, 0xFF131E2B}, {27, 30, 0xFF000000}};
        for (const Band& band : kBands) {
            rect(n++, kFrameX0 + band.from, kFrameY0 + kCorner, kFrameX0 + band.to, kFrameY1 - kCorner, band.colour, band.colour);
            rect(n++, kFrameX1 - band.to, kFrameY0 + kCorner, kFrameX1 - band.from, kFrameY1 - kCorner, band.colour, band.colour);
        }
    }
    if (g_frame_owner == opt && g_help_inst) draw_frame(ctx, base);
    if (g_box && paged) {  // the page plaque, over the frame's top strip: shadow, outline, rim, inner line, face
        rect(18, kPlaqueX0 - 1, kPlaqueY0 + 3, kPlaqueX1 + 7, kPlaqueY1 + 9, kShadow, kShadow);
        rect(19, kPlaqueX0 - 4, kPlaqueY0 - 4, kPlaqueX1 + 4, kPlaqueY1 + 4, 0xFF000000, 0xFF000000);
        rect(20, kPlaqueX0, kPlaqueY0, kPlaqueX1, kPlaqueY1, kPlaqueRimTop, kPlaqueRimBottom);
        rect(21, kPlaqueX0 + 4, kPlaqueY0 + 4, kPlaqueX1 - 4, kPlaqueY1 - 4, 0xFF000000, 0xFF000000);
        rect(22, kPlaqueX0 + 6, kPlaqueY0 + 6, kPlaqueX1 - 6, kPlaqueY1 - 6, kPlaqueTop, kPlaqueBottom);
    }
    for (int r = page_start(p); r < page_end(p); ++r) {  // the other rows first: the selected one's glow last
        if (r == g_cursor) continue;
        const float dy = kTop + static_cast<float>(r - page_start(p)) * kStep - kRowBy;
        gui::draw_moved(ctx, base, b, 0, dy, {{label_b, g_label_copies[static_cast<size_t>(r)]}, {value_b, g_value_copies[static_cast<size_t>(r)]}});
        if (!g_rows[static_cast<size_t>(r)].action)  // action rows have no values to step through
            for (uint32_t arrow : ab) gui::draw_moved(ctx, base, arrow, 0, dy);
    }
    if (paged) {  // on the plaque: the back arrow, the page's name, the on arrow
        gui::draw_moved(ctx, base, ab[0], kPageArrowL - kArrowX[0], kHeaderY - kRowBy - kArrowDrop - 3);  // -3: centred on the face (measured)
        gui::draw_moved(ctx, base, ab[1], kPageArrowR - kArrowX[1], kHeaderY - kRowBy - kArrowDrop - 3);  // -3: centred on the face (measured)
        if (opt != g_header_owner) g_header_owner = opt, g_header = 0;
        const std::string name = header_name(page_of(g_cursor));
        if (!g_header) g_header = gui::clone_message(ctx, base, label_b, str(name)), g_header_text = name;
        const float ly = ppc::ldf32(base, label_b + 0x44) - kRowBy + kHeaderY - 34;  // the capitals centred on the face (measured)
        const auto place = [&](uint32_t m, float x) {
            if (!m) return;
            ppc::st32(base, m + 0x40, std::bit_cast<uint32_t>(x));
            ppc::st32(base, m + 0x44, std::bit_cast<uint32_t>(ly));
            gui::draw_object(ctx, base, ab[0], m);
        };
        place(g_header, (kPlaqueX0 + kPlaqueX1) / 2 - static_cast<float>(name.size()) * 8.0f);
    }
    const float dy = kTop + static_cast<float>(g_cursor - page_start(p)) * kStep - kRowAy;
    gui::draw_moved(ctx, base, a, 0, dy);
    if (!g_rows[static_cast<size_t>(g_cursor)].action)
        for (uint32_t arrow : aa) gui::draw_moved(ctx, base, arrow, 0, dy);
    return true;
}

std::string header_name(int pg) {
    std::string name = static_cast<size_t>(pg) < g_page->page_titles.size() ? g_page->page_titles[static_cast<size_t>(pg)] + "  " : "";
    return name + std::to_string(pg + 1) + "/" + std::to_string(g_page->page_starts.size());
}

}  // namespace

void install() {
    if (g_installed) return;
    g_installed = true;
    ppc::set_override(0x0275B3DC, &page_step);
    gui::add_draw_hook(&draw);
    // the box's polygon: a copy of the first cGUIObjPolygon drawn (the main menu's plates have them, shown before
    // any of our pages)
    gui::add_draw_hook([](ppc::Context&, uint8_t* base, uint32_t inst) {
        forget_layouts();
        if (g_box) return false;
        gui::each_object(base, ppc::ld32(base, inst + 0xFC), [&](uint32_t o) {
            if (!g_box && ppc::ld32(base, o) == kPolygonVtable) g_box = gui::clone_object(base, o, kPolygonSize);
        });
        return false;
    });
    // the help box's frame (its layout instance holds the help text): a corner set and the braided edge strip, top and
    // bottom (the bottom one turned 180 degrees), copied once per layout for our panel's frame
    gui::add_draw_hook([](ppc::Context&, uint8_t* base, uint32_t inst) {
        if (!g_page || !g_shown) return false;
        const uint32_t sm = start_menu_widget();
        const uint32_t opt = sm ? ppc::ld32(base, sm + kOption) : 0;
        const uint32_t help = opt ? ppc::ld32(base, opt + kHelp) : 0;
        if (!help || opt == g_frame_owner) return false;
        bool has = false;
        gui::each_object(base, ppc::ld32(base, inst + 0xFC), [&](uint32_t o) { has |= o == help; });
        if (!has) return false;
        g_frame_owner = opt;
        g_help_inst = inst;
        g_set_src = 0, g_edge_src[0] = g_edge_src[1] = 0;
        int edges = 0;
        gui::each_object(base, ppc::ld32(base, inst + 0xFC), [&](uint32_t o) {
            const uint32_t vt = ppc::ld32(base, o);
            if (vt == kTextureSetVtable && !g_set_src) g_set_src = o;
            else if (vt == kTextureVtable && ppc::ldf32(base, o + 0xC4) < 30 && edges < 2) g_edge_src[edges++] = o;
        });
        return false;
    });
}

void open(ppc::Context&, uint8_t*, const SettingsPage* page) {
    g_page = page;
    g_shown = false;
    g_rows.clear();
}

bool active() { return g_page && g_shown; }

void closed() {
    if (g_page && g_shown) g_page = nullptr, g_shown = false;
}

}  // namespace mh3u::game::settings_page
