// Input: host input (runtime/input: keyboard + mouse and SDL gamepads, docs/input.md) drives, by default, both a Wii U
// Pro Controller on WPAD/KPAD channel 0 and the Wii U GamePad (vpad channel 0) with the same buttons and sticks
// (MH3U_CONTROLLER=both, the default since issue #6): the game listens to the one its Controller Settings option
// names, so both choices work, and the GamePad's touch screen gets the mouse on the shown GamePad image (F3) and the
// PlayStation touchpad. One host read serves a VPADRead and a KPADReadEx in a row (shared_read), so taps, wheel
// pulses and mouse look are not split between them.
// MH3U_CONTROLLER=pro-only: the Pro Controller and no GamePad (the default up to 0.3.2: a save set to the GamePad
// switches after one "The set controller could not be detected" notice); pro: the Pro Controller plus a GamePad whose
// only input is the touch screen; gamepad: only the GamePad. No Wii Remotes.
//
// KPADStatus (0xF0, layout as in Cemu padscore.h): hold/trig/release +0/+4/+8 (Wii Remote core buttons), devType
// +0x5C (31 = Pro Controller "URCC"), wpadErr +0x5D, data_format +0x5F (22 = URCC), Pro Controller extension at
// +0x60: hold, trig, release (u32), lstick +0x6C, rstick +0x74 (2 floats), charge +0x7C, cable +0x80.
//
// VPADStatus (0xAC bytes, vpad/input.h): hold +0, trigger +4, release +8 (u32), leftStick +0xC, rightStick +0x14
// (2 floats), accelerometer +0x1C, gyro +0x38, angle +0x44, error +0x50 (s8), tpNormal +0x52 / tpFiltered1 +0x5A /
// tpFiltered2 +0x62 (u16 x, y, touched, validity), direction +0x6C, headphone +0x90, mag +0x94, volume +0xA0,
// battery +0xA1, mic +0xA2.
//
// MH3U_INPUT_SCRIPT="frame[+frames]:BUTTON[|BUTTON...];..." presses GamePad buttons at given swap counts (default 6
// frames), e.g. "700:A;760+10:A|R" - for headless tests. LSTICK_LEFT/RIGHT/UP/DOWN (RSTICK_... the right stick) push a stick fully. MH3U_INPUT_CLOCK=time reads the numbers as 1/60 s since
// the script was loaded instead, so a script keeps its timing at any frame rate (MH3U_FPS). "@N:..." counts N from the
// first pad read after the title's main menu opened (core/dev_anchor.h): boot length varies, menu routes then don't.
// In a script with "@" entries the plain-numbered presses stop once the main menu is open, so a burst of A presses
// gets through the attract movie and the title whenever they come: "500+400:A;@120:DOWN;@180:A".
#include "hle.h"

#include "dev_anchor.h"

#include "host_input.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace mh3u::os {
namespace {

constexpr uint32_t kVpadStatusSize = 0xAC;  // sizeof(VPADStatus)
constexpr int32_t kWpadNoController = -1;   // WPAD_ERROR_NO_CONTROLLER
constexpr int32_t kKpadNoSamples = -1;      // KPAD_ERROR_NO_SAMPLES

struct ScriptPress {
    uint32_t frame, frames, buttons;
    bool from_menu = false;  // "@N": N counts from the main menu's opening
    float lx = 0, ly = 0;  // left stick
    float rx = 0, ry = 0;  // right stick
};
// the presses active at this frame: buttons OR'ed, each stick from the last one that pushes it
void apply_script(input::PadState& s, uint32_t frame);
std::vector<ScriptPress> g_script;
bool g_script_time = false;  // MH3U_INPUT_CLOCK=time
std::chrono::steady_clock::time_point g_script_start;

// the script's clock: swap count, or 1/60 s ticks of wall time
uint32_t script_now() {
    if (!g_script_time) return rt().frames.load();
    const auto t = std::chrono::steady_clock::now() - g_script_start;
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(t).count() * 60 / 1000000);
}
uint32_t g_menu_anchor = 0;  // script clock when the main menu was first seen open (0: not yet)
uint32_t g_prev_hold = 0;
bool g_pro = true;                 // host input is a Pro Controller (default; MH3U_CONTROLLER=gamepad turns it off)
bool g_no_gamepad = false;         // VPADRead reports no GamePad (MH3U_CONTROLLER=pro-only)
bool g_vpad_buttons = true;        // with g_pro: the GamePad gets the host buttons and sticks too (MH3U_CONTROLLER=both)
uint32_t g_pro_prev = 0;           // previous Pro Controller hold
uint32_t g_connect_cb = 0;         // KPADSetConnectCallback(0, cb)
uint32_t g_extension_cb = 0;       // WPADSetExtensionCallback(0, cb)
// Guest callbacks (function, r3, r4) the real padscore calls asynchronously; delivered on the game's next pad read.
struct PendingCallback {
    uint32_t fn, a, b;
};
std::vector<PendingCallback> g_pending;

