// Espresso (Wii U CPU) instruction decoder: PowerPC 750CL user + supervisor ISA plus Gekko/Broadway
// paired singles. Decodes one 32-bit big-endian word into canonical form (no simplified mnemonics:
// `li` is addi, `mr` is or, `beqlr` is bclr, ...). The recompiler interprets the fields per op.
#pragma once

#include <cstdint>
#include <string>

namespace mh3u::ppc {

// X(name, mnemonic). Record (`.`), overflow (`o`), link (`l`) and absolute (`a`) variants are flags on
// the instruction, not separate ops; ops whose base form always records (andi., stwcx., ...) keep the dot.
#define MH3U_PPC_OPS(X)                                                                                  \
    /* integer arithmetic */                                                                             \
    X(add, "add") X(addc, "addc") X(adde, "adde") X(addi, "addi") X(addic, "addic") X(addic_rc, "addic.") \
    X(addis, "addis") X(addme, "addme") X(addze, "addze") X(divw, "divw") X(divwu, "divwu")              \
    X(mulhw, "mulhw") X(mulhwu, "mulhwu") X(mulli, "mulli") X(mullw, "mullw") X(neg, "neg")              \
    X(subf, "subf") X(subfc, "subfc") X(subfe, "subfe") X(subfic, "subfic") X(subfme, "subfme")          \
    X(subfze, "subfze")                                                                                  \
    /* compare / trap */                                                                                 \
    X(cmp, "cmp") X(cmpi, "cmpi") X(cmpl, "cmpl") X(cmpli, "cmpli") X(tw, "tw") X(twi, "twi")            \
    /* logical / rotate / shift */                                                                       \
    X(and_, "and") X(andc, "andc") X(andi_rc, "andi.") X(andis_rc, "andis.") X(cntlzw, "cntlzw")         \
    X(eqv, "eqv") X(extsb, "extsb") X(extsh, "extsh") X(nand, "nand") X(nor, "nor") X(or_, "or")        \
    X(orc, "orc") X(ori, "ori") X(oris, "oris") X(xor_, "xor") X(xori, "xori") X(xoris, "xoris")         \
    X(rlwimi, "rlwimi") X(rlwinm, "rlwinm") X(rlwnm, "rlwnm") X(slw, "slw") X(sraw, "sraw")             \
    X(srawi, "srawi") X(srw, "srw")                                                                      \
    /* branch / condition register */                                                                    \
    X(b, "b") X(bc, "bc") X(bcctr, "bcctr") X(bclr, "bclr") X(crand, "crand") X(crandc, "crandc")        \
    X(creqv, "creqv") X(crnand, "crnand") X(crnor, "crnor") X(cror, "cror") X(crorc, "crorc")           \
    X(crxor, "crxor") X(mcrf, "mcrf") X(mcrxr, "mcrxr") X(mfcr, "mfcr") X(mtcrf, "mtcrf")              \
    /* integer load/store */                                                                             \
    X(lbz, "lbz") X(lbzu, "lbzu") X(lbzux, "lbzux") X(lbzx, "lbzx") X(lha, "lha") X(lhau, "lhau")        \
    X(lhaux, "lhaux") X(lhax, "lhax") X(lhbrx, "lhbrx") X(lhz, "lhz") X(lhzu, "lhzu") X(lhzux, "lhzux")  \
    X(lhzx, "lhzx") X(lmw, "lmw") X(lswi, "lswi") X(lswx, "lswx") X(lwarx, "lwarx") X(lwbrx, "lwbrx")    \
    X(lwz, "lwz") X(lwzu, "lwzu") X(lwzux, "lwzux") X(lwzx, "lwzx") X(stb, "stb") X(stbu, "stbu")        \
    X(stbux, "stbux") X(stbx, "stbx") X(sth, "sth") X(sthbrx, "sthbrx") X(sthu, "sthu")                  \
    X(sthux, "sthux") X(sthx, "sthx") X(stmw, "stmw") X(stswi, "stswi") X(stswx, "stswx") X(stw, "stw")  \
    X(stwbrx, "stwbrx") X(stwcx_rc, "stwcx.") X(stwu, "stwu") X(stwux, "stwux") X(stwx, "stwx")          \
    /* floating-point load/store */                                                                      \
    X(lfd, "lfd") X(lfdu, "lfdu") X(lfdux, "lfdux") X(lfdx, "lfdx") X(lfs, "lfs") X(lfsu, "lfsu")        \
    X(lfsux, "lfsux") X(lfsx, "lfsx") X(stfd, "stfd") X(stfdu, "stfdu") X(stfdux, "stfdux")              \
    X(stfdx, "stfdx") X(stfiwx, "stfiwx") X(stfs, "stfs") X(stfsu, "stfsu") X(stfsux, "stfsux")          \
    X(stfsx, "stfsx")                                                                                    \
    /* floating point */                                                                                 \
    X(fabs, "fabs") X(fadd, "fadd") X(fadds, "fadds") X(fcmpo, "fcmpo") X(fcmpu, "fcmpu")                \
    X(fctiw, "fctiw") X(fctiwz, "fctiwz") X(fdiv, "fdiv") X(fdivs, "fdivs") X(fmadd, "fmadd")            \
    X(fmadds, "fmadds") X(fmr, "fmr") X(fmsub, "fmsub") X(fmsubs, "fmsubs") X(fmul, "fmul")              \
    X(fmuls, "fmuls") X(fnabs, "fnabs") X(fneg, "fneg") X(fnmadd, "fnmadd") X(fnmadds, "fnmadds")        \
    X(fnmsub, "fnmsub") X(fnmsubs, "fnmsubs") X(fres, "fres") X(frsp, "frsp") X(frsqrte, "frsqrte")      \
    X(fsel, "fsel") X(fsqrt, "fsqrt") X(fsqrts, "fsqrts") X(fsub, "fsub") X(fsubs, "fsubs")              \
    X(mcrfs, "mcrfs") X(mffs, "mffs") X(mtfsb0, "mtfsb0") X(mtfsb1, "mtfsb1") X(mtfsf, "mtfsf")          \
    X(mtfsfi, "mtfsfi")                                                                                  \
    /* paired singles (Gekko/Broadway/Espresso) */                                                       \
    X(psq_l, "psq_l") X(psq_lu, "psq_lu") X(psq_lux, "psq_lux") X(psq_lx, "psq_lx") X(psq_st, "psq_st") \
    X(psq_stu, "psq_stu") X(psq_stux, "psq_stux") X(psq_stx, "psq_stx") X(ps_abs, "ps_abs")             \
    X(ps_add, "ps_add") X(ps_cmpo0, "ps_cmpo0") X(ps_cmpo1, "ps_cmpo1") X(ps_cmpu0, "ps_cmpu0")          \
    X(ps_cmpu1, "ps_cmpu1") X(ps_div, "ps_div") X(ps_madd, "ps_madd") X(ps_madds0, "ps_madds0")          \
    X(ps_madds1, "ps_madds1") X(ps_merge00, "ps_merge00") X(ps_merge01, "ps_merge01")                    \
    X(ps_merge10, "ps_merge10") X(ps_merge11, "ps_merge11") X(ps_mr, "ps_mr") X(ps_msub, "ps_msub")      \
    X(ps_mul, "ps_mul") X(ps_muls0, "ps_muls0") X(ps_muls1, "ps_muls1") X(ps_nabs, "ps_nabs")            \
    X(ps_neg, "ps_neg") X(ps_nmadd, "ps_nmadd") X(ps_nmsub, "ps_nmsub") X(ps_res, "ps_res")              \
    X(ps_rsqrte, "ps_rsqrte") X(ps_sel, "ps_sel") X(ps_sub, "ps_sub") X(ps_sum0, "ps_sum0")              \
    X(ps_sum1, "ps_sum1")                                                                                \
    /* system / cache / storage control */                                                               \
    X(dcbf, "dcbf") X(dcbi, "dcbi") X(dcbst, "dcbst") X(dcbt, "dcbt") X(dcbtst, "dcbtst") X(dcbz, "dcbz") \
    X(dcbz_l, "dcbz_l") X(eciwx, "eciwx") X(ecowx, "ecowx") X(eieio, "eieio") X(icbi, "icbi")            \
    X(isync, "isync") X(mfmsr, "mfmsr") X(mfspr, "mfspr") X(mfsr, "mfsr") X(mfsrin, "mfsrin")            \
    X(mftb, "mftb") X(mtmsr, "mtmsr") X(mtspr, "mtspr") X(mtsr, "mtsr") X(mtsrin, "mtsrin") X(rfi, "rfi") \
    X(sc, "sc") X(sync, "sync") X(tlbie, "tlbie") X(tlbsync, "tlbsync")

enum class Op : uint16_t {
    invalid,
#define X(id, mn) id,
    MH3U_PPC_OPS(X)
#undef X
        count
};

const char* mnemonic(Op op);

// Fields are named after their bit position (IBM numbering), not their role, because the role depends on
// the op: `d` = bits 6-10 (rD/rS/frD/frS/crbD/BO/TO), `a` = 11-15 (rA/frA/crbA/BI), `b` = 16-20
// (rB/frB/crbB/SH/NB), `c` = 21-25 (frC/MB), `e` = 26-30 (ME).
struct Insn {
    Op op = Op::invalid;
    uint32_t raw = 0;
    uint8_t d = 0, a = 0, b = 0, c = 0, e = 0;
    int32_t imm = 0;   // D-form immediate (signed or unsigned per op), displacement, branch offset,
                       // psq 12-bit displacement
    uint16_t spr = 0;  // mfspr/mtspr/mftb SPR number (halves already swapped)
    uint8_t crf = 0;   // cmp*/mcrf/mcrfs/fcmp*/ps_cmp*/mcrxr target CR field (d >> 2)
    uint8_t crfs = 0;  // mcrf/mcrfs source CR field (a >> 2)
    uint8_t fxm = 0;   // mtcrf CRM / mtfsf FM
    uint8_t gqr = 0;   // psq_*: I (GQR index)
    bool w = false;    // psq_*: W (single value)
    bool rc = false;   // record form (`.`)
    bool oe = false;   // overflow form (`o`)
    bool lk = false;   // branch: link
    bool aa = false;   // branch: absolute

    bool valid() const { return op != Op::invalid; }
};

Insn decode(uint32_t word);

// Canonical text, e.g. "addi r3,r1,0x8", "bc 12,2,0x02000040", "psq_l f1,0x8(r3),0,2".
// `pc` resolves branch targets.
std::string format(const Insn& in, uint32_t pc);

}  // namespace mh3u::ppc
