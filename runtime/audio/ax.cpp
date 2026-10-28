// AX (snd_core) + AXFX (snd_user) HLE with a software mixer: the game's sound engine runs as on hardware – voices are
// acquired, configured, started and advance in time – and every 3 ms "AX frame" mixes the playing voices into the TV
// main bus, downmixes it to stereo and queues it on an SDL3 audio stream. Mixing follows Cemu's snd_core (MPL-2.0,
// src/Cafe/OS/libs/snd_core/ax_mix.cpp), simplified: see docs/runtime.md § Audio.
//
// Per voice and frame (96 samples at the 32 kHz renderer rate): decode (DSP ADPCM / PCM16 BE / PCM8) through linear
// sample-rate conversion (16.16 ratio), envelope (VE volume + per-sample delta, 1.15), one-pole low-pass, then the TV
// device mix (6 channels x 4 buses, {u16 vol 1.15, s16 delta}; GamePad-only voices use their DRC L/R mix); only
// bus 0 (main) is mixed – AUX buses feed effects,
// which are not run. After mixing, the game's frame callback runs on its own guest stack.
//
// Timing: with an audio device, frames are produced whenever the device queue drops below ~40 ms (AX time follows
// the audio clock, like hardware); without one (--headless, MH3U_AUDIO=0 or no device) a steady 3 ms host clock.
// MH3U_AUDIO_WAV=<file> also writes the stereo output to a WAV (for checking without listening).
//
// AXVoice layout (public Cafe headers): index +0, state +4, priority +0x1C, callback +0x20, userContext +0x24,
// offsets (AXVoiceOffsets) +0x34: u16 dataType, u16 loopingEnabled, u32 loopOffset, u32 endOffset, u32 currentOffset,
// const void* data.
#include "ax.h"