// VPAD button bits -> Pro Controller (WPAD_PRO_*) bits
uint32_t pro_buttons(uint32_t v) {
    static const uint32_t map[][2] = {
        {input::kUp, 0x1}, {input::kLeft, 0x2}, {input::kZR, 0x4}, {input::kX, 0x8}, {input::kA, 0x10},
        {input::kY, 0x20}, {input::kB, 0x40}, {input::kZL, 0x80}, {input::kR, 0x200}, {input::kPlus, 0x400},
        {input::kHome, 0x800}, {input::kMinus, 0x1000}, {input::kL, 0x2000}, {input::kDown, 0x4000},
        {input::kRight, 0x8000}, {input::kStickR, 0x10000}, {input::kStickL, 0x20000}};
    uint32_t r = 0;
    for (const auto& m : map)
        if (v & m[0]) r |= m[1];
    return r;
}

uint32_t button_mask(const std::string& name) {
    static const struct {
        const char* n;
        uint32_t m;
    } names[] = {{"A", input::kA}, {"B", input::kB}, {"X", input::kX}, {"Y", input::kY}, {"L", input::kL},
                 {"R", input::kR}, {"ZL", input::kZL}, {"ZR", input::kZR}, {"PLUS", input::kPlus},
                 {"MINUS", input::kMinus}, {"UP", input::kUp}, {"DOWN", input::kDown}, {"LEFT", input::kLeft},
                 {"RIGHT", input::kRight}, {"HOME", input::kHome}};
    for (const auto& n : names)
        if (name == n.n) return n.m;
    return 0;
}

void load_script() {
    char* env = nullptr;
    size_t len = 0;
    if (_dupenv_s(&env, &len, "MH3U_INPUT_SCRIPT") != 0 || !env) return;
    std::string spec = env;
    std::free(env);
    size_t pos = 0;
    while (pos < spec.size()) {
        const size_t end = std::min(spec.find(';', pos), spec.size());
        const std::string item = spec.substr(pos, end - pos);
        pos = end + 1;
        const size_t colon = item.find(':');
        if (colon == std::string::npos) continue;
        const bool rel = !item.empty() && item[0] == '@';
        ScriptPress p{static_cast<uint32_t>(std::strtoul(item.c_str() + (rel ? 1 : 0), nullptr, 10)), 6, 0};
        p.from_menu = rel;
        if (const size_t plus = item.find('+'); plus < colon) p.frames = static_cast<uint32_t>(std::strtoul(item.c_str() + plus + 1, nullptr, 10));
        std::string names = item.substr(colon + 1);
        for (char& c : names) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        size_t q = 0;
        while (q <= names.size()) {
            const size_t bar = std::min(names.find('|', q), names.size());
            const std::string n = names.substr(q, bar - q);
            p.buttons |= button_mask(n);
            if (n == "LSTICK_LEFT") p.lx = -1;
            if (n == "LSTICK_RIGHT") p.lx = 1;
            if (n == "LSTICK_UP") p.ly = 1;
            if (n == "LSTICK_DOWN") p.ly = -1;
            if (n == "RSTICK_LEFT") p.rx = -1;
            if (n == "RSTICK_RIGHT") p.rx = 1;
            if (n == "RSTICK_UP") p.ry = 1;
            if (n == "RSTICK_DOWN") p.ry = -1;
            q = bar + 1;
        }
        g_script.push_back(p);
    }
    env = nullptr;
    g_script_time = _dupenv_s(&env, &len, "MH3U_INPUT_CLOCK") == 0 && env && std::string(env) == "time";
    std::free(env);
    g_script_start = std::chrono::steady_clock::now();
    std::printf("input: script with %zu presses%s\n", g_script.size(), g_script_time ? " (1/60 s clock)" : "");
}

