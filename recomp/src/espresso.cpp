#include "espresso.h"

#include <cstdio>

namespace mh3u::ppc {
namespace {

const char* const kMnemonics[] = {
    "(invalid)",
#define X(id, mn) mn,
    MH3U_PPC_OPS(X)
#undef X
};
static_assert(sizeof(kMnemonics) / sizeof(kMnemonics[0]) == static_cast<size_t>(Op::count));

int32_t sext(uint32_t v, int bits) {
    uint32_t m = 1u << (bits - 1);
    return static_cast<int32_t>((v ^ m) - m);
}

// Opcode 31. Arithmetic ops use a 9-bit XO with OE in bit 21; the remaining XO values never collide with
// an arithmetic XO plus 512, so the 9-bit table is checked first.
Op op31_arith(uint32_t xo9) {
    switch (xo9) {
        case 8: return Op::subfc;
        case 10: return Op::addc;
        case 11: return Op::mulhwu;
        case 40: return Op::subf;
        case 75: return Op::mulhw;
        case 104: return Op::neg;
        case 136: return Op::subfe;
        case 138: return Op::adde;
        case 200: return Op::subfze;
        case 202: return Op::addze;
        case 232: return Op::subfme;
        case 234: return Op::addme;
        case 235: return Op::mullw;
        case 266: return Op::add;
        case 459: return Op::divwu;
        case 491: return Op::divw;
        default: return Op::invalid;
    }
}

Op op31(uint32_t xo10) {
    switch (xo10) {
        case 0: return Op::cmp;
        case 4: return Op::tw;
        case 19: return Op::mfcr;
        case 20: return Op::lwarx;
        case 23: return Op::lwzx;
        case 24: return Op::slw;
        case 26: return Op::cntlzw;
        case 28: return Op::and_;
        case 32: return Op::cmpl;
        case 54: return Op::dcbst;
        case 55: return Op::lwzux;
        case 60: return Op::andc;
        case 83: return Op::mfmsr;
        case 86: return Op::dcbf;
        case 87: return Op::lbzx;
        case 119: return Op::lbzux;
        case 124: return Op::nor;
        case 144: return Op::mtcrf;
        case 146: return Op::mtmsr;
        case 150: return Op::stwcx_rc;
        case 151: return Op::stwx;
        case 183: return Op::stwux;
        case 210: return Op::mtsr;
        case 215: return Op::stbx;
        case 242: return Op::mtsrin;
        case 246: return Op::dcbtst;
        case 247: return Op::stbux;
        case 278: return Op::dcbt;
        case 279: return Op::lhzx;
        case 284: return Op::eqv;
        case 306: return Op::tlbie;
        case 310: return Op::eciwx;
        case 311: return Op::lhzux;
        case 316: return Op::xor_;
        case 339: return Op::mfspr;
        case 343: return Op::lhax;
        case 371: return Op::mftb;
        case 375: return Op::lhaux;
        case 407: return Op::sthx;
        case 412: return Op::orc;
        case 438: return Op::ecowx;
        case 439: return Op::sthux;
        case 444: return Op::or_;
        case 467: return Op::mtspr;
        case 470: return Op::dcbi;
        case 476: return Op::nand;
        case 512: return Op::mcrxr;
        case 533: return Op::lswx;
        case 534: return Op::lwbrx;
        case 535: return Op::lfsx;
        case 536: return Op::srw;
        case 566: return Op::tlbsync;
        case 567: return Op::lfsux;
        case 595: return Op::mfsr;
        case 597: return Op::lswi;
        case 598: return Op::sync;
        case 599: return Op::lfdx;
        case 631: return Op::lfdux;
        case 659: return Op::mfsrin;
        case 661: return Op::stswx;
        case 662: return Op::stwbrx;
        case 663: return Op::stfsx;
        case 695: return Op::stfsux;
        case 725: return Op::stswi;
        case 727: return Op::stfdx;
        case 759: return Op::stfdux;
        case 790: return Op::lhbrx;
        case 792: return Op::sraw;
        case 824: return Op::srawi;
        case 854: return Op::eieio;
        case 918: return Op::sthbrx;
        case 922: return Op::extsh;
        case 954: return Op::extsb;
        case 982: return Op::icbi;
        case 983: return Op::stfiwx;
        case 1014: return Op::dcbz;
        default: return Op::invalid;
    }
}

Op op19(uint32_t xo10) {
    switch (xo10) {
        case 0: return Op::mcrf;
        case 16: return Op::bclr;
        case 33: return Op::crnor;
        case 50: return Op::rfi;
        case 129: return Op::crandc;
        case 150: return Op::isync;
        case 193: return Op::crxor;
        case 225: return Op::crnand;
        case 257: return Op::crand;
        case 289: return Op::creqv;
        case 417: return Op::crorc;
        case 449: return Op::cror;
        case 528: return Op::bcctr;
        default: return Op::invalid;
    }
}

Op op59(uint32_t xo5) {
    switch (xo5) {
        case 18: return Op::fdivs;
        case 20: return Op::fsubs;
        case 21: return Op::fadds;
        case 22: return Op::fsqrts;
        case 24: return Op::fres;
        case 25: return Op::fmuls;
        case 28: return Op::fmsubs;
        case 29: return Op::fmadds;
        case 30: return Op::fnmsubs;
        case 31: return Op::fnmadds;
        default: return Op::invalid;
    }
}

// Opcode 63: A-form ops have XO >= 16 in bits 26-30; no X-form XO has those low bits.
Op op63(uint32_t xo10) {
    switch (xo10 & 31) {
        case 18: return Op::fdiv;
        case 20: return Op::fsub;
        case 21: return Op::fadd;
        case 22: return Op::fsqrt;
        case 23: return Op::fsel;
        case 25: return Op::fmul;
        case 26: return Op::frsqrte;
        case 28: return Op::fmsub;
        case 29: return Op::fmadd;
        case 30: return Op::fnmsub;
        case 31: return Op::fnmadd;
        default: break;
    }
    switch (xo10) {
        case 0: return Op::fcmpu;
        case 12: return Op::frsp;
        case 14: return Op::fctiw;
        case 15: return Op::fctiwz;
        case 32: return Op::fcmpo;
        case 38: return Op::mtfsb1;
        case 40: return Op::fneg;
        case 64: return Op::mcrfs;
        case 70: return Op::mtfsb0;
        case 72: return Op::fmr;
        case 134: return Op::mtfsfi;
        case 136: return Op::fnabs;
        case 264: return Op::fabs;
        case 583: return Op::mffs;
        case 711: return Op::mtfsf;
        default: return Op::invalid;
    }
}

// Opcode 4 (paired singles): A-form (5-bit XO), then the indexed psq forms (6-bit XO), then X-form.
Op op4(uint32_t xo10) {
    switch (xo10 & 31) {
        case 10: return Op::ps_sum0;
        case 11: return Op::ps_sum1;
        case 12: return Op::ps_muls0;
        case 13: return Op::ps_muls1;
        case 14: return Op::ps_madds0;
        case 15: return Op::ps_madds1;
        case 18: return Op::ps_div;
        case 20: return Op::ps_sub;
        case 21: return Op::ps_add;
        case 23: return Op::ps_sel;
        case 24: return Op::ps_res;
        case 25: return Op::ps_mul;
        case 26: return Op::ps_rsqrte;
        case 28: return Op::ps_msub;
        case 29: return Op::ps_madd;
        case 30: return Op::ps_nmsub;
        case 31: return Op::ps_nmadd;
        default: break;
    }
    switch (xo10 & 63) {
        case 6: return Op::psq_lx;
        case 7: return Op::psq_stx;
        case 38: return Op::psq_lux;
        case 39: return Op::psq_stux;
        default: break;
    }
    switch (xo10) {
        case 0: return Op::ps_cmpu0;
        case 32: return Op::ps_cmpo0;
        case 40: return Op::ps_neg;
        case 64: return Op::ps_cmpu1;
        case 72: return Op::ps_mr;
        case 96: return Op::ps_cmpo1;
        case 136: return Op::ps_nabs;
        case 264: return Op::ps_abs;
        case 528: return Op::ps_merge00;
        case 560: return Op::ps_merge01;
        case 592: return Op::ps_merge10;
        case 624: return Op::ps_merge11;
        case 1014: return Op::dcbz_l;
        default: return Op::invalid;
    }
}

Op primary(uint32_t opcd) {
    switch (opcd) {
        case 3: return Op::twi;
        case 7: return Op::mulli;
        case 8: return Op::subfic;
        case 10: return Op::cmpli;
        case 11: return Op::cmpi;
        case 12: return Op::addic;
        case 13: return Op::addic_rc;
        case 14: return Op::addi;
        case 15: return Op::addis;
        case 16: return Op::bc;
        case 17: return Op::sc;
        case 18: return Op::b;
        case 20: return Op::rlwimi;
        case 21: return Op::rlwinm;
        case 23: return Op::rlwnm;
        case 24: return Op::ori;
        case 25: return Op::oris;
        case 26: return Op::xori;
        case 27: return Op::xoris;
        case 28: return Op::andi_rc;
        case 29: return Op::andis_rc;
        case 32: return Op::lwz;
        case 33: return Op::lwzu;
        case 34: return Op::lbz;
        case 35: return Op::lbzu;
        case 36: return Op::stw;
        case 37: return Op::stwu;
        case 38: return Op::stb;
        case 39: return Op::stbu;
        case 40: return Op::lhz;
        case 41: return Op::lhzu;
        case 42: return Op::lha;
        case 43: return Op::lhau;
        case 44: return Op::sth;
        case 45: return Op::sthu;
        case 46: return Op::lmw;
        case 47: return Op::stmw;
        case 48: return Op::lfs;
        case 49: return Op::lfsu;
        case 50: return Op::lfd;
        case 51: return Op::lfdu;
        case 52: return Op::stfs;
        case 53: return Op::stfsu;
        case 54: return Op::stfd;
        case 55: return Op::stfdu;
        case 56: return Op::psq_l;
        case 57: return Op::psq_lu;
        case 60: return Op::psq_st;
        case 61: return Op::psq_stu;
        default: return Op::invalid;
    }
}

bool unsigned_imm(Op op) {
    switch (op) {
        case Op::andi_rc: case Op::andis_rc: case Op::ori: case Op::oris: case Op::xori: case Op::xoris:
        case Op::cmpli:
            return true;
        default:
            return false;
    }
}

// Ops whose bit 31 is a record (Rc) flag rather than part of the encoding or reserved.
bool has_rc(Op op, uint32_t opcd) {
    if (opcd == 59 || opcd == 63) return op != Op::fcmpu && op != Op::fcmpo && op != Op::mcrfs;
    if (opcd == 4) return op != Op::ps_cmpu0 && op != Op::ps_cmpu1 && op != Op::ps_cmpo0 && op != Op::ps_cmpo1 &&
                          op != Op::psq_lx && op != Op::psq_stx && op != Op::psq_lux && op != Op::psq_stux &&
                          op != Op::dcbz_l;
    if (opcd == 20 || opcd == 21 || opcd == 23) return true;
    if (opcd == 31) {
        switch (op) {
            case Op::add: case Op::addc: case Op::adde: case Op::addme: case Op::addze: case Op::divw:
            case Op::divwu: case Op::mulhw: case Op::mulhwu: case Op::mullw: case Op::neg: case Op::subf:
            case Op::subfc: case Op::subfe: case Op::subfme: case Op::subfze: case Op::and_: case Op::andc:
            case Op::cntlzw: case Op::eqv: case Op::extsb: case Op::extsh: case Op::nand: case Op::nor:
            case Op::or_: case Op::orc: case Op::slw: case Op::sraw: case Op::srawi: case Op::srw: case Op::xor_:
                return true;
            default:
                return false;
        }
    }
    return false;
}

}  // namespace

const char* mnemonic(Op op) {
    return kMnemonics[static_cast<size_t>(op)];
}

Insn decode(uint32_t w) {
    Insn in;
    in.raw = w;
    const uint32_t opcd = w >> 26, xo10 = (w >> 1) & 0x3FF;
    in.d = (w >> 21) & 31;
    in.a = (w >> 16) & 31;
    in.b = (w >> 11) & 31;
    in.c = (w >> 6) & 31;
    in.e = (w >> 1) & 31;

    switch (opcd) {
        case 4: in.op = op4(xo10); break;
        case 19: in.op = op19(xo10); break;
        case 31:
            in.op = op31_arith(xo10 & 0x1FF);
            if (in.op != Op::invalid) in.oe = (w >> 10) & 1;
            else in.op = op31(xo10);
            break;
        case 59: in.op = op59(xo10 & 31); break;
        case 63: in.op = op63(xo10); break;
        default: in.op = primary(opcd); break;
    }
    if (in.op == Op::invalid) return in;
    if (has_rc(in.op, opcd)) in.rc = w & 1;

    switch (in.op) {
        case Op::b:
            in.imm = sext(w & 0x03FFFFFC, 26);
            in.aa = (w >> 1) & 1;
            in.lk = w & 1;
            break;
        case Op::bc:
            in.imm = sext(w & 0xFFFC, 16);
            in.aa = (w >> 1) & 1;
            in.lk = w & 1;
            break;
        case Op::bclr:
        case Op::bcctr:
            in.lk = w & 1;
            break;
        case Op::psq_l: case Op::psq_lu: case Op::psq_st: case Op::psq_stu:
            in.imm = sext(w & 0xFFF, 12);
            in.gqr = (w >> 12) & 7;
            in.w = (w >> 15) & 1;
            break;
        case Op::psq_lx: case Op::psq_lux: case Op::psq_stx: case Op::psq_stux:
            in.gqr = (w >> 7) & 7;
            in.w = (w >> 10) & 1;
            break;
        case Op::mfspr: case Op::mtspr: case Op::mftb:
            in.spr = static_cast<uint16_t>(((w >> 16) & 31) | (((w >> 11) & 31) << 5));
            break;
        case Op::mtcrf:
            in.fxm = (w >> 12) & 0xFF;
            break;
        case Op::mtfsf:
            in.fxm = (w >> 17) & 0xFF;
            break;
        default:
            if ((opcd >= 3 && opcd <= 15) || (opcd >= 24 && opcd <= 29) || (opcd >= 32 && opcd <= 55))
                in.imm = unsigned_imm(in.op) ? static_cast<int32_t>(w & 0xFFFF) : sext(w & 0xFFFF, 16);
            break;
    }
    in.crf = in.d >> 2;
    in.crfs = in.a >> 2;
    return in;
}

namespace {

std::string hex(int64_t v) {
    char buf[24];
    if (v < 0) std::snprintf(buf, sizeof buf, "-0x%llx", static_cast<unsigned long long>(-v));
    else std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}

std::string r(int n) { return "r" + std::to_string(n); }
std::string f(int n) { return "f" + std::to_string(n); }
std::string n(int v) { return std::to_string(v); }
std::string mem(const Insn& in, bool fp) { return (fp ? f(in.d) : r(in.d)) + "," + hex(in.imm) + "(" + r(in.a) + ")"; }

}  // namespace

std::string format(const Insn& in, uint32_t pc) {
    if (!in.valid()) {
        char buf[24];
        std::snprintf(buf, sizeof buf, ".long 0x%08x", in.raw);
        return buf;
    }
    std::string m = mnemonic(in.op);
    if (in.oe) m += "o";
    if (in.rc) m += ".";
    if (in.op == Op::b || in.op == Op::bc || in.op == Op::bclr || in.op == Op::bcctr) {
        if (in.lk) m += "l";
        if (in.aa) m += "a";
    }
    std::string ops;
    switch (in.op) {
        case Op::addi: case Op::addis: case Op::addic: case Op::addic_rc: case Op::mulli: case Op::subfic:
            ops = r(in.d) + "," + r(in.a) + "," + hex(in.imm); break;
        case Op::ori: case Op::oris: case Op::xori: case Op::xoris: case Op::andi_rc: case Op::andis_rc:
            ops = r(in.a) + "," + r(in.d) + "," + hex(in.imm); break;
        case Op::cmpi: case Op::cmpli:
            ops = n(in.crf) + "," + n(in.d & 1) + "," + r(in.a) + "," + hex(in.imm); break;
        case Op::cmp: case Op::cmpl:
            ops = n(in.crf) + "," + n(in.d & 1) + "," + r(in.a) + "," + r(in.b); break;
        case Op::twi: ops = n(in.d) + "," + r(in.a) + "," + hex(in.imm); break;
        case Op::tw: ops = n(in.d) + "," + r(in.a) + "," + r(in.b); break;
        case Op::lwz: case Op::lwzu: case Op::lbz: case Op::lbzu: case Op::stw: case Op::stwu: case Op::stb:
        case Op::stbu: case Op::lhz: case Op::lhzu: case Op::lha: case Op::lhau: case Op::sth: case Op::sthu:
        case Op::lmw: case Op::stmw:
            ops = mem(in, false); break;
        case Op::lfs: case Op::lfsu: case Op::lfd: case Op::lfdu: case Op::stfs: case Op::stfsu: case Op::stfd:
        case Op::stfdu:
            ops = mem(in, true); break;
        case Op::lfsx: case Op::lfsux: case Op::lfdx: case Op::lfdux: case Op::stfsx: case Op::stfsux:
        case Op::stfdx: case Op::stfdux: case Op::stfiwx:
            ops = f(in.d) + "," + r(in.a) + "," + r(in.b); break;
        case Op::psq_l: case Op::psq_lu: case Op::psq_st: case Op::psq_stu:
            ops = mem(in, true) + "," + n(in.w) + "," + n(in.gqr); break;
        case Op::psq_lx: case Op::psq_lux: case Op::psq_stx: case Op::psq_stux:
            ops = f(in.d) + "," + r(in.a) + "," + r(in.b) + "," + n(in.w) + "," + n(in.gqr); break;
        case Op::and_: case Op::andc: case Op::eqv: case Op::nand: case Op::nor: case Op::or_: case Op::orc:
        case Op::xor_: case Op::slw: case Op::sraw: case Op::srw:
            ops = r(in.a) + "," + r(in.d) + "," + r(in.b); break;
        case Op::cntlzw: case Op::extsb: case Op::extsh: ops = r(in.a) + "," + r(in.d); break;
        case Op::srawi: ops = r(in.a) + "," + r(in.d) + "," + n(in.b); break;
        case Op::rlwinm: case Op::rlwimi:
            ops = r(in.a) + "," + r(in.d) + "," + n(in.b) + "," + n(in.c) + "," + n(in.e); break;
        case Op::rlwnm: ops = r(in.a) + "," + r(in.d) + "," + r(in.b) + "," + n(in.c) + "," + n(in.e); break;
        case Op::neg: case Op::addme: case Op::addze: case Op::subfme: case Op::subfze:
            ops = r(in.d) + "," + r(in.a); break;
        case Op::add: case Op::addc: case Op::adde: case Op::divw: case Op::divwu: case Op::mulhw:
        case Op::mulhwu: case Op::mullw: case Op::subf: case Op::subfc: case Op::subfe:
        case Op::lbzx: case Op::lbzux: case Op::lhax: case Op::lhaux: case Op::lhbrx: case Op::lhzx:
        case Op::lhzux: case Op::lswx: case Op::lwarx: case Op::lwbrx: case Op::lwzx: case Op::lwzux:
        case Op::stbx: case Op::stbux: case Op::sthbrx: case Op::sthx: case Op::sthux: case Op::stswx:
        case Op::stwbrx: case Op::stwcx_rc: case Op::stwx: case Op::stwux: case Op::eciwx: case Op::ecowx:
            ops = r(in.d) + "," + r(in.a) + "," + r(in.b); break;
        case Op::b: {
            uint32_t t = in.aa ? static_cast<uint32_t>(in.imm) : pc + static_cast<uint32_t>(in.imm);
            ops = hex(t); break;
        }
        case Op::bc: {
            uint32_t t = in.aa ? static_cast<uint32_t>(in.imm) : pc + static_cast<uint32_t>(in.imm);
            ops = n(in.d) + "," + n(in.a) + "," + hex(t); break;
        }
        case Op::bclr: case Op::bcctr: ops = n(in.d) + "," + n(in.a); break;
        case Op::crand: case Op::crandc: case Op::creqv: case Op::crnand: case Op::crnor: case Op::cror:
        case Op::crorc: case Op::crxor:
            ops = n(in.d) + "," + n(in.a) + "," + n(in.b); break;
        case Op::mcrf: case Op::mcrfs: ops = n(in.crf) + "," + n(in.crfs); break;
        case Op::mcrxr: ops = n(in.crf); break;
        case Op::mfcr: case Op::mfmsr: ops = r(in.d); break;
        case Op::mtmsr: ops = r(in.d); break;
        case Op::mtcrf: ops = hex(in.fxm) + "," + r(in.d); break;
        case Op::mfspr: case Op::mftb: ops = r(in.d) + "," + n(in.spr); break;
        case Op::mtspr: ops = n(in.spr) + "," + r(in.d); break;
        case Op::mfsr: ops = r(in.d) + "," + n(in.a & 15); break;
        case Op::mtsr: ops = n(in.a & 15) + "," + r(in.d); break;
        case Op::mfsrin: case Op::mtsrin: ops = r(in.d) + "," + r(in.b); break;
        case Op::lswi: case Op::stswi: ops = r(in.d) + "," + r(in.a) + "," + n(in.b); break;
        case Op::dcbf: case Op::dcbi: case Op::dcbst: case Op::dcbt: case Op::dcbtst: case Op::dcbz:
        case Op::dcbz_l: case Op::icbi:
            ops = r(in.a) + "," + r(in.b); break;
        case Op::tlbie: ops = r(in.b); break;
        case Op::eieio: case Op::isync: case Op::sync: case Op::rfi: case Op::sc: case Op::tlbsync: break;
        // floating point / paired singles
        case Op::fadd: case Op::fadds: case Op::fsub: case Op::fsubs: case Op::fdiv: case Op::fdivs:
        case Op::ps_add: case Op::ps_sub: case Op::ps_div: case Op::ps_merge00: case Op::ps_merge01:
        case Op::ps_merge10: case Op::ps_merge11:
            ops = f(in.d) + "," + f(in.a) + "," + f(in.b); break;
        case Op::fmul: case Op::fmuls: case Op::ps_mul: case Op::ps_muls0: case Op::ps_muls1:
            ops = f(in.d) + "," + f(in.a) + "," + f(in.c); break;
        case Op::fmadd: case Op::fmadds: case Op::fmsub: case Op::fmsubs: case Op::fnmadd: case Op::fnmadds:
        case Op::fnmsub: case Op::fnmsubs: case Op::fsel: case Op::ps_madd: case Op::ps_msub: case Op::ps_nmadd:
        case Op::ps_nmsub: case Op::ps_sel: case Op::ps_madds0: case Op::ps_madds1: case Op::ps_sum0:
        case Op::ps_sum1:
            ops = f(in.d) + "," + f(in.a) + "," + f(in.c) + "," + f(in.b); break;
        case Op::fcmpu: case Op::fcmpo: case Op::ps_cmpu0: case Op::ps_cmpu1: case Op::ps_cmpo0:
        case Op::ps_cmpo1:
            ops = n(in.crf) + "," + f(in.a) + "," + f(in.b); break;
        case Op::mffs: ops = f(in.d); break;
        case Op::mtfsf: ops = hex(in.fxm) + "," + f(in.b); break;
        case Op::mtfsfi: ops = n(in.crf) + "," + n(in.b >> 1); break;
        case Op::mtfsb0: case Op::mtfsb1: ops = n(in.d); break;
        default:  // frD,frB unary ops (fmr, fneg, frsp, fctiw*, fres, frsqrte, fsqrt*, ps_mr, ps_res, ...)
            ops = f(in.d) + "," + f(in.b); break;
    }
    return ops.empty() ? m : m + " " + ops;
}

}  // namespace mh3u::ppc
