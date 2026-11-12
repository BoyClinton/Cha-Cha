// Helpers for Cha-Cha's menus built from the game's own GUI (gui_util.cpp): game text and sounds, message objects of
// our own, and instances drawn again elsewhere. Used by game_options.cpp and the pages it opens.
#pragma once

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>

#include "ppc_context.h"

namespace mh3u::game::gui {

constexpr uint32_t kSID = 0x10314578, kSAppPad = 0x10312BB0, kSGSound = 0x101D52E0;
constexpr uint32_t kMessageVtable = 0x100A80F0;  // cGUIObjMessage
// pad bits (sAppPad +0x6C pressed, +0x80 pressed or repeating): 1 up, 2 down, 4 left, 8 right, 0x10 A, 0x20 B
constexpr uint32_t kUp = 1, kDown = 2, kLeft = 4, kRight = 8, kDecide = 0x10, kCancel = 0x20;

// A string in guest memory that stays (game text objects keep the pointer they were given).
uint32_t guest_string(const std::string& s);

// FUN_020918FC(sGSound, handle, bank, id) with the volume / pitch floats the calling screen passes (read from the
// guest addresses f1 / f2, as that screen loads them).
void play_sound(ppc::Context& ctx, uint8_t* base, uint32_t bank, uint32_t id, uint32_t f1, uint32_t f2);

// Game text (group, index) through sID's vtable, as the game asks for it; the result may be a reused buffer.
uint32_t game_text(ppc::Context& ctx, uint8_t* base, uint32_t group, uint32_t index);

// cGUIObjMessage::setText (vf22 through FUN_02AFF91C): the object keeps str, so str must stay.
void set_text(ppc::Context& ctx, uint8_t* base, uint32_t obj, uint32_t str);

// Every object of a GUI object tree (cGUIObject: first child +0x68, next sibling +0x6C).
void each_object(uint8_t* base, uint32_t obj, const std::function<void(uint32_t)>& f);

// A message object of our own: a copy of src (0x130 bytes) without what src owns, showing text (laid out at once).
// Never freed; 0 if src is not a cGUIObjMessage.
uint32_t clone_message(ppc::Context& ctx, uint8_t* base, uint32_t src, uint32_t text);
// A new text for such a copy (set and laid out again). Call outside a draw: draws of a frame are deferred.
void set_clone_text(ppc::Context& ctx, uint8_t* base, uint32_t clone, uint32_t text);

struct Swap {
    uint32_t from = 0, to = 0;  // a message object of the instance's tree, and the copy drawn in its place
};
// Draws an animated instance (cGUIInstAnimation::draw?) again, its tree moved by dx, dy TV pixels, with message objects
// swapped for copies (each copy takes the original's matrix, colour and flags first). ctx = the draw call's registers
// (r4 the draw context); restored afterwards.
void draw_moved(ppc::Context& ctx, uint8_t* base, uint32_t inst, float dx, float dy, std::initializer_list<Swap> swaps = {});

// A copy of a GUI object (size bytes, e.g. a cGUIObjPolygon's 0xE0) without its tree links; never freed.
uint32_t clone_object(uint8_t* base, uint32_t src, uint32_t size);
// Draws one object of our own through a carrier instance: the carrier's draw list is cut to that object for one of
// its draws (the object keeps its own matrix, colour and flags). ctx = the draw call's registers.
void draw_object(ppc::Context& ctx, uint8_t* base, uint32_t carrier, uint32_t obj);
// The same, in the place of one of the carrier's own objects (src): the draw list is run from src's entry alone, with
// obj in it (the entry's op is the one the game uses for src). Nothing is drawn if src is not in the list.
void draw_object_as(ppc::Context& ctx, uint8_t* base, uint32_t carrier, uint32_t src, uint32_t obj);

// cGUIInstAnimation::draw? 0x02AE1A50 (r3 = the instance) is shared by every menu here: a hook returns true when it
// drew (or skipped) the instance itself; otherwise the next hook, and finally the game, draws it.
using DrawHook = std::function<bool(ppc::Context& ctx, uint8_t* base, uint32_t inst)>;
void add_draw_hook(DrawHook hook);
// The game's own draw of an instance (no hooks).
void draw_original(ppc::Context& ctx, uint8_t* base, uint32_t inst);

}  // namespace mh3u::game::gui