// MH3U_TOUCH_SCRIPT="frame[+frames]:x,y[>x2,y2];..." touches the GamePad screen (854x480 pixels) at x, y for those
// reads (default 6), or drags from x, y to x2, y2 over them - for headless tests of the touch screen.
struct TouchPress {
    uint32_t frame, frames;
    float x, y, x2, y2;
};
std::vector<TouchPress> g_touch_script;
void load_touch_script() {
    char* env = nullptr;
    size_t len = 0;
    if (_dupenv_s(&env, &len, "MH3U_TOUCH_SCRIPT") != 0 || !env) return;
    const std::string spec = env;
    std::free(env);
    size_t pos = 0;
    while (pos < spec.size()) {
        const size_t end = std::min(spec.find(';', pos), spec.size());
        const std::string item = spec.substr(pos, end - pos);
        pos = end + 1;
        TouchPress t{};
        unsigned f = 0, n = 6;
        if (std::sscanf(item.c_str(), "%u+%u:%f,%f>%f,%f", &f, &n, &t.x, &t.y, &t.x2, &t.y2) == 6 ||
            std::sscanf(item.c_str(), "%u:%f,%f>%f,%f", &f, &t.x, &t.y, &t.x2, &t.y2) == 5) {
        } else if (std::sscanf(item.c_str(), "%u+%u:%f,%f", &f, &n, &t.x, &t.y) == 4 ||
                   (n = 6, std::sscanf(item.c_str(), "%u:%f,%f", &f, &t.x, &t.y) == 3)) {
            t.x2 = t.x, t.y2 = t.y;
        } else {
            continue;
        }
        t.frame = f, t.frames = std::max(1u, n);
        g_touch_script.push_back(t);
    }
    std::printf("input: touch script with %zu touches\n", g_touch_script.size());
}
void apply_touch_script(input::PadState& s, uint32_t frame) {
    for (const TouchPress& t : g_touch_script) {
        if (frame < t.frame || frame >= t.frame + t.frames) continue;
        const float k = t.frames > 1 ? static_cast<float>(frame - t.frame) / static_cast<float>(t.frames - 1) : 0.0f;
        s.touch = true;
        s.touch_x = static_cast<uint16_t>(std::clamp(t.x + (t.x2 - t.x) * k, 0.0f, 853.0f));
        s.touch_y = static_cast<uint16_t>(std::clamp(t.y + (t.y2 - t.y) * k, 0.0f, 479.0f));
    }
}

void apply_script(input::PadState& s, uint32_t frame) {
    if (!g_menu_anchor && core::g_main_menu_opened.load()) {
        g_menu_anchor = frame ? frame : 1;
        if (!g_script.empty()) std::printf("input: main menu open at script frame %u\n", g_menu_anchor);
    }
    static const bool any_from_menu =
        std::any_of(g_script.begin(), g_script.end(), [](const ScriptPress& p) { return p.from_menu; });
    for (const ScriptPress& p : g_script) {
        if (p.from_menu && !g_menu_anchor) continue;
        // with "@" presses, the plain ones only get the game to the main menu (a burst of A through the attract movie
        // and the title) and stop once it is open
        if (!p.from_menu && any_from_menu && g_menu_anchor) continue;
        const uint32_t at = p.from_menu ? g_menu_anchor + p.frame : p.frame;
        if (frame < at || frame >= at + p.frames) continue;
        s.buttons |= p.buttons;
        if (p.lx || p.ly) {
            s.lx = p.lx;
            s.ly = p.ly;
        }
        if (p.rx || p.ry) {
            s.rx = p.rx;
            s.ry = p.ry;
        }
    }
}

