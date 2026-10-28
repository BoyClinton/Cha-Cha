// Guest CPU state and memory model used by recompiled code (recomp/out/*.cpp) and the runtime.
//
// Memory: the guest's 32-bit address space is a flat host allocation; guest address A lives at base + A.
// The guest is big-endian, so every load/store byte-swaps. Recompiled functions take (ctx, base).
//
// Registers follow the PowerPC ABI used by GHS on Cafe OS: r1 stack, r2/r13 small-data bases, r3-r10 args,
// f1-f8 FP args. FPRs hold two lanes (ps0/ps1): game code does no paired-single math, but GHS saves and
// restores ps1 of f14-f31 with ps_merge10 + stfs/lfs, so the lane is modelled (see recomp/README.md).
#pragma once

#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>

// Signature of every recompiled function.
#define PPC_FUNC(name) void name([[maybe_unused]] mh3u::ppc::Context& ctx, [[maybe_unused]] uint8_t* base)

namespace mh3u::ppc {

struct CRField {
    uint8_t lt, gt, eq, so;  // each 0 or 1; for FP compares `so` is "unordered"
};

struct FPR {
    double ps0;
    double ps1;
};

struct Context {
    uint32_t r[32];
    FPR f[32];
    CRField cr[8];
    uint32_t lr, ctr;
    uint8_t xer_so, xer_ov, xer_ca;
    uint32_t fpscr;
    uint32_t gqr[8];
    // lwarx/stwcx. reservation: address and the raw (guest-order) word seen by lwarx
    uint32_t reserve_addr, reserve_raw;
    uint8_t reserve_valid;
};

// Every recompiled function, indexed by guest address (generated function_table.cpp), for indirect calls.
using GuestFunction = void (*)(Context&, uint8_t*);
struct FunctionEntry {
    uint32_t addr;
    GuestFunction fn;
};

// Called by generated stubs for functions the emitter could not translate yet and for Cafe OS imports without an
// HLE implementation (recomp/support/unimplemented.cpp; the runtime replaces the import stubs).
[[noreturn]] void unimplemented_function(uint32_t addr);
[[noreturn]] void unimplemented_import(const char* name);

// Call through a guest code pointer (bctrl, bctr, bclrl): looks the address up in the generated function table and
// import table (recomp/support/indirect.cpp); an unknown address is reported as unimplemented.
void call_indirect(Context& ctx, uint8_t* base, uint32_t target);
// Target of the innermost call_indirect on this thread (lets one host function serve several guest addresses).
uint32_t current_indirect_target();

// HLE registry (recomp/support/imports.cpp). The runtime registers its Cafe OS implementations by qualified name
// ("coreinit.OSCreateThread") before running guest code; generated import stubs resolve through it once (null =
// not registered: the stub calls unimplemented_import). register_indirect makes a host function callable
// through a guest code pointer (e.g. the MEMAllocFromDefaultHeap data import holds such an address).
void register_import(const char* qualified_name, GuestFunction fn);
GuestFunction resolve_import(const char* qualified_name);
void register_indirect(uint32_t guest_addr, GuestFunction fn);
GuestFunction find_indirect(uint32_t guest_addr);
// Hand-written replacements of game functions (src/; recomp/README.md § Overrides). Every function listed in
// config/overrides.txt is emitted twice: <name>__orig (the translation) and <name>, a dispatcher that calls the
// replacement registered with set_override, or the original when there is none. Register before guest code runs.
GuestFunction* override_slot(uint32_t guest_addr);  // stable address; null until set_override
void set_override(uint32_t guest_addr, GuestFunction fn);
// Called on every import call when tracing is enabled (set by the runtime).
extern void (*import_trace_hook)(const char* qualified_name);

// tw/twi whose condition holds (recomp/support/unimplemented.cpp): report and stop.
[[noreturn]] void trap(uint32_t pc);

// ---- big-endian guest memory ---------------------------------------------------------------------------
inline uint8_t ld8(const uint8_t* base, uint32_t a) { return base[a]; }
inline uint16_t ld16(const uint8_t* base, uint32_t a) {
    uint16_t v;
    std::memcpy(&v, base + a, 2);
    return static_cast<uint16_t>((v >> 8) | (v << 8));
}
inline uint32_t ld32(const uint8_t* base, uint32_t a) {
    uint32_t v;
    std::memcpy(&v, base + a, 4);
    return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}
inline uint64_t ld64(const uint8_t* base, uint32_t a) {
    return (uint64_t{ld32(base, a)} << 32) | ld32(base, a + 4);
}
inline void st8(uint8_t* base, uint32_t a, uint8_t v) { base[a] = v; }
inline void st16(uint8_t* base, uint32_t a, uint16_t v) {
    v = static_cast<uint16_t>((v >> 8) | (v << 8));
    std::memcpy(base + a, &v, 2);
}
inline void st32(uint8_t* base, uint32_t a, uint32_t v) {
    v = (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
    std::memcpy(base + a, &v, 4);
}
inline void st64(uint8_t* base, uint32_t a, uint64_t v) {
    st32(base, a, static_cast<uint32_t>(v >> 32));
    st32(base, a + 4, static_cast<uint32_t>(v));
}
// byte-reversed forms (lwbrx/sthbrx...): the value is little-endian in guest memory
inline uint16_t ld16_le(const uint8_t* base, uint32_t a) { uint16_t v; std::memcpy(&v, base + a, 2); return v; }
inline uint32_t ld32_le(const uint8_t* base, uint32_t a) { uint32_t v; std::memcpy(&v, base + a, 4); return v; }
inline void st16_le(uint8_t* base, uint32_t a, uint16_t v) { std::memcpy(base + a, &v, 2); }
inline void st32_le(uint8_t* base, uint32_t a, uint32_t v) { std::memcpy(base + a, &v, 4); }

inline float ldf32(const uint8_t* base, uint32_t a) { return std::bit_cast<float>(ld32(base, a)); }
inline double ldf64(const uint8_t* base, uint32_t a) { return std::bit_cast<double>(ld64(base, a)); }
inline void stf32(uint8_t* base, uint32_t a, float v) { st32(base, a, std::bit_cast<uint32_t>(v)); }
inline void stf64(uint8_t* base, uint32_t a, double v) { st64(base, a, std::bit_cast<uint64_t>(v)); }

// ---- helpers used by generated code ---------------------------------------------------------------------
inline uint32_t rotl32(uint32_t v, uint32_t n) { return std::rotl(v, static_cast<int>(n & 31)); }
// rlw* mask from MB..ME (IBM bit numbering, wraps when MB > ME)
constexpr uint32_t mask32(uint32_t mb, uint32_t me) {
    uint32_t m1 = 0xFFFFFFFFu >> mb, m2 = 0xFFFFFFFFu << (31 - me);
    return mb <= me ? (m1 & m2) : (m1 | m2);
}
inline void cr_set_s(CRField& c, int32_t a, int32_t b, uint8_t so) {
    c.lt = a < b; c.gt = a > b; c.eq = a == b; c.so = so;
}
inline void cr_set_u(CRField& c, uint32_t a, uint32_t b, uint8_t so) {
    c.lt = a < b; c.gt = a > b; c.eq = a == b; c.so = so;
}
inline void cr_set_f(CRField& c, double a, double b) {
    bool un = std::isnan(a) || std::isnan(b);
    c.lt = !un && a < b; c.gt = !un && a > b; c.eq = !un && a == b; c.so = un;
}
inline uint8_t& cr_bit(Context& ctx, uint32_t bi) {
    CRField& c = ctx.cr[bi >> 2];
    switch (bi & 3) { case 0: return c.lt; case 1: return c.gt; case 2: return c.eq; default: return c.so; }
}
inline uint32_t cr_pack(const Context& ctx) {
    uint32_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= ((ctx.cr[i].lt << 3) | (ctx.cr[i].gt << 2) | (ctx.cr[i].eq << 1) | ctx.cr[i].so) << (28 - 4 * i);
    return v;
}
inline void cr_unpack(Context& ctx, uint32_t v, uint32_t fxm) {
    for (int i = 0; i < 8; ++i) {
        if (!(fxm & (0x80u >> i))) continue;
        uint32_t n = v >> (28 - 4 * i);
        ctx.cr[i] = {static_cast<uint8_t>((n >> 3) & 1), static_cast<uint8_t>((n >> 2) & 1),
                     static_cast<uint8_t>((n >> 1) & 1), static_cast<uint8_t>(n & 1)};
    }
}
// tw/twi TO bits: 16 lt, 8 gt, 4 eq, 2 lt unsigned, 1 gt unsigned
inline bool trap_cond(uint32_t a, uint32_t b, uint32_t to) {
    int32_t sa = static_cast<int32_t>(a), sb = static_cast<int32_t>(b);
    return ((to & 16) && sa < sb) || ((to & 8) && sa > sb) || ((to & 4) && a == b) || ((to & 2) && a < b) ||
           ((to & 1) && a > b);
}
// lwarx/stwcx.: the store succeeds only if the reserved word is unchanged, checked with a host compare-exchange so
// it stays atomic when guest threads run on host threads (ABA is accepted, as on most emulators).
inline uint32_t lwarx(Context& ctx, uint8_t* base, uint32_t ea) {
    uint32_t raw = std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(base + ea)).load();
    ctx.reserve_addr = ea;
    ctx.reserve_raw = raw;
    ctx.reserve_valid = 1;
    return (raw >> 24) | ((raw >> 8) & 0xFF00) | ((raw << 8) & 0xFF0000) | (raw << 24);
}
inline void stwcx(Context& ctx, uint8_t* base, uint32_t ea, uint32_t v) {
    bool ok = false;
    if (ctx.reserve_valid && ctx.reserve_addr == ea) {
        uint32_t expected = ctx.reserve_raw;
        uint32_t raw = (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
        ok = std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(base + ea)).compare_exchange_strong(expected, raw);
    }
    ctx.reserve_valid = 0;
    ctx.cr[0] = {0, 0, static_cast<uint8_t>(ok), ctx.xer_so};
}
// lswi/stswi: n bytes to/from consecutive registers from rd (wrapping after r31), 4 bytes per register, big-endian
inline void lswi(Context& ctx, const uint8_t* base, uint32_t ea, uint32_t rd, uint32_t n) {
    uint32_t r = (rd + 31) & 31;
    for (uint32_t i = 0; i < n; ++i) {
        if (i % 4 == 0) {
            r = (r + 1) & 31;
            ctx.r[r] = 0;
        }
        ctx.r[r] |= uint32_t{base[ea + i]} << (24 - 8 * (i % 4));
    }
}
inline void stswi(Context& ctx, uint8_t* base, uint32_t ea, uint32_t rs, uint32_t n) {
    uint32_t r = (rs + 31) & 31;
    for (uint32_t i = 0; i < n; ++i) {
        if (i % 4 == 0) r = (r + 1) & 31;
        base[ea + i] = static_cast<uint8_t>(ctx.r[r] >> (24 - 8 * (i % 4)));
    }
}
// single-precision results are rounded to float and written to both lanes (as on Gekko/Espresso)
inline double round_single(double v) { return static_cast<double>(static_cast<float>(v)); }
// fctiw(z): saturating conversion; the integer lands in the low word of the FPR's bit pattern
inline double fctiw_bits(double v, bool toward_zero) {
    int32_t i;
    if (std::isnan(v)) i = INT32_MIN;
    else if (v >= 2147483647.0) i = INT32_MAX;
    else if (v <= -2147483648.0) i = INT32_MIN;
    else i = static_cast<int32_t>(toward_zero ? std::trunc(v) : std::nearbyint(v));
    return std::bit_cast<double>(0xFFF8000000000000ull | static_cast<uint32_t>(i));
}

}  // namespace mh3u::ppc
