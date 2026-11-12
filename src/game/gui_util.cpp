// Helpers for Cha-Cha's menus built from the game's own GUI (gui_util.h).
//
// Replaces: cGUIInstAnimation::draw? 0x02AE1A50 (vf11, vtable +0x64, called by the GUI's draw walk FUN_02ADAFE4 -
// r3 = the instance, r4 = the draw context: draws the instance's object tree with FUN_02AF4BA8, which runs the root's
// draw list +0x80 of {kind, object}: 0 draws the object, 1 finishes it, 2 ends the list). Status: wip - read in
// Ghidra; the menus using it were checked headless with frame dumps (docs/research-log.md, "Game Options").
//
// Message objects (cGUIObjMessage, 0x130 bytes): setText (vf22, vtable +0xBC) frees the parse result (+0xB0) and
// the glyph vertex caches (+0xB4, +0xD8 / +0xDC, +0x100 + 16 i, +0x120 + 4 i, all through sGUI) and parses the new
// text, keeping its pointer; vf10 (vtable +0x5C) lays it out; draws are deferred to the end of the frame, so one
// object relabelled between two draws of a frame breaks both. Hence copies: one message object per text shown. Font
// filters (+0xF8 / +0xFC, sGUI pool) are shared by every label using them and show what was drawn last through
// them, so a menu draws its highlighted item (the glow filter) after the others.
#include "gui_util.h"

#include <bit>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "functions.h"
#include "guest_memory.h"

