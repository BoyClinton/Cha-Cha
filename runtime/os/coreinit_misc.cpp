// coreinit odds and ends: IM (screen dimming), IPC buffer pools, app switching, and OSAlarm.
// Alarms are real: one host thread services all of them and calls the guest callback(OSAlarm*, OSContext*) on its
// own guest stack, like Cafe OS runs alarm callbacks in an interrupt context.
#include "hle.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

namespace mh3u::os {
namespace {

// ---- IM: dimming is disabled; parameters read as 0
HLE_FUNC(coreinit, IM_Open) { ret(ctx, 1); }
HLE_FUNC(coreinit, IM_Close) { ret(ctx, 0); }
// IMError IM_GetParameter(handle, ipcbuf, IMParameter, u32* value, ...)
HLE_FUNC(coreinit, IM_GetParameter) {
    if (arg(ctx, 3)) ppc::st32(base, arg(ctx, 3), 0);
    ret(ctx, 0);
}
HLE_FUNC(coreinit, IM_SetDeviceState) { ret(ctx, 0); }
HLE_FUNC(coreinit, IMDisableDim) { ret(ctx, 0); }
HLE_FUNC(coreinit, IMEnableDim) { ret(ctx, 0); }
// IMError IMIsDimEnabled(BOOL* out)
HLE_FUNC(coreinit, IMIsDimEnabled) {
    if (arg(ctx, 0)) ppc::st32(base, arg(ctx, 0), 0);
    ret(ctx, 0);
}

// ---- IPC buffer pools: void* IPCBufPoolCreate(void* buf, u32 size, u32 msgSize, u32* numMsgs, u32 flags)
struct Pool {
    uint32_t start, end, msg, next;
};
std::mutex g_pool_m;
std::map<uint32_t, Pool> g_pools;
HLE_FUNC(coreinit, IPCBufPoolCreate) {
    const uint32_t buf = arg(ctx, 0), size = arg(ctx, 1), msg = std::max(1u, arg(ctx, 2));
    std::lock_guard g(g_pool_m);
    g_pools[buf] = {buf, buf + size, msg, buf};
    if (arg(ctx, 3)) ppc::st32(base, arg(ctx, 3), size / msg);
    ret(ctx, buf);
}
// void* IPCBufPoolAllocate(pool, u32 size): bump allocation, recycled wholesale when exhausted
HLE_FUNC(coreinit, IPCBufPoolAllocate) {
    std::lock_guard g(g_pool_m);
    auto it = g_pools.find(arg(ctx, 0));
    if (it == g_pools.end()) { ret(ctx, 0); return; }
    Pool& p = it->second;
    const uint32_t size = (std::max(arg(ctx, 1), p.msg) + 63) & ~63u;
    if (p.next + size > p.end) p.next = p.start;
    ret(ctx, p.next + size <= p.end ? p.next : 0);
    p.next += size;
}
HLE_FUNC(coreinit, IPCBufPoolFree) { ret(ctx, 0); }

// ---- app switching (HOME menu etc.) is not modelled
HLE_FUNC(coreinit, OSSendAppSwitchRequest) { ret(ctx, 0); }

// ---- OSAlarm
struct Alarm {
    uint64_t due = 0;        // host steady-clock nanoseconds
    uint64_t period = 0;     // 0 = one-shot
    uint32_t callback = 0;
    uint32_t user_data = 0;
    uint32_t tag = 0;
    bool armed = false;
};
std::mutex g_alarm_m;
std::condition_variable g_alarm_cv;
std::map<uint32_t, Alarm> g_alarms;  // by OSAlarm address
std::thread g_alarm_thread;
uint32_t g_alarm_stack_top = 0, g_alarm_context = 0;
constexpr double kNsPerTick = 1e9 / 62156250.0;

uint64_t now_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

void alarm_service() {
    std::unique_lock l(g_alarm_m);
    for (;;) {
        uint64_t next = UINT64_MAX;
        uint32_t fire = 0;
        for (auto& [addr, a] : g_alarms)
            if (a.armed && a.due < next) {
                next = a.due;
                fire = addr;
            }
        if (!fire) {
            g_alarm_cv.wait(l);
            continue;
        }
        const uint64_t now = now_ns();
        if (next > now) {
            g_alarm_cv.wait_for(l, std::chrono::nanoseconds(next - now));
            continue;
        }
        Alarm& a = g_alarms[fire];
        const uint32_t cb = a.callback;
        if (a.period) a.due += a.period;
        else a.armed = false;
        l.unlock();
        ppc::Context ctx{};
        ctx.r[1] = g_alarm_stack_top - 0x20;
        ctx.r[3] = fire;
        ctx.r[4] = g_alarm_context;
        ppc::call_indirect(ctx, gbase(), cb);
        l.lock();
    }
}

void arm(uint32_t alarm, uint64_t delay_ticks, uint64_t period_ticks, uint32_t callback) {
    {
        std::lock_guard g(g_alarm_m);
        Alarm& a = g_alarms[alarm];
        a.due = now_ns() + static_cast<uint64_t>(static_cast<double>(delay_ticks) * kNsPerTick);
        a.period = static_cast<uint64_t>(static_cast<double>(period_ticks) * kNsPerTick);
        a.callback = callback;
        a.armed = true;
        if (!g_alarm_thread.joinable()) {
            g_alarm_thread = std::thread(alarm_service);
            g_alarm_thread.detach();
        }
    }
    g_alarm_cv.notify_all();
}

// void OSCreateAlarm(OSAlarm*)
HLE_FUNC(coreinit, OSCreateAlarm) {
    std::lock_guard g(g_alarm_m);
    g_alarms[arg(ctx, 0)] = {};
    ppc::st32(base, arg(ctx, 0), 0x614C724D);  // 'aLrM'
}
// BOOL OSSetAlarm(OSAlarm*, OSTime delay /* r5:r6 */, OSAlarmCallback /* r7 */)
HLE_FUNC(coreinit, OSSetAlarm) {
    const uint64_t delay = (uint64_t{ctx.r[5]} << 32) | ctx.r[6];
    arm(arg(ctx, 0), delay, 0, ctx.r[7]);
    ret(ctx, 1);
}
// BOOL OSCancelAlarm(OSAlarm*)
HLE_FUNC(coreinit, OSCancelAlarm) {
    std::lock_guard g(g_alarm_m);
    g_alarms[arg(ctx, 0)].armed = false;
    ret(ctx, 1);
}
// void OSCancelAlarms(u32 tag)
HLE_FUNC(coreinit, OSCancelAlarms) {
    std::lock_guard g(g_alarm_m);
    for (auto& [addr, a] : g_alarms)
        if (a.tag == arg(ctx, 0)) a.armed = false;
}
HLE_FUNC(coreinit, OSSetAlarmUserData) {
    std::lock_guard g(g_alarm_m);
    g_alarms[arg(ctx, 0)].user_data = arg(ctx, 1);
}
HLE_FUNC(coreinit, OSGetAlarmUserData) {
    std::lock_guard g(g_alarm_m);
    ret(ctx, g_alarms[arg(ctx, 0)].user_data);
}

}  // namespace

void register_coreinit_misc(Runtime& r) {
    // alarm callbacks: 64 KB guest stack at the top of the stack region, a zeroed OSContext
    g_alarm_stack_top = core::layout::kStackEnd;
    g_alarm_context = r.mem->alloc_hle(0x320, 8);
    HLE_REGISTER(coreinit, IM_Open);
    HLE_REGISTER(coreinit, IM_Close);
    HLE_REGISTER(coreinit, IM_GetParameter);
    HLE_REGISTER(coreinit, IM_SetDeviceState);
    HLE_REGISTER(coreinit, IMDisableDim);
    HLE_REGISTER(coreinit, IMEnableDim);
    HLE_REGISTER(coreinit, IMIsDimEnabled);
    HLE_REGISTER(coreinit, IPCBufPoolCreate);
    HLE_REGISTER(coreinit, IPCBufPoolAllocate);
    HLE_REGISTER(coreinit, IPCBufPoolFree);
    HLE_REGISTER(coreinit, OSSendAppSwitchRequest);
    HLE_REGISTER(coreinit, OSCreateAlarm);
    HLE_REGISTER(coreinit, OSSetAlarm);
    HLE_REGISTER(coreinit, OSCancelAlarm);
    HLE_REGISTER(coreinit, OSCancelAlarms);
    HLE_REGISTER(coreinit, OSSetAlarmUserData);
    HLE_REGISTER(coreinit, OSGetAlarmUserData);
}

}  // namespace mh3u::os