#include "hle.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mh3u::audio {
namespace {

using os::arg;
using os::ret;

constexpr int kVoices = 96;
constexpr uint32_t kVoiceSize = 0x58;
constexpr int kRate = 32000;
constexpr int kFrame = 96;  // samples per 3 ms frame
constexpr int kTvChannels = 6, kBuses = 4;
constexpr uint16_t kAdpcm = 0, kPcm16 = 0x0A, kPcm8 = 0x19;

struct Mix {
    float vol = 0, delta = 0;  // 1.0 = 0x8000
};

struct Voice {
    bool acquired = false, playing = false, looping = false;
    uint16_t type = 0;   // kAdpcm (offsets in nibbles), kPcm16, kPcm8 (offsets in samples)
    uint16_t stream = 0; // AXSetVoiceType: 1 = stream (loops keep the running ADPCM history)
    uint32_t pos = 0, loop = 0, end = 0, data = 0;
    // ADPCM
    int16_t coef[16] = {};
    uint16_t scale = 0, loop_scale = 0;
    int32_t yn1 = 0, yn2 = 0, loop_yn1 = 0, loop_yn2 = 0;
    // SRC
    bool src = true;     // AXSetVoiceSrcType 0 = no conversion
    uint32_t ratio = 0x10000, frac = 0;
    float s0 = 0, s1 = 0;  // previous/next input sample for interpolation
    bool primed = false;
    // envelope, low-pass, device mix
    float ve = 1.0f, ve_delta = 0;
    bool lpf = false;
    float lpf_a0 = 1, lpf_b0 = 0, lpf_y = 0;
    Mix tv[kTvChannels][kBuses];
    Mix drc[2];  // GamePad (DRC 0) main bus L/R: played on the TV output when the voice has no TV mix
};

std::recursive_mutex g_m;  // also the AXUserBegin/AXUserEnd protection
Voice g_voice[kVoices];
uint32_t g_voice_base = 0;  // guest AXVoice array
uint32_t g_frame_callback = 0;
uint32_t g_stack_top = 0;
std::atomic<bool> g_running{false};
std::thread g_thread;
SDL_AudioStream* g_stream = nullptr;
std::FILE* g_wav = nullptr;
uint32_t g_wav_bytes = 0;

uint32_t voice_addr(int i) { return g_voice_base + static_cast<uint32_t>(i) * kVoiceSize; }
int voice_index(uint32_t addr) {
    if (addr < g_voice_base) return -1;
    uint32_t i = (addr - g_voice_base) / kVoiceSize;
    return i < kVoices ? static_cast<int>(i) : -1;
}

void mirror(uint8_t* base, int i) {
    const Voice& v = g_voice[i];
    const uint32_t a = voice_addr(i);
    ppc::st32(base, a + 0x4, v.playing ? 1 : 0);
    ppc::st16(base, a + 0x34, v.type);
    ppc::st16(base, a + 0x36, v.looping ? 1 : 0);
    ppc::st32(base, a + 0x38, v.loop);
    ppc::st32(base, a + 0x3C, v.end);
    ppc::st32(base, a + 0x40, v.pos);
    ppc::st32(base, a + 0x44, v.data);
}

// End offsets on an ADPCM frame header can never be reached (the decoder skips the 2 header nibbles); as in Cemu,
// move them to the first sample nibble.
uint32_t adpcm_end(uint32_t end) { return (end & 0xF) <= 1 ? end + 2 : end; }

// Next input sample of a voice in 16-bit units, advancing its offset; at the end it loops or stops (then 0).
float next_sample(Voice& v, const uint8_t* base) {
    if (!v.playing) return 0;
    float out = 0;
    uint32_t end = v.end;
    if (v.type == kAdpcm) {
        end = adpcm_end(end);
        if ((v.pos & 0xF) == 0) {
            v.scale = base[v.data + (v.pos >> 1)];
            v.pos += 2;
        }
        const int ci = (v.scale >> 4) & 7;
        int nib = (base[v.data + (v.pos >> 1)] >> ((v.pos & 1) ? 0 : 4)) & 0xF;
        if (nib >= 8) nib -= 16;
        int32_t s = ((nib << 11) * (1 << (v.scale & 0xF)) + v.yn1 * v.coef[ci * 2] + v.yn2 * v.coef[ci * 2 + 1] + 0x400) >> 11;
        s = std::clamp(s, -32768, 32767);
        v.yn2 = v.yn1;
        v.yn1 = s;
        out = static_cast<float>(s);
    } else if (v.type == kPcm16) {
        out = static_cast<int16_t>(ppc::ld16(base, v.data + v.pos * 2));
    } else if (v.type == kPcm8) {
        out = static_cast<float>(static_cast<int8_t>(base[v.data + v.pos]) * 256);
    }
    if (v.pos == end) {
        if (v.looping) {
            v.pos = v.loop;
            if (v.type == kAdpcm) {
                v.scale = v.loop_scale;
                if (!v.stream) {
                    v.yn1 = v.loop_yn1;
                    v.yn2 = v.loop_yn2;
                }
            }
        } else {
            v.playing = false;
        }
    } else {
        ++v.pos;
    }
    return out;
}

// One frame of a voice: decode + SRC + envelope + low-pass, then mixed into the TV main bus.
void mix_voice(Voice& v, const uint8_t* base, float (&tv)[kTvChannels][kFrame]) {
    float buf[kFrame];
    for (int i = 0; i < kFrame; ++i) {
        if (!v.src) {
            buf[i] = next_sample(v, base);
            continue;
        }
        if (!v.primed) {
            v.s1 = next_sample(v, base);
            v.primed = true;
        }
        buf[i] = v.s0 + (v.s1 - v.s0) * (static_cast<float>(v.frac) / 65536.0f);
        v.frac += v.ratio;
        while (v.frac >= 0x10000) {
            v.frac -= 0x10000;
            v.s0 = v.s1;
            v.s1 = next_sample(v, base);
        }
    }
    for (int i = 0; i < kFrame; ++i) {
        v.ve += v.ve_delta;
        buf[i] *= v.ve;
    }
    if (v.lpf)
        for (int i = 0; i < kFrame; ++i) buf[i] = v.lpf_y = v.lpf_a0 * buf[i] - v.lpf_b0 * v.lpf_y;
    bool tv_mix = false;
    for (int c = 0; c < kTvChannels; ++c) tv_mix |= v.tv[c][0].vol != 0 || v.tv[c][0].delta != 0;
    for (int c = 0; c < kTvChannels; ++c) {
        Mix& m = c < 2 && !tv_mix ? v.drc[c] : v.tv[c][0];
        if (m.vol == 0 && m.delta == 0) continue;
        for (int i = 0; i < kFrame; ++i) {
            m.vol += m.delta;
            tv[c][i] += buf[i] * m.vol;
        }
    }
}

void write_wav_header() {
    struct {
        char riff[4] = {'R', 'I', 'F', 'F'};
        uint32_t size;
        char wave[8] = {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '};
        uint32_t fmt_size = 16;
        uint16_t format = 1, channels = 2;
        uint32_t rate = kRate, byte_rate = kRate * 4;
        uint16_t align = 4, bits = 16;
        char data[4] = {'d', 'a', 't', 'a'};
        uint32_t data_size;
    } h;
    h.size = 36 + g_wav_bytes;
    h.data_size = g_wav_bytes;
    std::fseek(g_wav, 0, SEEK_SET);
    std::fwrite(&h, sizeof h, 1, g_wav);
    std::fseek(g_wav, 0, SEEK_END);
}

// Mixes one frame of every playing voice and hands it to the output; returns the frame callback to run.
uint32_t produce_frame(uint8_t* base) {
    float tv[kTvChannels][kFrame] = {};
    uint32_t cb;
    {
        std::lock_guard g(g_m);
        for (int i = 0; i < kVoices; ++i) {
            Voice& v = g_voice[i];
            if (!v.acquired || !v.playing) continue;
            mix_voice(v, base, tv);
            mirror(base, i);
        }
        cb = g_frame_callback;
    }
    // TV channels L R SL SR C LFE -> stereo
    int16_t pcm[kFrame * 2];
    for (int i = 0; i < kFrame; ++i) {
        const float c = 0.7071f * tv[4][i];
        const float l = tv[0][i] + c + 0.7071f * tv[2][i], r = tv[1][i] + c + 0.7071f * tv[3][i];
        pcm[i * 2] = static_cast<int16_t>(std::clamp(l, -32768.0f, 32767.0f));
        pcm[i * 2 + 1] = static_cast<int16_t>(std::clamp(r, -32768.0f, 32767.0f));
    }
    if (g_stream) SDL_PutAudioStreamData(g_stream, pcm, sizeof pcm);
    if (g_wav) {
        std::fwrite(pcm, sizeof pcm, 1, g_wav);
        g_wav_bytes += sizeof pcm;
        if ((g_wav_bytes / sizeof pcm) % 64 == 0) {
            write_wav_header();
            std::fflush(g_wav);
        }
    }
    return cb;
}

void frame_thread() {
    using clock = std::chrono::steady_clock;
    constexpr int kTargetBytes = kRate * 4 * 40 / 1000;  // ~40 ms queued
    auto next = clock::now();
    uint8_t* base = os::gbase();
    while (g_running) {
        if (g_stream) {
            if (SDL_GetAudioStreamQueued(g_stream) >= kTargetBytes) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
        } else {
            next += std::chrono::microseconds(3000);
            std::this_thread::sleep_until(next);
        }
        if (const uint32_t cb = produce_frame(base)) {
            ppc::Context ctx{};
            ctx.r[1] = g_stack_top - 0x20;
            ppc::call_indirect(ctx, base, cb);
        }
    }
}

float fixed15(uint16_t v) { return static_cast<float>(v) / 32768.0f; }
float fixed15s(uint16_t v) { return static_cast<float>(static_cast<int16_t>(v)) / 32768.0f; }

#define AX_FUNC(name) static void hle_##name([[maybe_unused]] ppc::Context& ctx, [[maybe_unused]] uint8_t* base)
#define VOICE()                                \
    std::lock_guard g(g_m);                    \
    const int vi = voice_index(arg(ctx, 0));   \
    if (vi < 0) return;                        \
    Voice& v = g_voice[vi]

AX_FUNC(AXInit) {
    if (g_running.exchange(true)) return;
    g_thread = std::thread(frame_thread);
    g_thread.detach();
}
AX_FUNC(AXQuit) { g_running = false; }
// AXVoice* AXAcquireVoice(u32 priority, AXVoiceCallbackFn, void* userContext)
AX_FUNC(AXAcquireVoice) {
    std::lock_guard g(g_m);
    for (int i = 0; i < kVoices; ++i) {
        if (g_voice[i].acquired) continue;
        g_voice[i] = Voice{};
        g_voice[i].acquired = true;
        const uint32_t a = voice_addr(i);
        std::memset(base + a, 0, kVoiceSize);
        ppc::st32(base, a, static_cast<uint32_t>(i));
        ppc::st32(base, a + 0x1C, arg(ctx, 0));
        ppc::st32(base, a + 0x20, arg(ctx, 1));
        ppc::st32(base, a + 0x24, arg(ctx, 2));
        ret(ctx, a);
        return;
    }
    ret(ctx, 0);  // all voices in use
}
AX_FUNC(AXFreeVoice) {
    VOICE();
    v = Voice{};
}
// void AXSetVoiceState(AXVoice*, AXVoiceState): 0 stopped, 1 playing
AX_FUNC(AXSetVoiceState) {
    VOICE();
    const bool play = (arg(ctx, 1) & 0xFFFF) != 0;
    if (play && !v.playing) {
        v.primed = false;
        v.s0 = v.s1 = 0;
    }
    v.playing = play;
    mirror(base, vi);
}
// void AXSetVoiceOffsets(AXVoice*, const AXVoiceOffsets*)
AX_FUNC(AXSetVoiceOffsets) {
    VOICE();
    const uint32_t o = arg(ctx, 1);
    v.type = ppc::ld16(base, o);
    v.looping = ppc::ld16(base, o + 2) != 0;
    v.loop = ppc::ld32(base, o + 4);
    v.end = ppc::ld32(base, o + 8);
    v.pos = ppc::ld32(base, o + 12);
    v.data = ppc::ld32(base, o + 16);
    mirror(base, vi);
}
// void AXGetVoiceOffsets(AXVoice*, AXVoiceOffsets* out)
AX_FUNC(AXGetVoiceOffsets) {
    std::lock_guard g(g_m);
    const uint32_t o = arg(ctx, 1);
    if (int i = voice_index(arg(ctx, 0)); i >= 0) {
        const Voice& v = g_voice[i];
        ppc::st16(base, o, v.type);
        ppc::st16(base, o + 2, v.looping ? 1 : 0);
        ppc::st32(base, o + 4, v.loop);
        ppc::st32(base, o + 8, v.end);
        ppc::st32(base, o + 12, v.pos);
        ppc::st32(base, o + 16, v.data);
    } else {
        std::memset(base + o, 0, 0x14);
    }
}
// AXResult AXSetVoiceSrcRatio(AXVoice*, float ratio /* f1 */): 16.16, clamped to 8.0 as in snd_core
AX_FUNC(AXSetVoiceSrcRatio) {
    std::lock_guard g(g_m);
    if (int i = voice_index(arg(ctx, 0)); i >= 0)
        g_voice[i].ratio = static_cast<uint32_t>(std::clamp(ctx.f[1].ps0 * 65536.0, 0.0, 524288.0));
    ret(ctx, 0);
}
// void AXSetVoiceSrc(AXVoice*, const AXVoiceSrc*): {u16 ratioInt, u16 ratioFrac, u16 currentFrac, u16 history[4]}
AX_FUNC(AXSetVoiceSrc) {
    VOICE();
    const uint32_t s = arg(ctx, 1);
    v.ratio = std::min<uint32_t>(ppc::ld32(base, s), 0x80000);
    v.frac = ppc::ld16(base, s + 4);
}
// void AXSetVoiceSrcType(AXVoice*, u32 type): 0 none, 1 linear, 2-4 polyphase (run as linear here)
AX_FUNC(AXSetVoiceSrcType) {
    VOICE();
    v.src = arg(ctx, 1) != 0;
}
// void AXSetVoiceAdpcm(AXVoice*, const AXVoiceAdpcm*): {s16 coef[16], u16 gain, u16 predScale, s16 yn1, s16 yn2}
AX_FUNC(AXSetVoiceAdpcm) {
    VOICE();
    const uint32_t a = arg(ctx, 1);
    for (int i = 0; i < 16; ++i) v.coef[i] = static_cast<int16_t>(ppc::ld16(base, a + i * 2));
    v.scale = ppc::ld16(base, a + 0x22);
    v.yn1 = static_cast<int16_t>(ppc::ld16(base, a + 0x24));
    v.yn2 = static_cast<int16_t>(ppc::ld16(base, a + 0x26));
}
// void AXSetVoiceAdpcmLoop(AXVoice*, const AXVoiceAdpcmLoopData*): {u16 predScale, s16 yn1, s16 yn2}
AX_FUNC(AXSetVoiceAdpcmLoop) {
    VOICE();
    const uint32_t a = arg(ctx, 1);
    v.loop_scale = ppc::ld16(base, a);
    v.loop_yn1 = static_cast<int16_t>(ppc::ld16(base, a + 2));
    v.loop_yn2 = static_cast<int16_t>(ppc::ld16(base, a + 4));
}
// void AXSetVoiceType(AXVoice*, u16 type): 0 normal, 1 stream
AX_FUNC(AXSetVoiceType) {
    VOICE();
    v.stream = static_cast<uint16_t>(arg(ctx, 1));
}
// AXResult AXSetVoiceDeviceMix(AXVoice*, device, deviceId, const AXVoiceDeviceMixData* mix): per channel, per bus
// {u16 volume, s16 delta}; TV has 6 channels (L R SL SR C LFE), DRC 4. Only the main bus (0) of TV and DRC 0 is kept.
AX_FUNC(AXSetVoiceDeviceMix) {
    const uint32_t voice = arg(ctx, 0), device = arg(ctx, 1), id = arg(ctx, 2), m = arg(ctx, 3);
    ret(ctx, 0);  // r3 is also the voice argument
    std::lock_guard g(g_m);
    const int vi = voice_index(voice);
    if (vi < 0 || device > 1 || id != 0) return;
    Voice& v = g_voice[vi];
    const auto entry = [&](int c, int b) {
        const uint32_t e = m + static_cast<uint32_t>(c * kBuses + b) * 4;
        return Mix{fixed15(ppc::ld16(base, e)), fixed15s(ppc::ld16(base, e + 2))};
    };
    if (device == 1) {
        for (int c = 0; c < 2; ++c) v.drc[c] = entry(c, 0);
        return;
    }
    for (int c = 0; c < kTvChannels; ++c)
        for (int b = 0; b < kBuses; ++b) v.tv[c][b] = entry(c, b);
}
// void AXSetVoiceVe(AXVoice*, const AXVoiceVeData*): {u16 volume, s16 delta}
AX_FUNC(AXSetVoiceVe) {
    VOICE();
    v.ve = fixed15(ppc::ld16(base, arg(ctx, 1)));
    v.ve_delta = fixed15s(ppc::ld16(base, arg(ctx, 1) + 2));
}
// void AXSetVoiceLpf(AXVoice*, const AXPBLPF*): {u16 on, s16 yn1, s16 a0, s16 b0}
AX_FUNC(AXSetVoiceLpf) {
    VOICE();
    const uint32_t l = arg(ctx, 1);
    v.lpf = ppc::ld16(base, l) != 0;
    v.lpf_y = static_cast<int16_t>(ppc::ld16(base, l + 2));
    v.lpf_a0 = static_cast<int16_t>(ppc::ld16(base, l + 4)) / 32767.0f;
    v.lpf_b0 = static_cast<int16_t>(ppc::ld16(base, l + 6)) / 32767.0f;
}
// void AXSetVoiceLpfCoefs(AXVoice*, u16 a0, u16 b0)
AX_FUNC(AXSetVoiceLpfCoefs) {
    VOICE();
    v.lpf_a0 = static_cast<int16_t>(arg(ctx, 1)) / 32767.0f;
    v.lpf_b0 = static_cast<int16_t>(arg(ctx, 2)) / 32767.0f;
}
AX_FUNC(AXSetVoiceMixerSelect) { ret(ctx, 0); }
// void AXComputeLpfCoefs(u16 freq, u16* a0, u16* b0): one-pole low-pass at freq for the 32 kHz rate (as in Cemu)
AX_FUNC(AXComputeLpfCoefs) {
    const double t = 2.0 - std::cos(static_cast<double>(arg(ctx, 0) & 0xFFFF) / kRate * 6.2831855);
    const uint16_t b0 = static_cast<uint16_t>(-(std::sqrt(t * t - 1.0) - t) * 32768.0);
    if (arg(ctx, 1)) ppc::st16(base, arg(ctx, 1), static_cast<uint16_t>(0x7FFF - b0));
    if (arg(ctx, 2)) ppc::st16(base, arg(ctx, 2), b0);
}
// AXFrameCallback AXRegisterFrameCallback(AXFrameCallback) -> previous
AX_FUNC(AXRegisterFrameCallback) {
    std::lock_guard g(g_m);
    const uint32_t prev = g_frame_callback;
    g_frame_callback = arg(ctx, 0);
    ret(ctx, prev);
}
AX_FUNC(AXRegisterAuxCallback) { ret(ctx, 0); }
AX_FUNC(AXRegisterDeviceFinalMixCallback) { ret(ctx, 0); }
// AXResult AXGetAuxCallback(device, deviceId, auxId, AXAuxCallback* cb, void** ctx)
AX_FUNC(AXGetAuxCallback) {
    if (arg(ctx, 3)) ppc::st32(base, arg(ctx, 3), 0);
    if (arg(ctx, 4)) ppc::st32(base, arg(ctx, 4), 0);
    ret(ctx, 0);
}
// AXResult AXGetDeviceMode(device, u32* mode); AXGetDRCVSMode(u32* mode)
AX_FUNC(AXGetDeviceMode) {
    if (arg(ctx, 1)) ppc::st32(base, arg(ctx, 1), 0);
    ret(ctx, 0);
}
AX_FUNC(AXGetDRCVSMode) {
    if (arg(ctx, 0)) ppc::st32(base, arg(ctx, 0), 0);
    ret(ctx, 0);
}
AX_FUNC(AXSetDRCVSMode) { ret(ctx, 0); }
AX_FUNC(AXSetDRCVSDownmixBalance) { ret(ctx, 0); }
AX_FUNC(AXSetDRCVSLC) { ret(ctx, 0); }
AX_FUNC(AXSetDRCVSOutputGain) { ret(ctx, 0); }
AX_FUNC(AXSetDRCVSSpeakerPosition) { ret(ctx, 0); }
AX_FUNC(AXSetDRCVSSurroundDepth) { ret(ctx, 0); }
AX_FUNC(AXSetDRCVSSurroundLevelGain) { ret(ctx, 0); }
AX_FUNC(AXSetDefaultMixerSelect) {}
AX_FUNC(AXSetDeviceVolume) { ret(ctx, 0); }
// voice-parameter protection: s32 AXUserBegin()/AXUserEnd() -> nesting depth; BOOL AXUserIsProtected()
AX_FUNC(AXUserBegin) {
    g_m.lock();
    ret(ctx, 1);
}
AX_FUNC(AXUserEnd) {
    g_m.unlock();
    ret(ctx, 0);
}
AX_FUNC(AXUserIsProtected) { ret(ctx, 0); }

void ret_true(ppc::Context& c, uint8_t*) { ret(c, 1); }
void ret_void(ppc::Context&, uint8_t*) {}

std::string env(const char* name) {
    char* s = nullptr;
    size_t len = 0;
    std::string r;
    if (_dupenv_s(&s, &len, name) == 0 && s) r = s;
    std::free(s);
    return r;
}

void open_output(bool device) {
    if (const std::string wav = env("MH3U_AUDIO_WAV"); !wav.empty()) {
        if (fopen_s(&g_wav, wav.c_str(), "wb") == 0) write_wav_header();
        else g_wav = nullptr;
    }
    if (!device || env("MH3U_AUDIO") == "0") return;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "audio: SDL_InitSubSystem: %s (running silent)\n", SDL_GetError());
        return;
    }
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, kRate};
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!g_stream) {
        std::fprintf(stderr, "audio: no output device: %s (running silent)\n", SDL_GetError());
        return;
    }
    SDL_ResumeAudioStreamDevice(g_stream);
}

}  // namespace