namespace mh3u::game::gui {
namespace {

constexpr uint32_t kMessageSize = 0x130;
std::vector<DrawHook> g_draw_hooks;

PPC_FUNC(draw_instance) {
    const uint32_t inst = ctx.r[3];
    for (const DrawHook& h : g_draw_hooks)
        if (h(ctx, base, inst)) return;
    sub_02AE1A50__orig(ctx, base);
}

// world matrix translation (4 x 4 row-major at object +0x10: +0x40 / +0x44)
void shift_tree(uint8_t* base, uint32_t root, float dx, float dy) {
    each_object(base, root, [&](uint32_t o) {
        ppc::st32(base, o + 0x40, std::bit_cast<uint32_t>(ppc::ldf32(base, o + 0x40) + dx));
        ppc::st32(base, o + 0x44, std::bit_cast<uint32_t>(ppc::ldf32(base, o + 0x44) + dy));
    });
}

void each_object_at(uint8_t* base, uint32_t obj, const std::function<void(uint32_t)>& f, int depth) {
    for (int n = 0; obj && n < 256 && depth < 16; ++n, obj = ppc::ld32(base, obj + 0x6C)) {
        f(obj);
        each_object_at(base, ppc::ld32(base, obj + 0x68), f, depth + 1);
    }
}

void lay_out(ppc::Context& ctx, uint8_t* base, uint32_t obj) {
    const ppc::Context saved = ctx;
    ctx.r[3] = obj;
    ppc::call_indirect(ctx, base, ppc::ld32(base, ppc::ld32(base, obj) + 0x5C));
    ctx = saved;
}

}  // namespace

uint32_t guest_string(const std::string& s) {
    core::GuestMemory* gm = core::GuestMemory::current();
    if (!gm) return 0;
    const uint32_t n = static_cast<uint32_t>(s.size() + 1);
    const uint32_t g = gm->alloc_hle(n, 4);
    std::memcpy(gm->base() + g, s.c_str(), n);
    return g;
}

void play_sound(ppc::Context& ctx, uint8_t* base, uint32_t bank, uint32_t id, uint32_t f1, uint32_t f2) {
    const ppc::Context saved = ctx;
    ctx.r[1] -= 0x40;
    ctx.f[1].ps0 = ppc::ldf32(base, f1);
    ctx.f[2].ps0 = ppc::ldf32(base, f2);
    ctx.r[3] = ppc::ld32(base, kSGSound);
    ctx.r[4] = ctx.r[1] + 0x20;
    ctx.r[5] = bank;
    ctx.r[6] = id;
    ctx.r[7] = ctx.r[8] = ctx.r[9] = 0;
    sub_020918FC(ctx, base);
    ctx = saved;
}

uint32_t game_text(ppc::Context& ctx, uint8_t* base, uint32_t group, uint32_t index) {
    const ppc::Context saved = ctx;
    const uint32_t sid = ppc::ld32(base, kSID);
    ctx.r[3] = sid;
    ctx.r[4] = group;
    ctx.r[5] = index;
    ppc::call_indirect(ctx, base, ppc::ld32(base, ppc::ld32(base, sid) + 0x134));
    const uint32_t r = ctx.r[3];
    ctx = saved;
    return r;
}

void set_text(ppc::Context& ctx, uint8_t* base, uint32_t obj, uint32_t str) {
    if (!obj || !str) return;
    const ppc::Context saved = ctx;
    ctx.r[3] = obj;
    ctx.r[4] = str;
    ctx.r[5] = static_cast<uint32_t>(std::strlen(reinterpret_cast<const char*>(base + str)));
    sub_02AFF91C(ctx, base);
    ctx = saved;
}

void each_object(uint8_t* base, uint32_t obj, const std::function<void(uint32_t)>& f) { each_object_at(base, obj, f, 0); }

uint32_t clone_message(ppc::Context& ctx, uint8_t* base, uint32_t src, uint32_t text) {
    core::GuestMemory* gm = core::GuestMemory::current();
    if (!gm || !src || !text || ppc::ld32(base, src) != kMessageVtable) return 0;
    const uint32_t c = gm->alloc_hle(kMessageSize, 16);
    std::memcpy(base + c, base + src, kMessageSize);
    for (uint32_t off : {0x68u, 0x6Cu, 0x70u, 0xB0u, 0xB4u, 0xD8u, 0xDCu}) ppc::st32(base, c + off, 0);
    for (uint32_t off = 0x100; off < 0x128; off += 4) ppc::st32(base, c + off, 0);
    const uint32_t empty = ppc::ld32(base, 0x100A7354);  // what clearing a message leaves at +0xB8 / +0xBC
    ppc::st32(base, c + 0xB8, empty);
    ppc::st32(base, c + 0xBC, empty);
    set_clone_text(ctx, base, c, text);
    return c;
}

void set_clone_text(ppc::Context& ctx, uint8_t* base, uint32_t clone, uint32_t text) {
    if (!clone || !text) return;
    set_text(ctx, base, clone, text);
    lay_out(ctx, base, clone);  // in no tree: the GUI's update never lays it out
}

void draw_original(ppc::Context& ctx, uint8_t* base, uint32_t inst) {
    const ppc::Context saved = ctx;
    ctx.r[3] = inst;
    sub_02AE1A50__orig(ctx, base);
    ctx = saved;
}

void draw_moved(ppc::Context& ctx, uint8_t* base, uint32_t inst, float dx, float dy, std::initializer_list<Swap> swaps) {
    const uint32_t root = inst ? ppc::ld32(base, inst + 0xFC) : 0;
    if (!root) return;
    // the swapped objects' entries in the root's draw list
    struct Entry {
        uint32_t at, from, to;
    };
    Entry entries[16];
    int n = 0;
    const uint32_t ops = ppc::ld32(base, root + 0x80);
    for (uint32_t e = ops; e && ppc::ld32(base, e) != 2 && e < ops + 64 * 8 && n < 16; e += 8)
        for (const Swap& s : swaps)
            if (s.to && ppc::ld32(base, e + 4) == s.from) entries[n++] = {e + 4, s.from, s.to};
    const bool moved = dx != 0.0f || dy != 0.0f;
    if (moved) shift_tree(base, root, dx, dy);
    for (int i = 0; i < n; ++i) {  // the live original's matrix, colour and flags (animated by the game)
        std::memcpy(base + entries[i].to + 0x10, base + entries[i].from + 0x10, 0x68 - 0x10);
        std::memcpy(base + entries[i].to + 0x74, base + entries[i].from + 0x74, 0xB0 - 0x74);
        ppc::st32(base, entries[i].at, entries[i].to);
    }
    draw_original(ctx, base, inst);
    for (int i = 0; i < n; ++i)
        if (ppc::ld32(base, entries[i].at) == entries[i].to) ppc::st32(base, entries[i].at, entries[i].from);
    if (moved) shift_tree(base, root, -dx, -dy);
}

uint32_t clone_object(uint8_t* base, uint32_t src, uint32_t size) {
    core::GuestMemory* gm = core::GuestMemory::current();
    if (!gm || !src) return 0;
    const uint32_t c = gm->alloc_hle(size, 16);
    std::memcpy(base + c, base + src, size);
    for (uint32_t off : {0x68u, 0x6Cu, 0x70u}) ppc::st32(base, c + off, 0);  // children, sibling, parent
    return c;
}

void draw_object(ppc::Context& ctx, uint8_t* base, uint32_t carrier, uint32_t obj) {
    const uint32_t root = carrier ? ppc::ld32(base, carrier + 0xFC) : 0;
    const uint32_t ops = root ? ppc::ld32(base, root + 0x80) : 0;
    if (!ops || !obj || ppc::ld32(base, ops) == 2) return;
    const uint32_t keep_obj = ppc::ld32(base, ops + 4), keep_next = ppc::ld32(base, ops + 8);
    ppc::st32(base, ops + 4, obj);
    ppc::st32(base, ops + 8, 2);  // the list ends after it
    draw_original(ctx, base, carrier);
    ppc::st32(base, ops + 4, keep_obj);
    ppc::st32(base, ops + 8, keep_next);
}

void draw_object_as(ppc::Context& ctx, uint8_t* base, uint32_t carrier, uint32_t src, uint32_t obj) {
    const uint32_t root = carrier ? ppc::ld32(base, carrier + 0xFC) : 0;
    const uint32_t ops = root ? ppc::ld32(base, root + 0x80) : 0;
    if (!ops || !obj) return;
    uint32_t e = ops;
    for (int i = 0; i < 64 && ppc::ld32(base, e) != 2 && ppc::ld32(base, e + 4) != src; ++i) e += 8;
    if (ppc::ld32(base, e) == 2 || ppc::ld32(base, e + 4) != src) return;
    // the object's first entry (op 0) alone: the list holds an op 0 and an op 1 entry per object, and running both
    // draws it twice
    const uint32_t end = e + 8;
    const uint32_t keep_end = ppc::ld32(base, end);
    ppc::st32(base, root + 0x80, e);
    for (uint32_t i = e; i < end; i += 8) ppc::st32(base, i + 4, obj);
    ppc::st32(base, end, 2);
    draw_original(ctx, base, carrier);
    for (uint32_t i = e; i < end; i += 8) ppc::st32(base, i + 4, src);
    ppc::st32(base, end, keep_end);
    ppc::st32(base, root + 0x80, ops);
}

void add_draw_hook(DrawHook hook) {
    static std::once_flag once;
    std::call_once(once, [] { ppc::set_override(0x02AE1A50, &draw_instance); });
    g_draw_hooks.push_back(std::move(hook));
}

}  // namespace mh3u::game::gui