void put_float(uint8_t* base, uint32_t a, float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    ppc::st32(base, a, u);
}
// Touch samples hold raw panel values, 12 bits across and down (ours: linear, top-left 0, 4095 at the far edges);
// VPADGetTPCalibratedPoint turns them into 1280x720 screen coordinates as the real one does - the game scales those
// to its 854x480 screen (issue #6: handing it 854x480 pixels put every touch at two thirds of the way).
void put_touch(uint8_t* base, uint32_t a, const input::PadState& s) {
    ppc::st16(base, a, s.touch ? static_cast<uint16_t>(s.touch_x * 4095u / 853u) : 0);
    ppc::st16(base, a + 2, s.touch ? static_cast<uint16_t>(s.touch_y * 4095u / 479u) : 0);
    ppc::st16(base, a + 4, s.touch ? 1 : 0);  // touched
    ppc::st16(base, a + 6, 0);                // validity: both axes valid
}

bool pro_on(uint32_t chan) { return g_pro && chan == 0; }

// One host read for a VPADRead and a KPADReadEx in a row: a reader that already had the current state reads anew
// (who: 0 VPAD, 1 KPAD). A reader alone (the other pad off or not polled) reads anew every time.
std::mutex g_read_m;
input::PadState g_shared;
bool g_had[2] = {true, true};
input::PadState shared_read(int who) {
    std::lock_guard g(g_read_m);
    if (g_had[who]) {
        g_shared = input::read();
        apply_script(g_shared, script_now());
        apply_touch_script(g_shared, script_now());
        input::note_buttons(g_shared.buttons);  // presses for game code (input::take_pressed: cutscene skip)
        input::pass_read(g_shared);  // the on-screen keyboard: its buttons, and an idle pad for the game while it is on
        g_had[0] = g_had[1] = false;
    }
    g_had[who] = true;
    return g_shared;
}

// Pending callbacks run from the game's next VPADRead/KPADReadEx, on its thread. The game polls KPAD only after the
// connect callback reported channel 0 (sPad::WiiPad, connect callback 0x02BC9DCC), and reads Pro Controller
// buttons only once its extension callback (0x02BC9CD4) saw type 31 (URCC), which the connect callback registers.
void deliver_callbacks(const ppc::Context& ctx, uint8_t* base) {
    while (!g_pending.empty()) {
        const PendingCallback cb = g_pending.front();
        g_pending.erase(g_pending.begin());
        ppc::Context c{};
        c.r[1] = (ctx.r[1] - 0x200) & ~0xFu;
        c.r[3] = cb.a;
        c.r[4] = cb.b;
        ppc::call_indirect(c, base, cb.fn);
        if (cb.fn == g_connect_cb && cb.b == 0 && g_extension_cb) g_pending.push_back({g_extension_cb, 0, 31});
    }
}

