// erreula (error viewer) and swkbd (software keyboard): system applets the game loads with OSDynLoad.
//
// erreula: no UI; creation succeeds, the viewer never appears (state 0 = hidden), nothing is ever decided.
//
// swkbd: there is no on-screen keyboard – text comes from the PC keyboard (input::begin_text: type, Backspace,
// Enter = OK, Esc = cancel; the window title shows the text). Layouts follow Cemu's swkbd (MPL-2.0, read for the
// structures): AppearArg initialText +0xC8 (UTF-16BE), maxTextLength +0xD0; KeyboardArg receiver at +0xA8;
// ReceiverArg {IEventReceiver*, u16* stringBuf, s32 stringBufSize (UTF-16 units incl. terminator), ...}; an
// IEventReceiver's vtable +0xC is its "string changed" callback (receiver, {u32 begin, u32 end}). States: 0 hidden,
// 2 displayed. The game polls SwkbdCalc every frame on its own thread: text changes are written to the receiver's
// buffer and its callback is called there.
#include "hle.h"

#include "host_input.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace mh3u::os {
namespace {

constexpr uint32_t kFormChars = 4096;  // UTF-16 units of the GetInputFormString buffer

struct Swkbd {
    bool active = false;
    uint32_t receiver = 0, buf = 0, buf_size = 0;  // SwkbdSetReceiver / AppearKeyboard
    uint32_t form = 0;                             // guest buffer returned by SwkbdGetInputFormString
    uint32_t param = 0;                            // guest {begin, end} for the change callback
    uint32_t pushed = 0;                           // text revision last written to the receiver
    int font_work = 0, predict_work = 0;           // pending "sub-thread" calls after create (as in Cemu)
} g_kbd;

std::u16string read_u16(const uint8_t* base, uint32_t a, size_t max) {
    std::u16string s;
    if (!a) return s;
    for (size_t i = 0; !max || i < max; ++i) {
        const char16_t c = static_cast<char16_t>(ppc::ld16(base, a + static_cast<uint32_t>(2 * i)));
        if (!c) break;
        s += c;
    }
    return s;
}

void write_u16(uint8_t* base, uint32_t a, const std::u16string& s, size_t cap) {
    const size_t n = std::min(s.size(), cap ? cap - 1 : 0);
    for (size_t i = 0; i < n; ++i) ppc::st16(base, a + static_cast<uint32_t>(2 * i), s[i]);
    if (cap) ppc::st16(base, a + static_cast<uint32_t>(2 * n), 0);
}

#define SWKBD(name, sym) static void hle_swkbd_##name([[maybe_unused]] ppc::Context& ctx, [[maybe_unused]] uint8_t* base)

// BOOL SwkbdCreate(u8* work, RegionType, u32, FSClient*). Takes about a second, as the real one loading its
// resources: Cemu found MH3U soft-locks if it returns at once.
SWKBD(Create, ) {
    std::printf("swkbd: created\n");
    std::this_thread::sleep_for(std::chrono::seconds(1));
    g_kbd.font_work = g_kbd.predict_work = 3;  // the game's sub-thread polls these before using the keyboard
    if (!g_kbd.form) {
        g_kbd.form = rt().mem->alloc_hle(kFormChars * 2, 4);
        g_kbd.param = rt().mem->alloc_hle(8, 4);
    }
    ret(ctx, 1);
}
SWKBD(Destroy, ) {
    input::end_text();
    g_kbd.active = false;
    ret(ctx, 0);
}
// BOOL SwkbdAppearInputForm(const AppearArg&)
SWKBD(AppearInputForm, ) {
    const uint32_t a = arg(ctx, 0);
    uint32_t max = ppc::ld32(base, a + 0xD0);
    if (!max || max >= kFormChars) max = kFormChars - 1;
    input::begin_text(read_u16(base, ppc::ld32(base, a + 0xC8), max), max);
    std::printf("swkbd: input form opened (max %u characters)\n", max);
    g_kbd.active = true;
    g_kbd.pushed = 0;
    ret(ctx, 1);
}
// BOOL SwkbdAppearKeyboard(const KeyboardArg&): keyboard only, text goes to the receiver
SWKBD(AppearKeyboard, ) {
    const uint32_t r = arg(ctx, 0) + 0xA8;
    g_kbd.receiver = ppc::ld32(base, r);
    g_kbd.buf = ppc::ld32(base, r + 4);
    g_kbd.buf_size = ppc::ld32(base, r + 8);
    input::begin_text(u"", g_kbd.buf_size > 1 ? g_kbd.buf_size - 1 : 0);
    std::printf("swkbd: keyboard opened (buffer %u characters)\n", g_kbd.buf_size);
    g_kbd.active = true;
    g_kbd.pushed = 0;
    ret(ctx, 1);
}
SWKBD(DisappearInputForm, ) {
    input::end_text();
    g_kbd.active = false;
    ret(ctx, 1);
}
SWKBD(DisappearKeyboard, ) {
    input::end_text();
    g_kbd.active = false;
    ret(ctx, 1);
}
// BOOL SwkbdSetReceiver(const ReceiverArg&)
SWKBD(SetReceiver, ) {
    const uint32_t r = arg(ctx, 0);
    g_kbd.receiver = ppc::ld32(base, r);
    g_kbd.buf = ppc::ld32(base, r + 4);
    g_kbd.buf_size = ppc::ld32(base, r + 8);
    ret(ctx, 0);
}
// void SwkbdCalc(const ControllerInfo&): per frame on the game's thread – push typed text to the receiver
SWKBD(Calc, ) {
    if (!g_kbd.active) return;
    const input::TextEntry t = input::text_state();
    if (t.revision == g_kbd.pushed) return;
    g_kbd.pushed = t.revision;
    if (!g_kbd.buf || g_kbd.buf_size < 1) return;
    write_u16(base, g_kbd.buf, t.text, g_kbd.buf_size);
    if (!g_kbd.receiver) return;
    const uint32_t vtable = ppc::ld32(base, g_kbd.receiver);
    const uint32_t changed = vtable ? ppc::ld32(base, vtable + 0xC) : 0;
    if (!changed) return;
    ppc::st32(base, g_kbd.param, 0);
    ppc::st32(base, g_kbd.param + 4, 0);
    ppc::Context c = ctx;
    c.r[3] = g_kbd.receiver;
    c.r[4] = g_kbd.param;
    ppc::call_indirect(c, base, changed);
}
SWKBD(GetStateInputForm, ) { ret(ctx, g_kbd.active ? 2 : 0); }
SWKBD(GetStateKeyboard, ) { ret(ctx, g_kbd.active ? 2 : 0); }
// BOOL SwkbdIsDecideOkButton(bool*) / SwkbdIsDecideCancelButton(bool*)
SWKBD(IsDecideOkButton, ) { ret(ctx, g_kbd.active && input::text_state().done ? 1 : 0); }
SWKBD(IsDecideCancelButton, ) { ret(ctx, g_kbd.active && input::text_state().cancelled ? 1 : 0); }
// const char16_t* SwkbdGetInputFormString()
SWKBD(GetInputFormString, ) {
    write_u16(base, g_kbd.form, input::text_state().text, kFormChars);
    ret(ctx, g_kbd.form);
}
// BOOL SwkbdIsNeedCalcSubThreadFont() / ...Predict(); void SwkbdCalcSubThreadFont() / ...Predict(): a little
// pretend work after create, each step ~5 ms
SWKBD(IsNeedCalcSubThreadFont, ) { ret(ctx, g_kbd.font_work > 0 ? 1 : 0); }
SWKBD(IsNeedCalcSubThreadPredict, ) { ret(ctx, g_kbd.predict_work > 0 ? 1 : 0); }
SWKBD(CalcSubThreadFont, ) {
    if (g_kbd.font_work <= 0) return;
    --g_kbd.font_work;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
}
SWKBD(CalcSubThreadPredict, ) {
    if (g_kbd.predict_work <= 0) return;
    --g_kbd.predict_work;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
}
// BOOL SwkbdInitLearnDic(void* dic): an empty learning dictionary (0xA460 bytes, "NJDC" header and trailer; field
// values as Cemu documents them). The game needs this to succeed before it opens the keyboard.
SWKBD(InitLearnDic, ) {
    const uint32_t d = arg(ctx, 0);
    if (!d || !g_kbd.form) {
        ret(ctx, 0);
        return;
    }
    constexpr uint32_t kSize = 0xA460, kEntries = 1000, kStride = 0x20, kMagic = 0x4E4A4443;  // "NJDC"
    const uint32_t first = 0x48, second = first + (kEntries + 1) * 2, index_end = second + (kEntries + 1) * 2;
    std::memset(base + d, 0, kSize);
    const uint32_t fields[][2] = {{0x00, kMagic},      {0x04, 0x30000},    {0x08, 0x80020000}, {0x0C, kSize - 0x48},
                                  {0x10, 0x2C},        {0x14, 0x6E},       {0x18, 0x6E},       {0x1C, index_end},
                                  {0x28, kEntries},    {0x2C, kStride},    {0x3C, first},      {0x40, second},
                                  {0x44, index_end + kEntries * kStride}, {kSize - 4, kMagic}};
    for (const auto& f : fields) ppc::st32(base, d + f[0], f[1]);
    ret(ctx, 1);
}
// void SwkbdGetDrawStringInfo(DrawStringInfo*): no drawn string (all -1, as in Cemu)
SWKBD(GetDrawStringInfo, ) {
    const uint32_t p = arg(ctx, 0);
    for (uint32_t i = 0; i < 6; ++i) ppc::st32(base, p + 4 * i, 0xFFFFFFFFu);
    base[p + 0x18] = 0;
}

}  // namespace

