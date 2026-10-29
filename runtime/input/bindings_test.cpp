// Rebinding round trip: bind a key and a controller button, save input.ini, reset, load it again.
#include "host_input.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <filesystem>
#include <string>

namespace in = mh3u::input;

int main() {
    int fails = 0;
    const auto expect = [&](const std::string& got, const std::string& want, const char* what) {
        if (got != want) {
            std::fprintf(stderr, "FAIL %s: got \"%s\", want \"%s\"\n", what, got.c_str(), want.c_str());
            ++fails;
        }
    };
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "mh3u_bindings_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    in::init(dir, false);
    expect(in::key_names("a"), "F, Return, Right click", "default keys for A");
    expect(in::pad_names("a"), "B / Circle", "default controller button for A (by position: right = A)");
    expect(in::pad_names("lstick_up"), "Left stick", "stick direction on the controller");

    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.scancode = SDL_SCANCODE_X;
    if (!in::bind_from_event("a", false, &e)) ++fails;
    e = {};
    e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    e.gbutton.button = SDL_GAMEPAD_BUTTON_WEST;
    if (!in::bind_from_event("zl", true, &e)) ++fails;
    e.gbutton.button = SDL_GAMEPAD_BUTTON_GUIDE;  // reserved for the settings
    if (in::bind_from_event("zr", true, &e)) ++fails;
    e.gbutton.button = SDL_GAMEPAD_BUTTON_TOUCHPAD;  // so is the touchpad click
    if (in::bind_from_event("zr", true, &e)) ++fails;
    expect(in::key_names("a"), "X", "A rebound to X");
    expect(in::pad_names("zl"), "X / Square", "ZL rebound to X / Square");
    expect(in::pad_names("y"), "", "X / Square left its old target (Y, by position)");
    if (!in::save_bindings()) ++fails;

    in::reset_bindings();
    expect(in::key_names("a"), "F, Return, Right click", "reset");
    in::init(dir, false);  // loads the saved input.ini
    expect(in::key_names("a"), "X", "A after reload");
    expect(in::pad_names("zl"), "X / Square", "ZL after reload");
    expect(in::key_names("lstick_up"), "W", "untouched binding after reload");

    std::filesystem::remove_all(dir);
    std::printf("%s\n", fails ? "bindings: FAILED" : "bindings: ok");
    return fails ? 1 : 0;
}
