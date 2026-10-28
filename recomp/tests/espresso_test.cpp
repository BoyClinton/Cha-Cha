// Unit tests for the Espresso decoder. Expected text is canonical form (see espresso.h); words come from the
// game's code as shown by Ghidra (e.g. 0x7C0903A6 = "mtspr CTR,r0") or are hand-encoded paired-single forms.
#include "espresso.h"

#include <cstdio>
#include <string>

using namespace mh3u::ppc;

static int failures = 0;

static void check(uint32_t word, uint32_t pc, const char* want) {
    std::string got = format(decode(word), pc);
    if (got != want) {
        std::printf("FAIL %08X: got \"%s\", want \"%s\"\n", word, got.c_str(), want);
        ++failures;
    }
}

int main() {
    // integer / logical / rotate
    check(0x38210310, 0, "addi r1,r1,0x310");
    check(0x3D80103D, 0, "addis r12,r0,0x103d");                 // lis r12,0x103d
    check(0x3863FFF8, 0, "addi r3,r3,-0x8");                     // subi r3,r3,8
    check(0x7D445378, 0, "or r4,r10,r10");                        // mr r4,r10
    check(0x7C6A1B79, 0, "or. r10,r3,r3");                        // mr. r10,r3
    check(0x548007FF, 0, "rlwinm. r0,r4,0,31,31");
    check(0x5508103A, 0, "rlwinm r8,r8,2,0,29");                  // slwi r8,r8,2
    check(0x60000000, 0, "ori r0,r0,0x0");                        // nop
    check(0x7C0802A6, 0, "mfspr r0,8");                           // mflr r0
    check(0x7C0903A6, 0, "mtspr 9,r0");                           // mtctr r0
    check(0x7C6C42E6, 0, "mftb r3,268");
    check(0x7C632214, 0, "add r3,r3,r4");
    check(0x7C632614, 0, "addo r3,r3,r4");
    check(0x7C6418F8, 0, "nor r4,r3,r3");                         // not r4,r3
    check(0x7C0004AC, 0, "sync");
    // compare
    check(0x2C030000, 0, "cmpi 0,0,r3,0x0");                      // cmpwi r3,0
    check(0x28030480, 0, "cmpli 0,0,r3,0x480");
    check(0x7F9F2040, 0, "cmpl 7,0,r31,r4");
    // load/store
    check(0x80636104, 0, "lwz r3,0x6104(r3)");
    check(0x9421FFF0, 0, "stwu r1,-0x10(r1)");
    check(0xBFA10014, 0, "stmw r29,0x14(r1)");
    check(0x7C63042C, 0, "lwbrx r3,r3,r0");
    // branch
    check(0x4E800020, 0, "bclr 20,0");                            // blr
    check(0x4D820020, 0, "bclr 12,2");                            // beqlr
    check(0x4E800420, 0, "bcctr 20,0");                           // bctr
    check(0x4E800421, 0, "bcctrl 20,0");                          // bctrl
    check(0x48E8175C, 0x02005AE4, "b 0x2e87240");
    check(0x4BFFFFF1, 0x02000100, "bl 0x20000f0");
    check(0x4182000C, 0x02000100, "bc 12,2,0x200010c");           // beq +0xc
    check(0x4200FFF8, 0x02000100, "bc 16,0,0x20000f8");           // bdnz -8
    check(0x4CC63182, 0, "crxor 6,6,6");                          // crclr 4*cr1+eq
    check(0x44000002, 0, "sc");
    // floating point
    check(0xFC000890, 0, "fmr f0,f1");
    check(0xEC21107A, 0, "fmadds f1,f1,f1,f2");
    check(0xFC011000, 0, "fcmpu 0,f1,f2");
    check(0xFC20081E, 0, "fctiwz f1,f1");
    check(0xC0230008, 0, "lfs f1,0x8(r3)");
    check(0x7C2018AE, 0, "lbzx r1,r0,r3");
    check(0x7C20F42E, 0, "lfsx f1,r0,r30");
    // paired singles
    check(0x13FFFCA0, 0, "ps_merge10 f31,f31,f31");
    check(0xE0232008, 0, "psq_l f1,0x8(r3),0,2");
    check(0xF003F00C, 0, "psq_st f0,0xc(r3),1,7");
    check(0x1003200C, 0, "psq_lx f0,r3,r4,0,0");
    check(0x1022182A, 0, "ps_add f1,f2,f3");
    check(0x10221932, 0, "ps_mul f1,f2,f4");
    check(0x1022193A, 0, "ps_madd f1,f2,f4,f3");
    check(0x10221814, 0, "ps_sum0 f1,f2,f0,f3");
    check(0x10011000, 0, "ps_cmpu0 0,f1,f2");
    check(0x100307EC, 0, "dcbz_l r3,r0");
    // invalid
    check(0x00000000, 0, ".long 0x00000000");
    check(0x00400000, 0, ".long 0x00400000");

    std::printf(failures ? "%d failure(s)\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