void register_applets(Runtime&) {
    const char* erreula_created[] = {
        "erreula.ErrEulaCreate__3RplFPUcQ3_2nn7erreula10RegionTypeQ3_2nn7erreula8LangTypeP8FSClient",
    };
    const char* idle[] = {
        "erreula.ErrEulaAppearError__3RplFRCQ3_2nn7erreula9AppearArg",
        "erreula.ErrEulaAppearHomeNixSign__3RplFRCQ3_2nn7erreula14HomeNixSignArg",
        "erreula.ErrEulaCalc__3RplFRCQ3_2nn7erreula14ControllerInfo",
        "erreula.ErrEulaChangeLang__3RplFQ3_2nn7erreula8LangType",
        "erreula.ErrEulaDestroy__3RplFv",
        "erreula.ErrEulaDisappearError__3RplFv",
        "erreula.ErrEulaDisappearHomeNixSign__3RplFv",
        "erreula.ErrEulaDrawDRC__3RplFv",
        "erreula.ErrEulaDrawTV__3RplFv",
        "erreula.ErrEulaGetResultCode__3RplFv",
        "erreula.ErrEulaGetResultType__3RplFv",
        "erreula.ErrEulaGetSelectButtonNumError__3RplFv",
        "erreula.ErrEulaGetStateErrorViewer__3RplFv",
        "erreula.ErrEulaIsAppearHomeNixSign__3RplFv",
        "erreula.ErrEulaIsDecideSelectButtonError__3RplFv",
        "erreula.ErrEulaIsDecideSelectLeftButtonError__3RplFv",
        "erreula.ErrEulaIsDecideSelectRightButtonError__3RplFv",
        "erreula.ErrEulaIsSelectCursorActive__3RplFv",
        "erreula.ErrEulaSetControllerRemo__3RplFQ3_2nn7erreula14ControllerType",
        // swkbd calls with nothing to do: no on-screen keyboard is drawn or navigated
        "swkbd.SwkbdConfirmUnfixAll__3RplFv",
        "swkbd.SwkbdDrawDRC__3RplFv",
        "swkbd.SwkbdDrawTV__3RplFv",
        "swkbd.SwkbdGetKeyboardCondition__3RplFPQ3_2nn5swkbd17KeyboardCondition",
        "swkbd.SwkbdInactivateSelectCursor__3RplFv",
        "swkbd.SwkbdIsCoveredWithSubWindow__3RplFv",
        "swkbd.SwkbdIsKeyboardTarget__3RplFPQ3_2nn5swkbd14IEventReceiver",
        "swkbd.SwkbdIsSelectCursorActive__3RplFv",
        "swkbd.SwkbdSetControllerRemo__3RplFQ3_2nn5swkbd14ControllerType",
        "swkbd.SwkbdSetCursorPos__3RplFi",
        "swkbd.SwkbdSetEnableOkButton__3RplFb",
        "swkbd.SwkbdSetSelectFrom__3RplFi",
        "swkbd.SwkbdSetUserControllerEventObj__3RplFPQ3_2nn5swkbd19IControllerEventObj",
        "swkbd.SwkbdSetUserSoundObj__3RplFPQ3_2nn5swkbd9ISoundObj",
    };
    for (const char* n : erreula_created) ppc::register_import(n, [](ppc::Context& c, uint8_t*) { ret(c, 1); });
    for (const char* n : idle) ppc::register_import(n, [](ppc::Context& c, uint8_t*) { ret(c, 0); });
#define R(name, sym) ppc::register_import("swkbd." sym, &hle_swkbd_##name);
    R(Create, "SwkbdCreate__3RplFPUcQ3_2nn5swkbd10RegionTypeUiP8FSClient")
    R(Destroy, "SwkbdDestroy__3RplFv")
    R(AppearInputForm, "SwkbdAppearInputForm__3RplFRCQ3_2nn5swkbd9AppearArg")
    R(AppearKeyboard, "SwkbdAppearKeyboard__3RplFRCQ3_2nn5swkbd11KeyboardArg")
    R(DisappearInputForm, "SwkbdDisappearInputForm__3RplFv")
    R(DisappearKeyboard, "SwkbdDisappearKeyboard__3RplFv")
    R(SetReceiver, "SwkbdSetReceiver__3RplFRCQ3_2nn5swkbd11ReceiverArg")
    R(Calc, "SwkbdCalc__3RplFRCQ3_2nn5swkbd14ControllerInfo")
    R(GetStateInputForm, "SwkbdGetStateInputForm__3RplFv")
    R(GetStateKeyboard, "SwkbdGetStateKeyboard__3RplFv")
    R(IsDecideOkButton, "SwkbdIsDecideOkButton__3RplFPb")
    R(IsDecideCancelButton, "SwkbdIsDecideCancelButton__3RplFPb")
    R(GetInputFormString, "SwkbdGetInputFormString__3RplFv")
    R(GetDrawStringInfo, "SwkbdGetDrawStringInfo__3RplFPQ3_2nn5swkbd14DrawStringInfo")
    R(IsNeedCalcSubThreadFont, "SwkbdIsNeedCalcSubThreadFont__3RplFv")
    R(IsNeedCalcSubThreadPredict, "SwkbdIsNeedCalcSubThreadPredict__3RplFv")
    R(CalcSubThreadFont, "SwkbdCalcSubThreadFont__3RplFv")
    R(CalcSubThreadPredict, "SwkbdCalcSubThreadPredict__3RplFv")
    R(InitLearnDic, "SwkbdInitLearnDic__3RplFPv")
#undef R
}

}  // namespace mh3u::os
