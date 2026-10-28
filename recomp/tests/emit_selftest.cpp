// Runs the emitted self-test functions (generated from selftest_cases.h) and checks results computed by hand.
#include "ppc_context.h"
#include "selftest_cases.h"

#include <cstdio>
#include <cstring>
#include <vector>

PPC_FUNC(sub_01000000);
PPC_FUNC(sub_01001000);
PPC_FUNC(sub_01002000);
PPC_FUNC(sub_01003000);
PPC_FUNC(sub_01004000);
PPC_FUNC(sub_01005000);
PPC_FUNC(sub_01006000);
PPC_FUNC(sub_01007000);
PPC_FUNC(sub_01008000);
PPC_FUNC(sub_01009000);
PPC_FUNC(sub_0100A000);
PPC_FUNC(sub_0100D000);
PPC_FUNC(sub_0100E000);
PPC_FUNC(sub_0100F000);
PPC_FUNC(sub_01010000);
PPC_FUNC(sub_01011000);
PPC_FUNC(sub_01012000);
PPC_FUNC(sub_01013000);
PPC_FUNC(sub_01014100);
PPC_FUNC(sub_01015000);
PPC_FUNC(sub_01016000);

// HLE stand-in for the import called by sub_0100D000
PPC_FUNC(import_test_hle) { ctx.r[3] = 1234; }

namespace ppc = mh3u::ppc;
static int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