void register_ax(os::Runtime& r, bool output) {
    g_voice_base = r.mem->alloc_hle(kVoices * kVoiceSize, 32);
    g_stack_top = core::layout::kStackEnd - 0x10000;  // 64 KB below the alarm stack
    open_output(output);
#define R(name) ppc::register_import("snd_core." #name, &hle_##name);
    R(AXInit) R(AXQuit) R(AXAcquireVoice) R(AXFreeVoice) R(AXSetVoiceState) R(AXSetVoiceOffsets) R(AXGetVoiceOffsets)
    R(AXSetVoiceSrcRatio) R(AXSetVoiceSrc) R(AXSetVoiceAdpcmLoop) R(AXSetVoiceAdpcm) R(AXSetVoiceType)
    R(AXSetVoiceDeviceMix) R(AXSetVoiceLpf) R(AXSetVoiceLpfCoefs) R(AXSetVoiceMixerSelect) R(AXSetVoiceSrcType)
    R(AXSetVoiceVe) R(AXComputeLpfCoefs) R(AXRegisterFrameCallback) R(AXRegisterAuxCallback)
    R(AXRegisterDeviceFinalMixCallback) R(AXGetAuxCallback) R(AXGetDeviceMode) R(AXGetDRCVSMode) R(AXSetDRCVSMode)
    R(AXSetDRCVSDownmixBalance) R(AXSetDRCVSLC) R(AXSetDRCVSOutputGain) R(AXSetDRCVSSpeakerPosition)
    R(AXSetDRCVSSurroundDepth) R(AXSetDRCVSSurroundLevelGain) R(AXSetDefaultMixerSelect) R(AXSetDeviceVolume)
    R(AXUserBegin) R(AXUserEnd) R(AXUserIsProtected)
#undef R
    // AXFX (snd_user): effects initialise and accept settings; their callbacks process nothing (AUX buses are not run)
    const char* fx[] = {"MultiChReverb", "ReverbHi", "ReverbHiDpl2", "ReverbHiExp", "ReverbHiExpDpl2",
                        "ReverbStd", "ReverbStdDpl2", "ReverbStdExp", "ReverbStdExpDpl2"};
    for (const char* e : fx) {
        std::string s(e), stem = s, suffix;
        if (s.size() > 4 && s.compare(s.size() - 4, 4, "Dpl2") == 0) {
            stem = s.substr(0, s.size() - 4);
            suffix = "Dpl2";
        }
        for (const char* op : {"Init", "Settings", "SettingsUpdate"})
            ppc::register_import(("snd_user.AXFX" + stem + op + suffix).c_str(), ret_true);
        for (const char* op : {"Shutdown", "Callback"})
            ppc::register_import(("snd_user.AXFX" + stem + op + suffix).c_str(), ret_void);
    }
    ppc::register_import("snd_user.AXFXSetHooks", ret_void);
}

}  // namespace mh3u::audio
