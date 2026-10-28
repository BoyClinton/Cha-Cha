// coreinit time and system info, plus OSReport/OSPanic (guest printf).
// Timer: Espresso bus clock 248.625 MHz, timebase = bus / 4 = 62,156,250 ticks/s. OSTime counts from 2000-01-01.
#include "hle.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>

namespace mh3u::os {
namespace {

constexpr uint64_t kBusClock = 248625000, kCoreClock = 1243125000, kTicksPerSec = kBusClock / 4;
const auto g_boot = std::chrono::steady_clock::now();

int64_t ticks_since_boot() {
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - g_boot).count();
    return static_cast<int64_t>(static_cast<double>(ns) * kTicksPerSec / 1e9);
}
int64_t ticks_since_2000() {
    using namespace std::chrono;
    const int64_t unix_2000 = 946684800;
    auto ns = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count() - unix_2000 * 1000000000ll;
    return static_cast<int64_t>(static_cast<double>(ns) * kTicksPerSec / 1e9);
}
void ret64(ppc::Context& ctx, int64_t v) {
    ctx.r[3] = static_cast<uint32_t>(static_cast<uint64_t>(v) >> 32);
    ctx.r[4] = static_cast<uint32_t>(v);
}
int64_t arg64(const ppc::Context& ctx, int first_gpr) {  // 64-bit args take an aligned register pair (r3:r4, r5:r6..)
    return static_cast<int64_t>((uint64_t{ctx.r[first_gpr]} << 32) | ctx.r[first_gpr + 1]);
}

uint32_t g_system_info = 0;

HLE_FUNC(coreinit, OSGetTime) { ret64(ctx, ticks_since_2000()); }
HLE_FUNC(coreinit, OSGetSystemTime) { ret64(ctx, ticks_since_boot()); }
// OSSystemInfo* OSGetSystemInfo(): {u32 busClockSpeed, u32 coreClockSpeed, s64 baseTime, ...}
HLE_FUNC(coreinit, OSGetSystemInfo) { ret(ctx, g_system_info); }
// void OSSleepTicks(OSTime ticks)
HLE_FUNC(coreinit, OSSleepTicks) {
    int64_t t = arg64(ctx, 3);
    if (t > 0) std::this_thread::sleep_for(std::chrono::nanoseconds(static_cast<int64_t>(t * 1e9 / kTicksPerSec)));
}
// void OSTicksToCalendarTime(OSTime, OSCalendarTime*): {sec, min, hour, mday, mon, year, wday, yday, msec, usec}
HLE_FUNC(coreinit, OSTicksToCalendarTime) {
    int64_t ticks = arg64(ctx, 3);
    uint32_t out = arg(ctx, 2);
    int64_t total_us = static_cast<int64_t>(static_cast<double>(ticks) * 1e6 / kTicksPerSec);
    std::time_t secs = static_cast<std::time_t>(946684800 + total_us / 1000000);
    std::tm tm{};
    gmtime_s(&tm, &secs);
    const int32_t v[] = {tm.tm_sec, tm.tm_min, tm.tm_hour, tm.tm_mday, tm.tm_mon, tm.tm_year + 1900, tm.tm_wday,
                         tm.tm_yday, static_cast<int32_t>((total_us / 1000) % 1000), static_cast<int32_t>(total_us % 1000)};
    for (int i = 0; i < 10; ++i) ppc::st32(base, out + 4 * i, static_cast<uint32_t>(v[i]));
}
// OSTime OSCalendarTimeToTicks(const OSCalendarTime*)
HLE_FUNC(coreinit, OSCalendarTimeToTicks) {
    uint32_t in = arg(ctx, 0);
    auto f = [&](int i) { return static_cast<int32_t>(ppc::ld32(base, in + 4 * i)); };
    std::tm tm{};
    tm.tm_sec = f(0); tm.tm_min = f(1); tm.tm_hour = f(2); tm.tm_mday = f(3); tm.tm_mon = f(4); tm.tm_year = f(5) - 1900;
    int64_t secs = static_cast<int64_t>(_mkgmtime(&tm)) - 946684800;
    int64_t us = secs * 1000000 + f(8) * 1000 + f(9);
    ret64(ctx, static_cast<int64_t>(static_cast<double>(us) * kTicksPerSec / 1e6));
}

// Minimal guest printf: integers from r(first_gpr).., doubles from f1.., then the stack parameter area.
std::string guest_format(const ppc::Context& ctx, uint32_t fmt, int first_gpr) {
    std::string out, f = guest_cstr(fmt);
    int gpr = first_gpr, fpr = 1;
    uint32_t stack = ctx.r[1] + 8;
    auto next_int = [&]() -> uint32_t {
        if (gpr <= 10) return ctx.r[gpr++];
        uint32_t v = ppc::ld32(gbase(), stack);
        stack += 4;
        return v;
    };
    auto next_double = [&]() -> double {
        if (fpr <= 8) return ctx.f[fpr++].ps0;
        stack = (stack + 7) & ~7u;
        double v = ppc::ldf64(gbase(), stack);
        stack += 8;
        return v;
    };
    for (size_t i = 0; i < f.size(); ++i) {
        if (f[i] != '%') {
            out += f[i];
            continue;
        }
        size_t j = i + 1;
        while (j < f.size() && std::strchr("-+ #0123456789.", f[j])) ++j;
        int longs = 0;
        while (j < f.size() && (f[j] == 'l' || f[j] == 'h' || f[j] == 'z')) longs += f[j++] == 'l';
        if (j >= f.size()) break;
        std::string spec = f.substr(i, j - i);
        char c = f[j];
        char buf[512];
        buf[0] = 0;
        switch (c) {
            case '%': std::snprintf(buf, sizeof buf, "%%"); break;
            case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c':
                if (longs >= 2) {
                    if (gpr % 2 == 0) ++gpr;  // 64-bit values use an odd/even register pair
                    uint64_t hi = next_int(), lo = next_int();
                    std::snprintf(buf, sizeof buf, (spec + "ll" + c).c_str(), (hi << 32) | lo);
                } else {
                    std::snprintf(buf, sizeof buf, (spec + c).c_str(), next_int());
                }
                break;
            case 'p': std::snprintf(buf, sizeof buf, "0x%08X", next_int()); break;
            case 's': {
                std::string s = guest_cstr(next_int());
                std::snprintf(buf, sizeof buf, (spec + "s").c_str(), s.c_str());
                break;
            }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
                std::snprintf(buf, sizeof buf, (spec + c).c_str(), next_double());
                break;
            default: std::snprintf(buf, sizeof buf, "%s", (spec + c).c_str()); break;
        }
        out += buf;
        i = j;
    }
    return out;
}

HLE_FUNC(coreinit, OSReport) {
    std::string s = guest_format(ctx, arg(ctx, 0), 4);
    std::fprintf(stdout, "[OSReport] %s%s", s.c_str(), (!s.empty() && s.back() == '\n') ? "" : "\n");
}
// void OSPanic(const char* file, int line, const char* fmt, ...)
HLE_FUNC(coreinit, OSPanic) {
    std::string s = guest_format(ctx, arg(ctx, 2), 6);
    std::fprintf(stderr, "[OSPanic] %s:%u: %s\n", guest_cstr(arg(ctx, 0)).c_str(), arg(ctx, 1), s.c_str());
    throw GuestExit{-1};
}

}  // namespace

void register_coreinit_time(Runtime& r) {
    g_system_info = r.mem->alloc_hle(0x20, 8);
    ppc::st32(gbase(), g_system_info, static_cast<uint32_t>(kBusClock));
    ppc::st32(gbase(), g_system_info + 4, static_cast<uint32_t>(kCoreClock));
    HLE_REGISTER(coreinit, OSGetTime);
    HLE_REGISTER(coreinit, OSGetSystemTime);
    HLE_REGISTER(coreinit, OSGetSystemInfo);
    HLE_REGISTER(coreinit, OSSleepTicks);
    HLE_REGISTER(coreinit, OSTicksToCalendarTime);
    HLE_REGISTER(coreinit, OSCalendarTimeToTicks);
    HLE_REGISTER(coreinit, OSReport);
    HLE_REGISTER(coreinit, OSPanic);
}

}  // namespace mh3u::os
