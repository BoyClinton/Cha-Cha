// Hand-encoded guest functions for the emitter self-test. emit_selftest_gen turns them into C++ at build
// time; emit_selftest runs the result and checks the outcome against hand-computed values.
#pragma once

#include <cstdint>
#include <vector>

namespace selftest {

constexpr uint32_t D(uint32_t op, uint32_t d, uint32_t a, int32_t imm) { return op << 26 | d << 21 | a << 16 | (uint32_t(imm) & 0xFFFF); }
constexpr uint32_t X(uint32_t d, uint32_t a, uint32_t b, uint32_t xo, uint32_t rc = 0) { return 31u << 26 | d << 21 | a << 16 | b << 11 | xo << 1 | rc; }
constexpr uint32_t M(uint32_t op, uint32_t s, uint32_t a, uint32_t sh, uint32_t mb, uint32_t me) { return op << 26 | s << 21 | a << 16 | sh << 11 | mb << 6 | me << 1; }
constexpr uint32_t A(uint32_t op, uint32_t d, uint32_t a, uint32_t b, uint32_t c, uint32_t xo) { return op << 26 | d << 21 | a << 16 | b << 11 | c << 6 | xo << 1; }
constexpr uint32_t BC(uint32_t bo, uint32_t bi, int32_t off) { return 16u << 26 | bo << 21 | bi << 16 | (uint32_t(off) & 0xFFFC); }
constexpr uint32_t SPR(uint32_t d, uint32_t spr, uint32_t xo) { return X(d, spr & 31, spr >> 5, xo); }
constexpr uint32_t B(int32_t off, uint32_t lk = 0) { return 18u << 26 | (uint32_t(off) & 0x03FFFFFC) | lk; }
constexpr uint32_t BLR = 0x4E800020;

struct Case {
    uint32_t addr;
    std::vector<uint32_t> words;  // the entry range
    std::vector<std::pair<uint32_t, std::vector<uint32_t>>> chunks = {};  // further body ranges
};

enum : uint32_t {
    kSumLoop = 0x01000000, kRotMask = 0x01001000, kCarry64 = 0x01002000, kMaxSigned = 0x01003000,
    kMemory = 0x01004000, kFloat = 0x01005000, kDivBy16 = 0x01006000, kDivw = 0x01007000,
    kRecordReturn = 0x01008000, kPsLanes = 0x01009000, kCaller = 0x0100A000, kCallee = 0x0100B000,
    kTail = 0x0100C000, kImportCall = 0x0100D000, kLrSaver = 0x0100E000, kSwitch = 0x0100F000,
    kIndirect = 0x01010000, kAtomicInc = 0x01011000, kLswi = 0x01012000, kMulti = 0x01013000, kMultiBefore = 0x01014100,
    kFall = 0x01015000, kFallNext = 0x01015004, kTrapNot = 0x01016000,
};
constexpr uint32_t kSwitchBctr = kSwitch + 0x1C;  // its table: 3 inline slots at +0x20, +0x24, +0x28
// the bl at kImportCall is treated as a call to this import (see emit_selftest_gen)
constexpr const char* kTestImport = "import_test_hle";

inline std::vector<Case> cases() {
    return {
        // r3 = 1 + 2 + ... + r3 using CTR: li r4,0; mtctr r3; L: mfctr r5; add r4,r4,r5; bdnz L; mr r3,r4; blr
        {kSumLoop, {D(14, 4, 0, 0), SPR(3, 9, 467), SPR(5, 9, 339), X(4, 4, 5, 266), BC(16, 0, -8), X(4, 3, 4, 444), BLR}},
        // rlwimi r4,r3,0,16,23 ; rlwinm r3,r3,8,24,31
        {kRotMask, {M(20, 3, 4, 0, 16, 23), M(21, 3, 3, 8, 24, 31), BLR}},
        // (r3:r4) += (r5:r6): addc r4,r4,r6; adde r3,r3,r5
        {kCarry64, {X(4, 4, 6, 10), X(3, 3, 5, 138), BLR}},
        // r3 = max(r3, r4) signed: cmpw r3,r4; bge +8; mr r3,r4; blr
        {kMaxSigned, {X(0, 3, 4, 0), BC(4, 0, 8), X(4, 3, 4, 444), BLR}},
        // lwz r5,4(r3); lhz r6,2(r3); lbz r7,0(r3); stw r5,8(r3); sth r6,12(r3); stbu r7,16(r3); blr
        {kMemory, {D(32, 5, 3, 4), D(40, 6, 3, 2), D(34, 7, 3, 0), D(36, 5, 3, 8), D(44, 6, 3, 12), D(39, 7, 3, 16), BLR}},
        // lfs f1,0(r3); lfd f2,8(r3); fmadds f3,f1,f1,f2; fctiwz f4,f3; stfiwx f4,0,r4; stfs f3,16(r3); fcmpu cr1,f3,f2; blr
        {kFloat, {D(48, 1, 3, 0), D(50, 2, 3, 8), A(59, 3, 1, 2, 1, 29), A(63, 4, 0, 3, 0, 15), X(4, 0, 4, 983),
                  D(52, 3, 3, 16), A(63, 4, 3, 2, 0, 0), BLR}},
        // signed r3 / 16 rounding toward zero: srawi r4,r3,4; addze r4,r4; blr
        {kDivBy16, {X(3, 4, 4, 824), X(4, 4, 0, 202), BLR}},
        // divw r3,r3,r4; blr
        {kDivw, {X(3, 3, 4, 491), BLR}},
        // cntlzw r4,r3; and. r5,r3,r3; beqlr; li r4,99; blr
        {kRecordReturn, {X(3, 4, 0, 26), X(3, 5, 3, 28, 1), 0x4D820020, D(14, 4, 0, 99), BLR}},
        // lfs f31,0(r4); lfd f31,8(r4); ps_merge10 f31,f31,f31; stfs f31,0(r3); blr  (GHS ps1 save idiom)
        {kPsLanes, {D(48, 31, 4, 0), D(50, 31, 4, 8), A(4, 31, 31, 31, 0, 0) | (592u << 1), D(52, 31, 3, 0), BLR}},
        // bl kCallee; addi r3,r3,1; b kTail   (call, then tail call)
        {kCaller, {B(int32_t(kCallee - kCaller), 1), D(14, 3, 3, 1), B(int32_t(kTail - (kCaller + 8)))}},
        // addi r3,r3,10; blr
        {kCallee, {D(14, 3, 3, 10), BLR}},
        // mflr r4; blr   (sees the caller's LR: a tail call does not change it)
        {kTail, {SPR(4, 8, 339), BLR}},
        // bl <import>; blr
        {kImportCall, {B(0x1000, 1), BLR}},
        // GHS-style frame: mflr r0; stw r0,4(r1); stwu r1,-16(r1); bl kCallee; lwz r0,20(r1); addi r1,r1,16; mtlr r0; blr
        {kLrSaver, {SPR(0, 8, 339), D(36, 0, 1, 4), D(37, 1, 1, -16), B(int32_t(kCallee - (kLrSaver + 12)), 1),
                    D(32, 0, 1, 20), D(14, 1, 1, 16), SPR(0, 8, 467), BLR}},
        // GHS switch: cmplwi r3,2; bgt default; lis r12,0x0100; ori r12,r12,0xF020; slwi r0,r3,2; add r12,r12,r0;
        // mtctr r12; bctr; table: b case0; b case1; b kCallee (tail-call case);
        // default: li r3,-1; blr; case0: li r3,100; blr; case1: li r3,101; blr
        {kSwitch, {D(10, 0, 3, 2), BC(12, 1, 0x28), D(15, 12, 0, 0x0100), D(24, 12, 12, 0xF020), M(21, 3, 0, 2, 0, 29),
                   X(12, 12, 0, 266), SPR(12, 9, 467), 0x4E800420, B(0x14), B(0x18),
                   B(int32_t(kCallee - (kSwitch + 0x28))), D(14, 3, 0, -1), BLR, D(14, 3, 0, 100), BLR,
                   D(14, 3, 0, 101), BLR}},
        // lis r12,0x0100; ori r12,r12,0xB000 (kCallee); mtctr r12; bctrl; addi r3,r3,1; blr
        {kIndirect, {D(15, 12, 0, 0x0100), D(24, 12, 12, 0xB000), SPR(12, 9, 467), 0x4E800421, D(14, 3, 3, 1), BLR}},
        // atomic increment of *r3: L: lwarx r4,0,r3; addi r4,r4,1; stwcx. r4,0,r3; bne L; blr
        {kAtomicInc, {X(4, 0, 3, 20), D(14, 4, 4, 1), X(4, 0, 3, 150, 1), BC(4, 2, -12), BLR}},
        // lswi r5,r3,6; blr   (r5 = 4 bytes, r6 = 2 bytes left-justified)
        {kLswi, {X(5, 3, 6, 597), BLR}},
        // entry range: addi r3,r3,1; b chunk  |  chunk (+0x100): addi r3,r3,2; blr
        {kMulti, {D(14, 3, 3, 1), B(0x100 - 4)}, {{kMulti + 0x100, {D(14, 3, 3, 2), BLR}}}},
        // entry at +0x100: addi r3,r3,5; b chunk  |  chunk below the entry (+0x0): addi r3,r3,7; blr
        {kMultiBefore, {D(14, 3, 3, 5), B(-0x104)}, {{kMultiBefore - 0x100, {D(14, 3, 3, 7), BLR}}}},
        // addi r3,r3,1 and fall through into the next function
        {kFall, {D(14, 3, 3, 1)}},
        {kFallNext, {D(14, 3, 3, 2), BLR}},
        // twi eq,r3,99 (r3 != 99: no trap); li r3,1; blr
        {kTrapNot, {D(3, 4, 3, 99), D(14, 3, 0, 1), BLR}},
    };
}

}  // namespace selftest
