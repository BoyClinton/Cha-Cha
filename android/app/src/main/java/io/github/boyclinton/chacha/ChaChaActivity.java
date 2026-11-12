package io.github.boyclinton.chacha;

import org.libsdl.app.SDLActivity;

// Cha-Cha's activity: SDL's, which loads libSDL3.so and libmain.so (the game) and calls its SDL_main.
public class ChaChaActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }
}
