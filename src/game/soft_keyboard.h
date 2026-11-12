// Cha-Cha's on-screen keyboard (soft_keyboard.cpp): when the game asks for text (the Wii U software keyboard, swkbd:
// a hunter's name, ...) a keyboard drawn with the game's own GUI pieces comes up over the screen. The d-pad / stick
// moves over the keys, A types, B deletes, X shifts, Y is a space, + is OK; the menu pointer hovers and clicks the
// keys; the PC keyboard keeps typing into the same text.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace mh3u::game {

// Host input's text entry (runtime/input): the entry the game is waiting for, and its answers.
struct TextIo {
    std::function<bool(std::u16string& text, size_t& max)> waiting;  // true while an entry waits (its text and limit)
    std::function<void(const std::u16string&)> set;                  // the text typed so far
    std::function<void(bool confirm)> finish;                        // OK / cancel
    std::function<void(bool on)> own_controller;  // the controller types (the game reads an idle pad meanwhile)
    std::function<uint32_t()> buttons;            // the buttons held at the last pad read (VPAD bits)
};
void register_soft_keyboard(TextIo io);

}  // namespace mh3u::game