// s32 VPADRead(VPADChan, VPADStatus* buffers, u32 count, VPADReadError* error) -> samples written
HLE_FUNC(vpad, VPADRead) {
    if (arg(ctx, 2) == 0 || arg(ctx, 0) != 0 || g_no_gamepad) {
        if (arg(ctx, 3)) ppc::st32(base, arg(ctx, 3), static_cast<uint32_t>(-2));  // VPAD_READ_INVALID_CONTROLLER
        ret(ctx, 0);
        if (g_no_gamepad) deliver_callbacks(ctx, base);
        return;
    }
    input::PadState s;
    if (!g_pro || g_vpad_buttons) {
        s = shared_read(0);
    } else {  // MH3U_CONTROLLER=pro: the touch screen only
        s = input::touch_state();
        apply_touch_script(s, script_now());
    }
    const uint32_t st = arg(ctx, 1);
    std::memset(base + st, 0, kVpadStatusSize);
    ppc::st32(base, st, s.buttons);
    ppc::st32(base, st + 4, s.buttons & ~g_prev_hold);
    ppc::st32(base, st + 8, g_prev_hold & ~s.buttons);
    g_prev_hold = s.buttons;
    put_float(base, st + 0x0C, s.lx);
    put_float(base, st + 0x10, s.ly);
    put_float(base, st + 0x14, s.rx);
    put_float(base, st + 0x18, s.ry);
    put_touch(base, st + 0x52, s);
    put_touch(base, st + 0x5A, s);
    put_touch(base, st + 0x62, s);
    base[st + 0xA1] = 6;  // battery: full
    if (arg(ctx, 3)) ppc::st32(base, arg(ctx, 3), 0);  // VPAD_READ_SUCCESS
    ret(ctx, 1);
    deliver_callbacks(ctx, base);
}
HLE_FUNC(vpad, VPADInit) {}
HLE_FUNC(vpad, VPADShutdown) {}
// VPADGetTPCalibratedPoint(chan, VPADTouchData* out, const VPADTouchData* in): raw panel values (put_touch) -> 1280x720
HLE_FUNC(vpad, VPADGetTPCalibratedPoint) {
    const uint32_t out = arg(ctx, 1), in = arg(ctx, 2);
    if (!out || !in) return;
    uint8_t t[8];
    std::memcpy(t, base + in, 8);  // out may be in
    const uint32_t x = (static_cast<uint32_t>(t[0]) << 8 | t[1]) & 0xFFF, y = (static_cast<uint32_t>(t[2]) << 8 | t[3]) & 0xFFF;
    std::memcpy(base + out, t, 8);
    ppc::st16(base, out, static_cast<uint16_t>(x * 1279u / 4095u));
    ppc::st16(base, out + 2, static_cast<uint16_t>(y * 719u / 4095u));
}
HLE_FUNC(vpad, VPADGetLcdMode) {
    if (arg(ctx, 1)) ppc::st32(base, arg(ctx, 1), 1);  // VPAD_LCD_ON
    ret(ctx, 0);
}
// s32 VPADControlMotor(chan, u8* pattern, u8 length): pattern bits are on/off steps; any on -> a short rumble
HLE_FUNC(vpad, VPADControlMotor) {
    const uint32_t len = std::min(arg(ctx, 2) & 0xFF, 120u);
    uint32_t on = 0;
    for (uint32_t i = 0; i < len; ++i) on += (base[arg(ctx, 1) + i / 8] >> (7 - i % 8)) & 1;
    if (on) input::rumble(0.6f, len * 8);
    ret(ctx, 0);
}
HLE_FUNC(vpad, VPADStopMotor) { input::rumble(0.0f, 0); }
#define VPAD_NOP(name) HLE_FUNC(vpad, name) { ret(ctx, 0); }
VPAD_NOP(VPADDisableGyroAccRevise) VPAD_NOP(VPADDisableGyroZeroPlay)
VPAD_NOP(VPADEnableGyroAccRevise) VPAD_NOP(VPADEnableGyroZeroPlay) VPAD_NOP(VPADSetAccParam) VPAD_NOP(VPADSetAccPlayMode)
VPAD_NOP(VPADSetGyroAccReviseParam) VPAD_NOP(VPADSetGyroAngle) VPAD_NOP(VPADSetGyroDirection)
VPAD_NOP(VPADSetGyroDirectionMag) VPAD_NOP(VPADSetGyroMagnification) VPAD_NOP(VPADSetGyroZeroDriftMode)
VPAD_NOP(VPADSetGyroZeroPlayParam)

