#include "emitter.h"

#include "espresso.h"
#include "ppc_context.h"

#include <cctype>
#include <algorithm>
#include <cstdio>
#include <set>
#include <sstream>

namespace mh3u::recomp {
namespace {

using ppc::Insn;
using ppc::Op;

std::string hex32(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08X", v);
    return b;
}
std::string label(uint32_t a) {
    char b[24];
    std::snprintf(b, sizeof b, "loc_%08X", a);
    return b;
}
std::string R(int n) { return "ctx.r[" + std::to_string(n) + "]"; }
std::string S(int n) { return "(int32_t)ctx.r[" + std::to_string(n) + "]"; }
std::string RA0(int n) { return n == 0 ? std::string("0u") : R(n); }
std::string F(int n) { return "ctx.f[" + std::to_string(n) + "]"; }
std::string imm32(int32_t v) { return hex32(static_cast<uint32_t>(v)) + "u"; }
std::string record0(int d) { return " ppc::cr_set_s(ctx.cr[0], " + S(d) + ", 0, ctx.xer_so);"; }

std::string cr_field_bit(uint32_t bi) {
    static const char* names[] = {"lt", "gt", "eq", "so"};
    return "ctx.cr[" + std::to_string(bi >> 2) + "]." + names[bi & 3];
}

// Branch condition for bc/bclr/bcctr from BO/BI. Returns "" for "always"; sets `dec` when CTR is decremented.
std::string branch_cond(const Insn& in, bool& dec) {
    const uint32_t bo = in.d, bi = in.a;
    std::string c;
    dec = !(bo & 4);
    if (dec) c = (bo & 2) ? "ctx.ctr == 0" : "ctx.ctr != 0";
    if (!(bo & 16)) {
        std::string t = (bo & 8) ? cr_field_bit(bi) : "!" + cr_field_bit(bi);
        c = c.empty() ? t : "(" + c + ") && " + t;
    }
    return c;
}

struct Ctx {
    std::vector<CodeRange> ranges;  // the function body: entry range + chunks
    std::set<uint32_t> labels;
    const EmitEnv* env;
    std::string error, error_key;
    bool in(uint32_t a) const {
        for (const auto& r : ranges)
            if (a >= r.start && a - r.start < r.words.size() * 4) return true;
        return false;
    }
};

// One instruction -> C++ statement(s). Returns false (with ctx.error) when not supported.
bool emit_insn(const Insn& in, uint32_t pc, Ctx& cx, std::string& out) {
    auto reject = [&](const std::string& key, const std::string& detail = {}) {
        cx.error_key = key;
        cx.error = key + (detail.empty() ? "" : " " + detail) + " at " + hex32(pc);
        return false;
    };
    const int d = in.d, a = in.a, b = in.b;
    std::ostringstream o;
    auto carry_add = [&](const std::string& x, const std::string& y, const std::string& z) {
        o << "uint64_t s = (uint64_t)" << x << " + (uint64_t)" << y << " + (uint64_t)" << z << "; " << R(d)
          << " = (uint32_t)s; ctx.xer_ca = (uint8_t)(s >> 32);";
    };
    auto ea_d = [&](bool update) { return update ? R(a) + " + " + imm32(in.imm) : RA0(a) + " + " + imm32(in.imm); };
    auto ea_x = [&](bool update) { return (update ? R(a) : RA0(a)) + " + " + R(b); };
    auto ld = [&](const std::string& fn, bool sign16, bool update, bool indexed) {
        o << "uint32_t ea = " << (indexed ? ea_x(update) : ea_d(update)) << "; ";
        if (sign16) o << R(d) << " = (uint32_t)(int32_t)(int16_t)ppc::" << fn << "(base, ea);";
        else o << R(d) << " = ppc::" << fn << "(base, ea);";
        if (update) o << " " << R(a) << " = ea;";
    };
    auto st = [&](const std::string& fn, const std::string& cast, bool update, bool indexed) {
        o << "uint32_t ea = " << (indexed ? ea_x(update) : ea_d(update)) << "; ppc::" << fn << "(base, ea, " << cast
          << R(d) << ");";
        if (update) o << " " << R(a) << " = ea;";
    };
    auto lfp = [&](bool single, bool update, bool indexed) {
        o << "uint32_t ea = " << (indexed ? ea_x(update) : ea_d(update)) << "; ";
        if (single) o << "double v = ppc::ldf32(base, ea); " << F(d) << ".ps0 = v; " << F(d) << ".ps1 = v;";
        else o << F(d) << ".ps0 = ppc::ldf64(base, ea);";
        if (update) o << " " << R(a) << " = ea;";
    };
    auto sfp = [&](bool single, bool update, bool indexed) {
        o << "uint32_t ea = " << (indexed ? ea_x(update) : ea_d(update)) << "; ";
        if (single) o << "ppc::stf32(base, ea, (float)" << F(d) << ".ps0);";
        else o << "ppc::stf64(base, ea, " << F(d) << ".ps0);";
        if (update) o << " " << R(a) << " = ea;";
    };
    auto fset = [&](const std::string& expr, bool single) {
        if (single) o << "double v = ppc::round_single(" << expr << "); " << F(d) << ".ps0 = v; " << F(d) << ".ps1 = v;";
        else o << F(d) << ".ps0 = " << expr << ";";
    };
    const std::string fa = F(a) + ".ps0", fb = F(b) + ".ps0", fc = F(in.c) + ".ps0";

    if (in.oe) return reject("overflow form (OE)", ppc::mnemonic(in.op));
    const bool fp_op = (in.raw >> 26) == 59 || (in.raw >> 26) == 63 || (in.raw >> 26) == 4;
    if (in.rc && fp_op) return reject("FP record form", ppc::mnemonic(in.op));

    switch (in.op) {
        // ---- integer arithmetic
        case Op::addi: o << R(d) << " = " << RA0(a) << " + " << imm32(in.imm) << ";"; break;
        case Op::addis: o << R(d) << " = " << RA0(a) << " + " << imm32(in.imm << 16) << ";"; break;
        case Op::addic: case Op::addic_rc: carry_add(R(a), imm32(in.imm), "0"); break;
        case Op::subfic: carry_add("(uint32_t)~" + R(a), imm32(in.imm), "1"); break;
        case Op::mulli: o << R(d) << " = (uint32_t)((int64_t)" << S(a) << " * " << in.imm << ");"; break;
        case Op::add: o << R(d) << " = " << R(a) << " + " << R(b) << ";"; break;
        case Op::addc: carry_add(R(a), R(b), "0"); break;
        case Op::adde: carry_add(R(a), R(b), "ctx.xer_ca"); break;
        case Op::addze: carry_add(R(a), "0", "ctx.xer_ca"); break;
        case Op::addme: carry_add(R(a), "0xFFFFFFFFu", "ctx.xer_ca"); break;
        case Op::subf: o << R(d) << " = " << R(b) << " - " << R(a) << ";"; break;
        case Op::subfc: carry_add("(uint32_t)~" + R(a), R(b), "1"); break;
        case Op::subfe: carry_add("(uint32_t)~" + R(a), R(b), "ctx.xer_ca"); break;
        case Op::subfze: carry_add("(uint32_t)~" + R(a), "0", "ctx.xer_ca"); break;
        case Op::subfme: carry_add("(uint32_t)~" + R(a), "0xFFFFFFFFu", "ctx.xer_ca"); break;
        case Op::neg: o << R(d) << " = 0u - " << R(a) << ";"; break;
        case Op::mullw: o << R(d) << " = (uint32_t)((int64_t)" << S(a) << " * " << S(b) << ");"; break;
        case Op::mulhw: o << R(d) << " = (uint32_t)(((int64_t)" << S(a) << " * " << S(b) << ") >> 32);"; break;
        case Op::mulhwu: o << R(d) << " = (uint32_t)(((uint64_t)" << R(a) << " * " << R(b) << ") >> 32);"; break;
        case Op::divw:  // undefined on hardware for /0 and INT_MIN/-1; Dolphin's results
            o << "int32_t x = " << S(a) << ", y = " << S(b) << "; " << R(d)
              << " = (y == 0 || (x == INT32_MIN && y == -1)) ? (x < 0 ? 0xFFFFFFFFu : 0u) : (uint32_t)(x / y);";
            break;
        case Op::divwu: o << R(d) << " = " << R(b) << " == 0 ? 0u : " << R(a) << " / " << R(b) << ";"; break;
        // ---- compare
        case Op::cmp: o << "ppc::cr_set_s(ctx.cr[" << +in.crf << "], " << S(a) << ", " << S(b) << ", ctx.xer_so);"; break;
        case Op::cmpl: o << "ppc::cr_set_u(ctx.cr[" << +in.crf << "], " << R(a) << ", " << R(b) << ", ctx.xer_so);"; break;
        case Op::cmpi: o << "ppc::cr_set_s(ctx.cr[" << +in.crf << "], " << S(a) << ", " << in.imm << ", ctx.xer_so);"; break;
        case Op::cmpli: o << "ppc::cr_set_u(ctx.cr[" << +in.crf << "], " << R(a) << ", " << imm32(in.imm) << ", ctx.xer_so);"; break;
        // ---- logical (dest rA, source rS = d)
        case Op::and_: o << R(a) << " = " << R(d) << " & " << R(b) << ";"; break;
        case Op::andc: o << R(a) << " = " << R(d) << " & ~" << R(b) << ";"; break;
        case Op::or_: o << R(a) << " = " << R(d) << " | " << R(b) << ";"; break;
        case Op::orc: o << R(a) << " = " << R(d) << " | ~" << R(b) << ";"; break;
        case Op::xor_: o << R(a) << " = " << R(d) << " ^ " << R(b) << ";"; break;
        case Op::nor: o << R(a) << " = ~(" << R(d) << " | " << R(b) << ");"; break;
        case Op::nand: o << R(a) << " = ~(" << R(d) << " & " << R(b) << ");"; break;
        case Op::eqv: o << R(a) << " = ~(" << R(d) << " ^ " << R(b) << ");"; break;
        case Op::andi_rc: o << R(a) << " = " << R(d) << " & " << imm32(in.imm) << ";"; break;
        case Op::andis_rc: o << R(a) << " = " << R(d) << " & " << imm32(in.imm << 16) << ";"; break;
        case Op::ori: o << R(a) << " = " << R(d) << " | " << imm32(in.imm) << ";"; break;
        case Op::oris: o << R(a) << " = " << R(d) << " | " << imm32(in.imm << 16) << ";"; break;
        case Op::xori: o << R(a) << " = " << R(d) << " ^ " << imm32(in.imm) << ";"; break;
        case Op::xoris: o << R(a) << " = " << R(d) << " ^ " << imm32(in.imm << 16) << ";"; break;
        case Op::cntlzw: o << R(a) << " = (uint32_t)std::countl_zero(" << R(d) << ");"; break;
        case Op::extsb: o << R(a) << " = (uint32_t)(int32_t)(int8_t)" << R(d) << ";"; break;
        case Op::extsh: o << R(a) << " = (uint32_t)(int32_t)(int16_t)" << R(d) << ";"; break;
        case Op::slw: o << "uint32_t n = " << R(b) << " & 63; " << R(a) << " = n > 31 ? 0u : " << R(d) << " << n;"; break;
        case Op::srw: o << "uint32_t n = " << R(b) << " & 63; " << R(a) << " = n > 31 ? 0u : " << R(d) << " >> n;"; break;
        case Op::sraw:
            o << "uint32_t n = " << R(b) << " & 63; int32_t v = " << S(d) << "; if (n > 31) { " << R(a)
              << " = (uint32_t)(v >> 31); ctx.xer_ca = v < 0; } else { " << R(a)
              << " = (uint32_t)(v >> n); ctx.xer_ca = v < 0 && ((uint32_t)v & ((1u << n) - 1)) != 0; }";
            break;
        case Op::srawi:
            o << "int32_t v = " << S(d) << "; " << R(a) << " = (uint32_t)(v >> " << b << "); ctx.xer_ca = v < 0 && ((uint32_t)v & "
              << hex32((1u << b) - 1) << "u) != 0;";
            break;
        case Op::rlwinm:
            o << R(a) << " = ppc::rotl32(" << R(d) << ", " << b << ") & " << hex32(ppc::mask32(in.c, in.e)) << "u;";
            break;
        case Op::rlwnm:
            o << R(a) << " = ppc::rotl32(" << R(d) << ", " << R(b) << ") & " << hex32(ppc::mask32(in.c, in.e)) << "u;";
            break;
        case Op::rlwimi: {
            uint32_t m = ppc::mask32(in.c, in.e);
            o << R(a) << " = (ppc::rotl32(" << R(d) << ", " << b << ") & " << hex32(m) << "u) | (" << R(a) << " & "
              << hex32(~m) << "u);";
            break;
        }
        // ---- integer load/store
        case Op::lbz: ld("ld8", false, false, false); break;
        case Op::lbzu: ld("ld8", false, true, false); break;
        case Op::lbzx: ld("ld8", false, false, true); break;
        case Op::lbzux: ld("ld8", false, true, true); break;
        case Op::lhz: ld("ld16", false, false, false); break;
        case Op::lhzu: ld("ld16", false, true, false); break;
        case Op::lhzx: ld("ld16", false, false, true); break;
        case Op::lhzux: ld("ld16", false, true, true); break;
        case Op::lha: ld("ld16", true, false, false); break;
        case Op::lhau: ld("ld16", true, true, false); break;
        case Op::lhax: ld("ld16", true, false, true); break;
        case Op::lhaux: ld("ld16", true, true, true); break;
        case Op::lwz: ld("ld32", false, false, false); break;
        case Op::lwzu: ld("ld32", false, true, false); break;
        case Op::lwzx: ld("ld32", false, false, true); break;
        case Op::lwzux: ld("ld32", false, true, true); break;
        case Op::lhbrx: ld("ld16_le", false, false, true); break;
        case Op::lwbrx: ld("ld32_le", false, false, true); break;
        case Op::stb: st("st8", "(uint8_t)", false, false); break;
        case Op::stbu: st("st8", "(uint8_t)", true, false); break;
        case Op::stbx: st("st8", "(uint8_t)", false, true); break;
        case Op::stbux: st("st8", "(uint8_t)", true, true); break;
        case Op::sth: st("st16", "(uint16_t)", false, false); break;
        case Op::sthu: st("st16", "(uint16_t)", true, false); break;
        case Op::sthx: st("st16", "(uint16_t)", false, true); break;
        case Op::sthux: st("st16", "(uint16_t)", true, true); break;
        case Op::stw: st("st32", "", false, false); break;
        case Op::stwu: st("st32", "", true, false); break;
        case Op::stwx: st("st32", "", false, true); break;
        case Op::stwux: st("st32", "", true, true); break;
        case Op::sthbrx: st("st16_le", "(uint16_t)", false, true); break;
        case Op::stwbrx: st("st32_le", "", false, true); break;
        case Op::lwarx: o << R(d) << " = ppc::lwarx(ctx, base, " << ea_x(false) << ");"; break;
        case Op::stwcx_rc: o << "ppc::stwcx(ctx, base, " << ea_x(false) << ", " << R(d) << ");"; break;
        case Op::lswi: o << "ppc::lswi(ctx, base, " << RA0(a) << ", " << d << ", " << (b ? b : 32) << ");"; break;
        case Op::stswi: o << "ppc::stswi(ctx, base, " << RA0(a) << ", " << d << ", " << (b ? b : 32) << ");"; break;
        case Op::tw:
            o << "if (ppc::trap_cond(" << R(a) << ", " << R(b) << ", " << d << ")) ppc::trap(" << hex32(pc) << "u);";
            break;
        case Op::twi:
            o << "if (ppc::trap_cond(" << R(a) << ", " << imm32(in.imm) << ", " << d << ")) ppc::trap(" << hex32(pc)
              << "u);";
            break;
        case Op::lmw:
            o << "uint32_t ea = " << ea_d(false) << "; for (int i = " << d << "; i < 32; ++i, ea += 4) ctx.r[i] = ppc::ld32(base, ea);";
            break;
        case Op::stmw:
            o << "uint32_t ea = " << ea_d(false) << "; for (int i = " << d << "; i < 32; ++i, ea += 4) ppc::st32(base, ea, ctx.r[i]);";
            break;
        // ---- FP load/store
        case Op::lfs: lfp(true, false, false); break;
        case Op::lfsu: lfp(true, true, false); break;
        case Op::lfsx: lfp(true, false, true); break;
        case Op::lfsux: lfp(true, true, true); break;
        case Op::lfd: lfp(false, false, false); break;
        case Op::lfdu: lfp(false, true, false); break;
        case Op::lfdx: lfp(false, false, true); break;
        case Op::lfdux: lfp(false, true, true); break;
        case Op::stfs: sfp(true, false, false); break;
        case Op::stfsu: sfp(true, true, false); break;
        case Op::stfsx: sfp(true, false, true); break;
        case Op::stfsux: sfp(true, true, true); break;
        case Op::stfd: sfp(false, false, false); break;
        case Op::stfdu: sfp(false, true, false); break;
        case Op::stfdx: sfp(false, false, true); break;
        case Op::stfdux: sfp(false, true, true); break;
        case Op::stfiwx:
            o << "ppc::st32(base, " << ea_x(false) << ", (uint32_t)std::bit_cast<uint64_t>(" << F(d) << ".ps0));";
            break;
        // ---- floating point (double ops write ps0; single ops round and write both lanes)
        case Op::fadd: fset(fa + " + " + fb, false); break;
        case Op::fadds: fset(fa + " + " + fb, true); break;
        case Op::fsub: fset(fa + " - " + fb, false); break;
        case Op::fsubs: fset(fa + " - " + fb, true); break;
        case Op::fmul: fset(fa + " * " + fc, false); break;
        case Op::fmuls: fset(fa + " * " + fc, true); break;
        case Op::fdiv: fset(fa + " / " + fb, false); break;
        case Op::fdivs: fset(fa + " / " + fb, true); break;
        case Op::fmadd: fset("std::fma(" + fa + ", " + fc + ", " + fb + ")", false); break;
        case Op::fmadds: fset("std::fma(" + fa + ", " + fc + ", " + fb + ")", true); break;
        case Op::fmsub: fset("std::fma(" + fa + ", " + fc + ", -" + fb + ")", false); break;
        case Op::fmsubs: fset("std::fma(" + fa + ", " + fc + ", -" + fb + ")", true); break;
        case Op::fnmadd: fset("-std::fma(" + fa + ", " + fc + ", " + fb + ")", false); break;
        case Op::fnmadds: fset("-std::fma(" + fa + ", " + fc + ", " + fb + ")", true); break;
        case Op::fnmsub: fset("-std::fma(" + fa + ", " + fc + ", -" + fb + ")", false); break;
        case Op::fnmsubs: fset("-std::fma(" + fa + ", " + fc + ", -" + fb + ")", true); break;
        case Op::fsel: fset("(" + fa + " >= 0.0 ? " + fc + " : " + fb + ")", false); break;
        case Op::fsqrt: fset("std::sqrt(" + fb + ")", false); break;
        case Op::fsqrts: fset("std::sqrt(" + fb + ")", true); break;
        case Op::fres: fset("1.0 / " + fb, true); break;  // hardware estimate; exact reciprocal here
        case Op::frsqrte: fset("1.0 / std::sqrt(" + fb + ")", false); break;
        case Op::fmr: o << F(d) << ".ps0 = " << fb << ";"; break;
        case Op::fneg: o << F(d) << ".ps0 = -" << fb << ";"; break;
        case Op::fabs: o << F(d) << ".ps0 = std::fabs(" << fb << ");"; break;
        case Op::fnabs: o << F(d) << ".ps0 = -std::fabs(" << fb << ");"; break;
        case Op::frsp: fset(fb, true); break;
        case Op::fctiwz: o << F(d) << ".ps0 = ppc::fctiw_bits(" << fb << ", true);"; break;
        case Op::fctiw: o << F(d) << ".ps0 = ppc::fctiw_bits(" << fb << ", false);"; break;  // assumes round-to-nearest
        case Op::fcmpu: case Op::fcmpo: o << "ppc::cr_set_f(ctx.cr[" << +in.crf << "], " << fa << ", " << fb << ");"; break;
        // ---- paired singles used by GHS prologues/epilogues
        case Op::ps_merge00: o << "double t0 = " << F(a) << ".ps0, t1 = " << F(b) << ".ps0; " << F(d) << ".ps0 = t0; " << F(d) << ".ps1 = t1;"; break;
        case Op::ps_merge01: o << "double t0 = " << F(a) << ".ps0, t1 = " << F(b) << ".ps1; " << F(d) << ".ps0 = t0; " << F(d) << ".ps1 = t1;"; break;
        case Op::ps_merge10: o << "double t0 = " << F(a) << ".ps1, t1 = " << F(b) << ".ps0; " << F(d) << ".ps0 = t0; " << F(d) << ".ps1 = t1;"; break;
        case Op::ps_merge11: o << "double t0 = " << F(a) << ".ps1, t1 = " << F(b) << ".ps1; " << F(d) << ".ps0 = t0; " << F(d) << ".ps1 = t1;"; break;
        // ---- condition register
        case Op::crand: o << cr_field_bit(d) << " = " << cr_field_bit(a) << " & " << cr_field_bit(b) << ";"; break;
        case Op::crandc: o << cr_field_bit(d) << " = " << cr_field_bit(a) << " & !" << cr_field_bit(b) << ";"; break;
        case Op::cror: o << cr_field_bit(d) << " = " << cr_field_bit(a) << " | " << cr_field_bit(b) << ";"; break;
        case Op::crorc: o << cr_field_bit(d) << " = " << cr_field_bit(a) << " | !" << cr_field_bit(b) << ";"; break;
        case Op::crxor: o << cr_field_bit(d) << " = " << cr_field_bit(a) << " ^ " << cr_field_bit(b) << ";"; break;
        case Op::creqv: o << cr_field_bit(d) << " = !(" << cr_field_bit(a) << " ^ " << cr_field_bit(b) << ");"; break;
        case Op::crnand: o << cr_field_bit(d) << " = !(" << cr_field_bit(a) << " & " << cr_field_bit(b) << ");"; break;
        case Op::crnor: o << cr_field_bit(d) << " = !(" << cr_field_bit(a) << " | " << cr_field_bit(b) << ");"; break;
        case Op::mcrf: o << "ctx.cr[" << +in.crf << "] = ctx.cr[" << +in.crfs << "];"; break;
        case Op::mfcr: o << R(d) << " = ppc::cr_pack(ctx);"; break;
        case Op::mtcrf: o << "ppc::cr_unpack(ctx, " << R(d) << ", " << +in.fxm << "u);"; break;
        // ---- special registers
        case Op::mfspr:
            if (in.spr == 8) o << R(d) << " = ctx.lr;";
            else if (in.spr == 9) o << R(d) << " = ctx.ctr;";
            else if (in.spr == 1) o << R(d) << " = ((uint32_t)ctx.xer_so << 31) | ((uint32_t)ctx.xer_ov << 30) | ((uint32_t)ctx.xer_ca << 29);";
            else if (in.spr >= 912 && in.spr <= 919) o << R(d) << " = ctx.gqr[" << in.spr - 912 << "];";
            else return reject("unsupported: mfspr", std::to_string(in.spr));
            break;
        case Op::mtspr:
            if (in.spr == 8) o << "ctx.lr = " << R(d) << ";";
            else if (in.spr == 9) o << "ctx.ctr = " << R(d) << ";";
            else if (in.spr == 1) o << "ctx.xer_so = " << R(d) << " >> 31; ctx.xer_ov = (" << R(d) << " >> 30) & 1; ctx.xer_ca = (" << R(d) << " >> 29) & 1;";
            else if (in.spr >= 912 && in.spr <= 919) o << "ctx.gqr[" << in.spr - 912 << "] = " << R(d) << ";";
            else return reject("unsupported: mtspr", std::to_string(in.spr));
            break;
        // ---- cache / ordering: no effect on the host, except dcbz which zeroes a 32-byte block
        case Op::sync: case Op::isync: case Op::eieio: case Op::dcbf: case Op::dcbst: case Op::dcbt: case Op::dcbtst:
        case Op::dcbi: case Op::icbi:
            o << "/* no-op on host */";
            break;
        case Op::dcbz: case Op::dcbz_l:
            o << "std::memset(base + ((" << RA0(a) << " + " << R(b) << ") & ~31u), 0, 32);";
            break;
        // ---- branches
        case Op::b: {
            const std::string imp = cx.env->import_call ? cx.env->import_call(pc) : std::string();
            if (!imp.empty()) {  // Cafe OS import (the encoded displacement is meaningless: stubs are out of range)
                if (in.lk) o << "ctx.lr = " << hex32(pc + 4) << "u; " << imp << "(ctx, base);";
                else o << imp << "(ctx, base); return;";
                break;
            }
            if (cx.env->undefined_call && cx.env->undefined_call(pc)) {
                // undefined weak symbol: the Cafe loader resolves it to 0; callers normally test the pointer first
                if (in.lk) o << "ctx.lr = " << hex32(pc + 4) << "u; ppc::call_indirect(ctx, base, 0u);  /* $UNDEF */";
                else o << "ppc::call_indirect(ctx, base, 0u); return;  /* $UNDEF */";
                break;
            }
            uint32_t t = in.aa ? static_cast<uint32_t>(in.imm) : pc + static_cast<uint32_t>(in.imm);
            const bool entry = cx.env->is_entry && cx.env->is_entry(t);
            if (in.lk) {
                if (t == pc + 4) o << "ctx.lr = " << hex32(pc + 4) << "u;  /* get-PC idiom */";
                else if (entry) o << "ctx.lr = " << hex32(pc + 4) << "u; " << function_name(t) << "(ctx, base);";
                else return reject(cx.env->is_entry ? "call to non-entry" : "call", hex32(t));
            } else if (cx.in(t)) {
                o << "goto " << label(t) << ";";
            } else if (entry) {
                o << function_name(t) << "(ctx, base); return;";
            } else {
                return reject("branch out of body", hex32(t));
            }
            break;
        }
        case Op::bc: {
            uint32_t t = in.aa ? static_cast<uint32_t>(in.imm) : pc + static_cast<uint32_t>(in.imm);
            bool dec;
            std::string c = branch_cond(in, dec);
            if (dec) o << "--ctx.ctr; ";
            if (in.lk) {  // conditional call
                if (!(cx.env->is_entry && cx.env->is_entry(t))) return reject("conditional call to non-entry", hex32(t));
                o << (c.empty() ? "{ " : "if (" + c + ") { ") << "ctx.lr = " << hex32(pc + 4) << "u; " << function_name(t)
                  << "(ctx, base); }";
            } else if (cx.in(t)) {
                o << (c.empty() ? "" : "if (" + c + ") ") << "goto " << label(t) << ";";
            } else if (cx.env->is_entry && cx.env->is_entry(t)) {
                o << (c.empty() ? "{ " : "if (" + c + ") { ") << function_name(t) << "(ctx, base); return; }";
            } else {
                return reject("branch out of body", hex32(t));
            }
            break;
        }
        case Op::bclr: {
            bool dec;
            std::string c = branch_cond(in, dec);
            if (dec) o << "--ctx.ctr; ";
            if (in.lk)  // call through LR: the target is the old LR, the new LR is the return address
                o << (c.empty() ? "{ " : "if (" + c + ") { ") << "uint32_t t = ctx.lr; ctx.lr = " << hex32(pc + 4)
                  << "u; ppc::call_indirect(ctx, base, t); }";
            else
                o << (c.empty() ? "" : "if (" + c + ") ") << "return;";
            break;
        }
        case Op::bcctr: {
            bool dec;
            std::string c = branch_cond(in, dec);
            if (dec) return reject("bcctr with CTR decrement");  // invalid form
            const std::string open = c.empty() ? "{ " : "if (" + c + ") { ";
            const std::vector<uint32_t>* slots = cx.env->switch_slots ? cx.env->switch_slots(pc) : nullptr;
            if (in.lk) {
                o << open << "ctx.lr = " << hex32(pc + 4) << "u; ppc::call_indirect(ctx, base, ctx.ctr); }";
            } else if (slots) {
                // inline switch table: CTR = address of a `b case` slot inside this body (config/switches.csv)
                o << open << "switch (ctx.ctr) {";
                for (uint32_t s : *slots) {
                    if (!cx.in(s)) return reject("switch slot out of body", hex32(s));
                    o << " case " << hex32(s) << "u: goto " << label(s) << ";";
                }
                o << " default: ppc::unimplemented_function(ctx.ctr); } }";
            } else {
                o << open << "ppc::call_indirect(ctx, base, ctx.ctr); return; }";  // indirect tail jump
            }
            break;
        }
        default: return reject(std::string("unsupported: ") + ppc::mnemonic(in.op));
    }
    if (in.rc && !fp_op) {
        // record forms set CR0 from the destination register (rA for logical/rotate ops, rD for arithmetic)
        bool dest_a = in.op == Op::and_ || in.op == Op::andc || in.op == Op::or_ || in.op == Op::orc || in.op == Op::xor_ ||
                      in.op == Op::nor || in.op == Op::nand || in.op == Op::eqv || in.op == Op::cntlzw ||
                      in.op == Op::extsb || in.op == Op::extsh || in.op == Op::slw || in.op == Op::srw ||
                      in.op == Op::sraw || in.op == Op::srawi || in.op == Op::rlwinm || in.op == Op::rlwnm ||
                      in.op == Op::rlwimi;
        o << record0(dest_a ? a : d);
    } else if (in.op == Op::andi_rc || in.op == Op::andis_rc) {
        o << record0(a);
    } else if (in.op == Op::addic_rc) {
        o << record0(d);
    }
    out = o.str();
    return true;
}

bool is_unconditional_end(const Insn& in) {
    if (in.op == Op::b) return true;  // jump/tail call, or a final call to a no-return function (exit, abort)
    if ((in.op == Op::bclr || in.op == Op::bcctr) && !in.lk && (in.d & 0x14) == 0x14) return true;
    return false;
}

}  // namespace

std::string function_name(uint32_t addr) {
    char b[24];
    std::snprintf(b, sizeof b, "sub_%08X", addr);
    return b;
}

std::string import_function_name(const std::string& rpl, const std::string& name) {
    std::string r = rpl;
    if (r.size() > 4 && r.compare(r.size() - 4, 4, ".rpl") == 0) r.resize(r.size() - 4);
    std::string out = "import_" + r + "_" + name;
    for (char& ch : out)
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_') ch = '_';
    return out;
}

EmitResult emit_leaf_function(uint32_t start, const std::vector<uint32_t>& words, const std::string& comment) {
    return emit_function(start, words, comment, EmitEnv{});
}

EmitResult emit_function(uint32_t start, const std::vector<uint32_t>& words, const std::string& comment,
                         const EmitEnv& env) {
    return emit_function(start, std::vector<CodeRange>{{start, words}}, comment, env);
}

EmitResult emit_function(uint32_t entry, const std::vector<CodeRange>& ranges_in, const std::string& comment,
                         const EmitEnv& env) {
    EmitResult res;
    auto fail = [&](const std::string& key, const std::string& detail = {}) {
        res.reason_key = key;
        res.reason = key + (detail.empty() ? "" : " " + detail);
        return res;
    };
    std::vector<CodeRange> ranges = ranges_in;
    std::sort(ranges.begin(), ranges.end(), [](const CodeRange& x, const CodeRange& y) { return x.start < y.start; });
    Ctx cx{ranges, {}, &env, {}, {}};
    if (!cx.in(entry)) return fail("entry outside body");

    struct Item {
        uint32_t pc;
        Insn in;
        bool range_end;
    };
    std::vector<Item> items;
    for (const auto& r : ranges)
        for (size_t i = 0; i < r.words.size(); ++i)
            items.push_back({r.start + static_cast<uint32_t>(i * 4), ppc::decode(r.words[i]), i + 1 == r.words.size()});
    if (items.empty()) return fail("empty body");

    // pass 1: validity and labels
    for (const Item& it : items) {
        const Insn& in = it.in;
        if (!in.valid()) return fail("invalid instruction", "at " + hex32(it.pc));
        if ((in.op == Op::b || in.op == Op::bc) && !in.lk) {
            uint32_t t = in.aa ? static_cast<uint32_t>(in.imm) : it.pc + static_cast<uint32_t>(in.imm);
            if (cx.in(t)) cx.labels.insert(t);
        }
        if (in.op == Op::bcctr && !in.lk && env.switch_slots)
            if (const auto* slots = env.switch_slots(it.pc))
                for (uint32_t s : *slots) cx.labels.insert(s);
        // a range that does not end in a jump/return continues at the following address
        if (it.range_end && !is_unconditional_end(in) && cx.in(it.pc + 4)) cx.labels.insert(it.pc + 4);
    }
    const bool entry_first = items.front().pc == entry;
    if (!entry_first) cx.labels.insert(entry);

    std::ostringstream o;
    o << "// " << hex32(entry) << (comment.empty() ? "" : " " + comment);
    if (ranges.size() > 1) o << " (" << ranges.size() << " ranges)";
    o << "\n";
    o << "PPC_FUNC(" << function_name(entry) << ") {\n";
    if (!entry_first) o << "    goto " << label(entry) << ";\n";
    for (const Item& it : items) {
        std::string stmt;
        if (!emit_insn(it.in, it.pc, cx, stmt)) {
            res.reason = cx.error;
            res.reason_key = cx.error_key;
            return res;
        }
        if (cx.labels.count(it.pc)) o << label(it.pc) << ":\n";
        o << "    { " << stmt << " }  // " << ppc::format(it.in, it.pc) << "\n";
        if (it.range_end && !is_unconditional_end(it.in)) {
            // execution runs on past the end of this range
            const uint32_t next = it.pc + 4;
            if (cx.in(next)) o << "    goto " << label(next) << ";\n";
            else if (env.is_entry && env.is_entry(next))
                o << "    " << function_name(next) << "(ctx, base); return;  // falls through into the next function\n";
            else return fail("falls off end", "into " + hex32(next));
        }
    }
    o << "}\n";
    res.ok = true;
    res.code = o.str();
    return res;
}

}  // namespace mh3u::recomp
