// coreinit synchronisation: OSMutex, OSCond, OSEvent, OSSemaphore, OSMessageQueue – real blocking on host
// primitives, keyed by the guest object's address (guest code treats the objects as opaque after init).
// Guest threads are host threads (coreinit_thread.cpp), so these block and wake exactly like on Cafe OS
// (priorities aside).
#include "hle.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace mh3u::os {
namespace {

constexpr double kTicksPerNs = 62156250.0 / 1e9;

struct Mutex {
    std::recursive_mutex m;
};
struct Event {
    std::mutex m;
    std::condition_variable cv;
    bool signalled = false;
    bool auto_reset = false;
};
struct Cond {
    std::condition_variable_any cv;
};
struct Sema {
    std::mutex m;
    std::condition_variable cv;
    int32_t count = 0;
};
struct Message {
    uint32_t w[4];
};
struct Queue {
    std::mutex m;
    std::condition_variable cv;
    std::deque<Message> q;
    uint32_t capacity = 0;
};

template <class T>
T& obj(uint32_t addr) {
    static std::mutex table_lock;
    static std::unordered_map<uint32_t, std::unique_ptr<T>> table;
    std::lock_guard g(table_lock);
    auto& p = table[addr];
    if (!p) p = std::make_unique<T>();
    return *p;
}
template <class T>
void reset(uint32_t addr) {
    obj<T>(addr).~T();
    new (&obj<T>(addr)) T();
}

// ---- OSMutex (recursive, as on Cafe OS)
HLE_FUNC(coreinit, OSInitMutex) { ppc::st32(base, arg(ctx, 0), 0x6D557478); obj<Mutex>(arg(ctx, 0)); }  // 'mUtx'
HLE_FUNC(coreinit, OSInitMutexEx) {
    ppc::st32(base, arg(ctx, 0), 0x6D557478);
    ppc::st32(base, arg(ctx, 0) + 4, arg(ctx, 1));
    obj<Mutex>(arg(ctx, 0));
}
HLE_FUNC(coreinit, OSLockMutex) {
    Mutex& mx = obj<Mutex>(arg(ctx, 0));
    if (mx.m.try_lock()) return;
    WaitScope w("OSLockMutex", arg(ctx, 0), ctx.lr);
    mx.m.lock();
}
HLE_FUNC(coreinit, OSTryLockMutex) { ret(ctx, obj<Mutex>(arg(ctx, 0)).m.try_lock() ? 1 : 0); }
HLE_FUNC(coreinit, OSUnlockMutex) { obj<Mutex>(arg(ctx, 0)).m.unlock(); }

// ---- OSCond: OSWaitCond(cond, mutex) releases the (once-locked) guest mutex while waiting
HLE_FUNC(coreinit, OSInitCond) { ppc::st32(base, arg(ctx, 0), 0x634E6456); obj<Cond>(arg(ctx, 0)); }  // 'cNdV'
HLE_FUNC(coreinit, OSWaitCond) {
    Mutex& mx = obj<Mutex>(arg(ctx, 1));
    WaitScope w("OSWaitCond", arg(ctx, 0), ctx.lr);
    obj<Cond>(arg(ctx, 0)).cv.wait(mx.m);
}
HLE_FUNC(coreinit, OSSignalCond) { obj<Cond>(arg(ctx, 0)).cv.notify_all(); }  // Cafe wakes all waiters

// ---- OSEvent: mode 0 = manual reset (wakes all), 1 = auto reset (wakes one)
HLE_FUNC(coreinit, OSInitEvent) {
    Event& e = obj<Event>(arg(ctx, 0));
    std::lock_guard g(e.m);
    e.signalled = arg(ctx, 1) != 0;
    e.auto_reset = arg(ctx, 2) == 1;
    ppc::st32(base, arg(ctx, 0), 0x65566E54);  // 'eVnT'
}
HLE_FUNC(coreinit, OSSignalEvent) {
    Event& e = obj<Event>(arg(ctx, 0));
    {
        std::lock_guard g(e.m);
        e.signalled = true;
    }
    if (e.auto_reset) e.cv.notify_one();
    else e.cv.notify_all();
}
HLE_FUNC(coreinit, OSResetEvent) {
    Event& e = obj<Event>(arg(ctx, 0));
    std::lock_guard g(e.m);
    e.signalled = false;
}
HLE_FUNC(coreinit, OSWaitEvent) {
    Event& e = obj<Event>(arg(ctx, 0));
    WaitScope w("OSWaitEvent", arg(ctx, 0), ctx.lr);
    std::unique_lock l(e.m);
    e.cv.wait(l, [&] { return e.signalled; });
    if (e.auto_reset) e.signalled = false;
}
// BOOL OSWaitEventWithTimeout(OSEvent*, OSTime timeout /* r5:r6 */)
HLE_FUNC(coreinit, OSWaitEventWithTimeout) {
    Event& e = obj<Event>(arg(ctx, 0));
    const int64_t ticks = static_cast<int64_t>((uint64_t{ctx.r[5]} << 32) | ctx.r[6]);
    std::unique_lock l(e.m);
    bool ok = e.cv.wait_for(l, std::chrono::nanoseconds(static_cast<int64_t>(static_cast<double>(ticks) / kTicksPerNs)),
                            [&] { return e.signalled; });
    if (ok && e.auto_reset) e.signalled = false;
    ret(ctx, ok ? 1 : 0);
}

// ---- OSSemaphore: Wait/Signal return the count before the operation
HLE_FUNC(coreinit, OSInitSemaphore) {
    Sema& s = obj<Sema>(arg(ctx, 0));
    std::lock_guard g(s.m);
    s.count = static_cast<int32_t>(arg(ctx, 1));
    ppc::st32(base, arg(ctx, 0), 0x73506852);  // 'sPhR'
}
HLE_FUNC(coreinit, OSWaitSemaphore) {
    Sema& s = obj<Sema>(arg(ctx, 0));
    WaitScope w("OSWaitSemaphore", arg(ctx, 0), ctx.lr);
    std::unique_lock l(s.m);
    s.cv.wait(l, [&] { return s.count > 0; });
    ret(ctx, static_cast<uint32_t>(s.count--));
}
HLE_FUNC(coreinit, OSSignalSemaphore) {
    Sema& s = obj<Sema>(arg(ctx, 0));
    int32_t prev;
    {
        std::lock_guard g(s.m);
        prev = s.count++;
    }
    s.cv.notify_one();
    ret(ctx, static_cast<uint32_t>(prev));
}

// ---- OSMessageQueue: 16-byte messages; flags bit 0 = block
// void OSInitMessageQueue(OSMessageQueue*, OSMessage* array, s32 count)
HLE_FUNC(coreinit, OSInitMessageQueue) {
    Queue& q = obj<Queue>(arg(ctx, 0));
    std::lock_guard g(q.m);
    q.q.clear();
    q.capacity = arg(ctx, 2);
    ppc::st32(base, arg(ctx, 0), 0x6D536751);  // 'mSgQ'
}
// BOOL OSSendMessage(queue, OSMessage* msg, OSMessageFlags)
HLE_FUNC(coreinit, OSSendMessage) {
    Queue& q = obj<Queue>(arg(ctx, 0));
    Message m;
    for (int i = 0; i < 4; ++i) m.w[i] = ppc::ld32(base, arg(ctx, 1) + 4 * i);
    std::unique_lock l(q.m);
    if (q.q.size() >= q.capacity) {
        if (!(arg(ctx, 2) & 1)) { ret(ctx, 0); return; }
        WaitScope w("OSSendMessage", arg(ctx, 0), ctx.lr);
        q.cv.wait(l, [&] { return q.q.size() < q.capacity; });
    }
    q.q.push_back(m);
    l.unlock();
    q.cv.notify_all();
    ret(ctx, 1);
}
// BOOL OSReceiveMessage(queue, OSMessage* out, OSMessageFlags)
HLE_FUNC(coreinit, OSReceiveMessage) {
    Queue& q = obj<Queue>(arg(ctx, 0));
    std::unique_lock l(q.m);
    if (q.q.empty()) {
        if (!(arg(ctx, 2) & 1)) { ret(ctx, 0); return; }
        WaitScope w("OSReceiveMessage", arg(ctx, 0), ctx.lr);
        q.cv.wait(l, [&] { return !q.q.empty(); });
    }
    Message m = q.q.front();
    q.q.pop_front();
    l.unlock();
    q.cv.notify_all();
    for (int i = 0; i < 4; ++i) ppc::st32(base, arg(ctx, 1) + 4 * i, m.w[i]);
    ret(ctx, 1);
}

}  // namespace