// s32 WPADProbe(WPADChan, WPADExtensionType* type)
HLE_FUNC(padscore, WPADProbe) {
    const bool on = pro_on(arg(ctx, 0));
    if (arg(ctx, 1)) ppc::st32(base, arg(ctx, 1), on ? 31 : 253);  // URCC / not found
    ret(ctx, on ? 0 : static_cast<uint32_t>(kWpadNoController));
}
// s32 KPADReadEx(KPADChan, KPADStatus* buffers, u32 count, KPADError* error) -> samples
HLE_FUNC(padscore, KPADReadEx) {
    const uint32_t chan = arg(ctx, 0), st = arg(ctx, 1), count = arg(ctx, 2), err = arg(ctx, 3);
    if (!pro_on(chan) || !count || !st) {
        if (err) ppc::st32(base, err, static_cast<uint32_t>(pro_on(chan) ? kKpadNoSamples : -2));  // NO_SAMPLES / NO_CONTROLLER
        ret(ctx, 0);
        return;
    }
    const input::PadState s = shared_read(1);
    const uint32_t hold = pro_buttons(s.buttons);
    std::memset(base + st, 0, 0xF0);
    base[st + 0x5C] = 31;  // devType: URCC
    base[st + 0x5D] = 0;   // WPAD_ERROR_NONE
    base[st + 0x5F] = 22;  // data format: URCC
    ppc::st32(base, st + 0x60, hold);
    ppc::st32(base, st + 0x64, hold & ~g_pro_prev);
    ppc::st32(base, st + 0x68, g_pro_prev & ~hold);
    g_pro_prev = hold;
    put_float(base, st + 0x6C, s.lx);
    put_float(base, st + 0x70, s.ly);
    put_float(base, st + 0x74, s.rx);
    put_float(base, st + 0x78, s.ry);
    ppc::st32(base, st + 0x80, 1);  // cable: wired (no charging state to report)
    if (err) ppc::st32(base, err, 0);
    ret(ctx, 1);
    deliver_callbacks(ctx, base);
}
// s32 WPADGetInfoAsync(chan, WPADInfo* info, callback(chan, result)): {u32 dpd, speaker, attach, lowBat,
// nearempty; u8 battery, led, protocol, firmware}; the callback (chan, result) follows asynchronously
HLE_FUNC(padscore, WPADGetInfoAsync) {
    const uint32_t chan = arg(ctx, 0), info = arg(ctx, 1), cb = arg(ctx, 2);
    const bool on = pro_on(chan);
    if (on && info) {
        std::memset(base + info, 0, 0x18);
        base[info + 0x14] = 4;  // battery full
        base[info + 0x15] = 1;  // LED 1
    }
    const uint32_t result = on ? 0 : static_cast<uint32_t>(kWpadNoController);
    if (cb) g_pending.push_back({cb, chan, result});
    ret(ctx, result);
}
// KPADConnectCallback KPADSetConnectCallback(chan, cb) -> previous
HLE_FUNC(padscore, KPADSetConnectCallback) {
    if (arg(ctx, 0) != 0) {
        ret(ctx, 0);
        return;
    }
    const uint32_t prev = g_connect_cb;
    g_connect_cb = arg(ctx, 1);
    if (g_pro && g_connect_cb) g_pending.push_back({g_connect_cb, 0, 0});  // channel 0 connected
    ret(ctx, prev);
}
// WPADExtensionCallback WPADSetExtensionCallback(chan, cb) -> previous
HLE_FUNC(padscore, WPADSetExtensionCallback) {
    if (arg(ctx, 0) != 0) {
        ret(ctx, 0);
        return;
    }
    const uint32_t prev = g_extension_cb;
    g_extension_cb = arg(ctx, 1);
    ret(ctx, prev);
}
// void WPADControlMotor(chan, BOOL on): Pro Controller rumble
HLE_FUNC(padscore, WPADControlMotor) {
    if (pro_on(arg(ctx, 0))) input::rumble(arg(ctx, 1) ? 0.6f : 0.0f, arg(ctx, 1) ? 1000 : 0);
}
#define PAD_NOP(name) HLE_FUNC(padscore, name) { ret(ctx, 0); }
PAD_NOP(WPADRegisterAllocator) PAD_NOP(WPADSetAcceptConnection) 
PAD_NOP(WPADDisconnect) PAD_NOP(WPADEnableURCC) PAD_NOP(KPADInitEx) PAD_NOP(KPADShutdown) PAD_NOP(KPADReset)
PAD_NOP(KPADSetControlMplsCallback) PAD_NOP(KPADDisableDPD) PAD_NOP(KPADEnableDPD)
PAD_NOP(KPADDisableMplsAccRevise) PAD_NOP(KPADEnableMplsAccRevise) PAD_NOP(KPADDisableMplsZeroPlay)
PAD_NOP(KPADEnableMplsZeroPlay) PAD_NOP(KPADSetAccParam) PAD_NOP(KPADSetAccPlayMode) PAD_NOP(KPADSetMplsAccReviseParam)
PAD_NOP(KPADSetMplsAngle) PAD_NOP(KPADSetMplsDirection) PAD_NOP(KPADSetMplsDirectionMag)
PAD_NOP(KPADSetMplsMagnification) PAD_NOP(KPADSetMplsZeroDriftMode) PAD_NOP(KPADSetMplsZeroPlayParam)

}  // namespace