int main() {
    std::vector<uint8_t> mem(0x10000);
    uint8_t* base = mem.data();
    ppc::Context ctx{};

    ctx.r[3] = 10;
    sub_01000000(ctx, base);
    CHECK(ctx.r[3] == 55);
    CHECK(ctx.ctr == 0);

    ctx.r[3] = 0x12345678;
    ctx.r[4] = 0xFFFFFFFF;
    sub_01001000(ctx, base);
    CHECK(ctx.r[3] == 0x12);
    CHECK(ctx.r[4] == 0xFFFF56FF);

    ctx.r[3] = 1; ctx.r[4] = 0xFFFFFFFF; ctx.r[5] = 0; ctx.r[6] = 1;
    sub_01002000(ctx, base);
    CHECK(ctx.r[3] == 2 && ctx.r[4] == 0);

    ctx.r[3] = static_cast<uint32_t>(-5); ctx.r[4] = 3;
    sub_01003000(ctx, base);
    CHECK(ctx.r[3] == 3);
    ctx.r[3] = 7; ctx.r[4] = 3;
    sub_01003000(ctx, base);
    CHECK(ctx.r[3] == 7);

    const uint8_t src[] = {0xA1, 0xB2, 0xC3, 0xD4, 0x11, 0x22, 0x33, 0x44};
    std::memcpy(base + 0x100, src, sizeof src);
    ctx.r[3] = 0x100;
    sub_01004000(ctx, base);
    CHECK(ctx.r[5] == 0x11223344 && ctx.r[6] == 0xC3D4 && ctx.r[7] == 0xA1);
    CHECK(ppc::ld32(base, 0x108) == 0x11223344 && ppc::ld16(base, 0x10C) == 0xC3D4 && base[0x110] == 0xA1);
    CHECK(ctx.r[3] == 0x110);  // stbu updated rA

    ppc::stf32(base, 0x200, 3.0f);
    ppc::stf64(base, 0x208, 0.5);
    ctx.r[3] = 0x200; ctx.r[4] = 0x300;
    sub_01005000(ctx, base);
    CHECK(ctx.f[3].ps0 == 9.5 && ctx.f[3].ps1 == 9.5);  // single op writes both lanes
    CHECK(ppc::ld32(base, 0x300) == 9);                 // fctiwz + stfiwx
    CHECK(ppc::ldf32(base, 0x210) == 9.5f);
    CHECK(ctx.cr[1].gt == 1 && ctx.cr[1].lt == 0 && ctx.cr[1].eq == 0 && ctx.cr[1].so == 0);

    ctx.r[3] = static_cast<uint32_t>(-17);
    sub_01006000(ctx, base);
    CHECK(static_cast<int32_t>(ctx.r[4]) == -1);  // -17 / 16 toward zero
    ctx.r[3] = 33;
    sub_01006000(ctx, base);
    CHECK(ctx.r[4] == 2);

    ctx.r[3] = static_cast<uint32_t>(-7); ctx.r[4] = 2;
    sub_01007000(ctx, base);
    CHECK(static_cast<int32_t>(ctx.r[3]) == -3);
    ctx.r[3] = 5; ctx.r[4] = 0;
    sub_01007000(ctx, base);  // division by zero must not crash the host
    CHECK(ctx.r[3] == 0);

    ctx.r[3] = 0;
    sub_01008000(ctx, base);
    CHECK(ctx.r[4] == 32 && ctx.cr[0].eq == 1);  // beqlr taken
    ctx.r[3] = 1;
    sub_01008000(ctx, base);
    CHECK(ctx.r[4] == 99 && ctx.cr[0].gt == 1);

    ppc::stf32(base, 0x400, 1.5f);
    ppc::stf64(base, 0x408, 2.25);
    ctx.r[3] = 0x500; ctx.r[4] = 0x400;
    sub_01009000(ctx, base);
    CHECK(ppc::ldf32(base, 0x500) == 1.5f);  // ps1 survived lfd and was saved via ps_merge10 + stfs
    CHECK(ctx.f[31].ps0 == 1.5 && ctx.f[31].ps1 == 2.25);

    ctx.r[3] = 5; ctx.lr = 0;
    sub_0100A000(ctx, base);
    CHECK(ctx.r[3] == 16);          // +10 in the callee, +1 after the call
    CHECK(ctx.r[4] == 0x0100A004);  // tail-called function sees LR = return address of the earlier bl

    ctx.r[3] = 0;
    sub_0100D000(ctx, base);
    CHECK(ctx.r[3] == 1234 && ctx.lr == 0x0100D004);

    ctx.r[1] = 0x8000; ctx.r[3] = 1; ctx.lr = 0xDEAD0000;
    sub_0100E000(ctx, base);
    CHECK(ctx.r[1] == 0x8000 && ctx.lr == 0xDEAD0000 && ctx.r[3] == 11);              // frame and LR restored
    CHECK(ppc::ld32(base, 0x8004) == 0xDEAD0000 && ppc::ld32(base, 0x7FF0) == 0x8000);  // saved LR, back chain

    const struct { uint32_t in, out; } sw[] = {{0, 100}, {1, 101}, {2, 12}, {5, 0xFFFFFFFF}};  // 2 -> tail call +10
    for (auto [in, want] : sw) {
        ctx.r[3] = in;
        sub_0100F000(ctx, base);
        CHECK(ctx.r[3] == want);
    }

    ctx.r[3] = 1;
    sub_01010000(ctx, base);
    CHECK(ctx.r[3] == 12 && ctx.lr == 0x01010010);  // bctrl (at +0xC) through the function table, +10 then +1

    ppc::st32(base, 0x600, 41);
    ctx.r[3] = 0x600;
    sub_01011000(ctx, base);
    CHECK(ppc::ld32(base, 0x600) == 42 && ctx.reserve_valid == 0 && ctx.cr[0].eq == 1);

    const uint8_t text[] = {'H', 'e', 'l', 'l', 'o', '!'};
    std::memcpy(base + 0x700, text, sizeof text);
    ctx.r[3] = 0x700;
    sub_01012000(ctx, base);
    CHECK(ctx.r[5] == 0x48656C6C && ctx.r[6] == 0x6F210000);

    ctx.r[3] = 0;
    sub_01013000(ctx, base);
    CHECK(ctx.r[3] == 3);  // entry range then chunk
    ctx.r[3] = 0;
    sub_01014100(ctx, base);
    CHECK(ctx.r[3] == 12);  // entered at +0x100, chunk below
    ctx.r[3] = 0;
    sub_01015000(ctx, base);
    CHECK(ctx.r[3] == 3);  // fell through into the next function
    ctx.r[3] = 5;
    sub_01016000(ctx, base);
    CHECK(ctx.r[3] == 1);  // twi condition false: no trap

    std::printf(failures ? "%d failure(s)\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