void register_coreinit_sync(Runtime&) {
    HLE_REGISTER(coreinit, OSInitMutex);
    HLE_REGISTER(coreinit, OSInitMutexEx);
    HLE_REGISTER(coreinit, OSLockMutex);
    HLE_REGISTER(coreinit, OSTryLockMutex);
    HLE_REGISTER(coreinit, OSUnlockMutex);
    HLE_REGISTER(coreinit, OSInitCond);
    HLE_REGISTER(coreinit, OSWaitCond);
    HLE_REGISTER(coreinit, OSSignalCond);
    HLE_REGISTER(coreinit, OSInitEvent);
    HLE_REGISTER(coreinit, OSSignalEvent);
    HLE_REGISTER(coreinit, OSResetEvent);
    HLE_REGISTER(coreinit, OSWaitEvent);
    HLE_REGISTER(coreinit, OSWaitEventWithTimeout);
    HLE_REGISTER(coreinit, OSInitSemaphore);
    HLE_REGISTER(coreinit, OSWaitSemaphore);
    HLE_REGISTER(coreinit, OSSignalSemaphore);
    HLE_REGISTER(coreinit, OSInitMessageQueue);
    HLE_REGISTER(coreinit, OSSendMessage);
    HLE_REGISTER(coreinit, OSReceiveMessage);
}

}  // namespace mh3u::os
