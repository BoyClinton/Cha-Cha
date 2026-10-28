// AX/AXFX HLE with a software mixer and SDL3 output (runtime/audio/ax.cpp).
#pragma once

namespace mh3u::os {
struct Runtime;
}

namespace mh3u::audio {
// output: open an audio device (false for --headless; MH3U_AUDIO_WAV still records)
void register_ax(os::Runtime& r, bool output);
}