void register_input(Runtime&) {
    load_script();
    load_touch_script();
    char* env = nullptr;
    size_t len = 0;
    if (_dupenv_s(&env, &len, "MH3U_CONTROLLER") == 0 && env) {
        const std::string mode = env;
        g_pro = mode != "gamepad";
        g_no_gamepad = mode == "pro-only";
        g_vpad_buttons = g_pro && mode != "pro" && mode != "pro-only";
    }
    std::free(env);
    input::set_both_controllers(g_pro && g_vpad_buttons);
    std::printf("input: host controller is %s\n", !g_pro            ? "the GamePad"
                                                 : g_no_gamepad    ? "a Pro Controller (WPAD channel 0), no GamePad"
                                                 : g_vpad_buttons  ? "a Pro Controller (WPAD channel 0) and the GamePad (both)"
                                                                   : "a Pro Controller (WPAD channel 0) + GamePad touch screen");
#define RV(name) HLE_REGISTER(vpad, name);
#define RP(name) HLE_REGISTER(padscore, name);
    RV(VPADRead) RV(VPADInit) RV(VPADShutdown) RV(VPADGetTPCalibratedPoint) RV(VPADGetLcdMode) RV(VPADControlMotor)
    RV(VPADStopMotor) RV(VPADDisableGyroAccRevise) RV(VPADDisableGyroZeroPlay) RV(VPADEnableGyroAccRevise)
    RV(VPADEnableGyroZeroPlay) RV(VPADSetAccParam) RV(VPADSetAccPlayMode) RV(VPADSetGyroAccReviseParam)
    RV(VPADSetGyroAngle) RV(VPADSetGyroDirection) RV(VPADSetGyroDirectionMag) RV(VPADSetGyroMagnification)
    RV(VPADSetGyroZeroDriftMode) RV(VPADSetGyroZeroPlayParam)
    RP(WPADProbe) RP(KPADReadEx) RP(WPADGetInfoAsync) RP(WPADRegisterAllocator) RP(WPADSetAcceptConnection)
    RP(WPADSetExtensionCallback) RP(WPADControlMotor) RP(WPADDisconnect) RP(WPADEnableURCC) RP(KPADInitEx)
    RP(KPADShutdown) RP(KPADReset) RP(KPADSetConnectCallback) RP(KPADSetControlMplsCallback) RP(KPADDisableDPD)
    RP(KPADEnableDPD) RP(KPADDisableMplsAccRevise) RP(KPADEnableMplsAccRevise) RP(KPADDisableMplsZeroPlay)
    RP(KPADEnableMplsZeroPlay) RP(KPADSetAccParam) RP(KPADSetAccPlayMode) RP(KPADSetMplsAccReviseParam)
    RP(KPADSetMplsAngle) RP(KPADSetMplsDirection) RP(KPADSetMplsDirectionMag) RP(KPADSetMplsMagnification)
    RP(KPADSetMplsZeroDriftMode) RP(KPADSetMplsZeroPlayParam)
#undef RV
#undef RP
}

}  // namespace mh3u::os
