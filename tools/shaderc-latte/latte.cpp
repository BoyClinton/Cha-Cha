#include "latte.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace mh3u::latte {
namespace {

uint32_t bits(uint32_t v, int lo, int n) { return (v >> lo) & ((1u << n) - 1); }

uint32_t le32(const uint8_t* p) { return uint32_t{p[0]} | uint32_t{p[1]} << 8 | uint32_t{p[2]} << 16 | uint32_t{p[3]} << 24; }

int8_t sext(uint32_t v, int n) { return static_cast<int8_t>(static_cast<int32_t>(v << (32 - n)) >> (32 - n)); }

const char* op2_name(uint16_t op) {
    switch (op) {
#define N(x) case OP2_##x: return #x;
        N(ADD) N(MUL) N(MUL_IEEE) N(MAX) N(MIN) N(MAX_DX10) N(MIN_DX10) N(SETE) N(SETGT) N(SETGE) N(SETNE)
        N(SETE_DX10) N(SETGT_DX10) N(SETGE_DX10) N(SETNE_DX10) N(FRACT) N(TRUNC) N(CEIL) N(RNDNE) N(FLOOR) N(MOVA)
        N(MOVA_FLOOR) N(MOVA_INT) N(MOV) N(NOP) N(PRED_SETGT_UINT) N(PRED_SETGE_UINT) N(PRED_SETE) N(PRED_SETGT)
        N(PRED_SETGE) N(PRED_SETNE) N(PRED_SET_INV) N(PRED_SET_POP) N(PRED_SET_CLR) N(PRED_SET_RESTORE)
        N(PRED_SETE_PUSH) N(PRED_SETGT_PUSH) N(PRED_SETGE_PUSH) N(PRED_SETNE_PUSH) N(KILLE) N(KILLGT) N(KILLGE)
        N(KILLNE) N(AND_INT) N(OR_INT) N(XOR_INT) N(NOT_INT) N(ADD_INT) N(SUB_INT) N(MAX_INT) N(MIN_INT) N(MAX_UINT)
        N(MIN_UINT) N(SETE_INT) N(SETGT_INT) N(SETGE_INT) N(SETNE_INT) N(SETGT_UINT) N(SETGE_UINT) N(KILLGT_UINT)
        N(KILLGE_UINT) N(PRED_SETE_INT) N(PRED_SETGT_INT) N(PRED_SETGE_INT) N(PRED_SETNE_INT) N(KILLE_INT)
        N(KILLGT_INT) N(KILLGE_INT) N(KILLNE_INT) N(PRED_SETE_PUSH_INT) N(PRED_SETGT_PUSH_INT)
        N(PRED_SETGE_PUSH_INT) N(PRED_SETNE_PUSH_INT) N(PRED_SETLT_PUSH_INT) N(PRED_SETLE_PUSH_INT) N(DOT4)
        N(DOT4_IEEE) N(CUBE) N(MAX4) N(MOVA_GPR_INT) N(EXP_IEEE) N(LOG_CLAMPED) N(LOG_IEEE) N(RECIP_CLAMPED)
        N(RECIP_FF) N(RECIP_IEEE) N(RECIPSQRT_CLAMPED) N(RECIPSQRT_FF) N(RECIPSQRT_IEEE) N(SQRT_IEEE)
        N(FLT_TO_INT) N(INT_TO_FLT) N(UINT_TO_FLT) N(SIN) N(COS) N(ASHR_INT) N(LSHR_INT) N(LSHL_INT) N(MULLO_INT)
        N(MULHI_INT) N(MULLO_UINT) N(MULHI_UINT) N(RECIP_INT) N(RECIP_UINT) N(FLT_TO_UINT)
#undef N
        default: return nullptr;
    }
}

const char* op3_name(uint16_t op) {
    switch (op) {
#define N(x) case OP3_##x: return #x;
        N(MUL_LIT) N(MUL_LIT_M2) N(MUL_LIT_M4) N(MUL_LIT_D2) N(MULADD) N(MULADD_M2) N(MULADD_M4) N(MULADD_D2)
        N(MULADD_IEEE) N(MULADD_IEEE_M2) N(MULADD_IEEE_M4) N(MULADD_IEEE_D2) N(CNDE) N(CNDGT) N(CNDGE) N(CNDE_INT)
        N(CNDGT_INT) N(CNDGE_INT)
#undef N
        default: return nullptr;
    }
}

bool is_unary(uint16_t op) {
    switch (op) {
        case OP2_FRACT: case OP2_TRUNC: case OP2_CEIL: case OP2_RNDNE: case OP2_FLOOR: case OP2_MOVA:
        case OP2_MOVA_FLOOR: case OP2_MOVA_INT: case OP2_MOV: case OP2_NOT_INT: case OP2_MOVA_GPR_INT:
        case OP2_EXP_IEEE: case OP2_LOG_CLAMPED: case OP2_LOG_IEEE: case OP2_RECIP_CLAMPED: case OP2_RECIP_FF:
        case OP2_RECIP_IEEE: case OP2_RECIPSQRT_CLAMPED: case OP2_RECIPSQRT_FF: case OP2_RECIPSQRT_IEEE:
        case OP2_SQRT_IEEE: case OP2_FLT_TO_INT: case OP2_INT_TO_FLT: case OP2_UINT_TO_FLT: case OP2_SIN:
        case OP2_COS: case OP2_RECIP_INT: case OP2_RECIP_UINT: case OP2_FLT_TO_UINT: case OP2_PRED_SET_INV:
        case OP2_PRED_SET_POP: case OP2_PRED_SET_CLR: case OP2_PRED_SET_RESTORE:
            return true;
        default:
            return false;
    }
}

const char* cf_name(uint8_t inst) {
    switch (inst) {
#define N(x) case CF_##x: return #x;
        N(NOP) N(TEX) N(VTX) N(VTX_TC) N(LOOP_START) N(LOOP_END) N(LOOP_START_DX10) N(LOOP_START_NO_AL)
        N(LOOP_CONTINUE) N(LOOP_BREAK) N(JUMP) N(PUSH) N(PUSH_ELSE) N(ELSE) N(POP) N(POP_JUMP) N(POP_PUSH)
        N(POP_PUSH_ELSE) N(CALL) N(CALL_FS) N(RETURN) N(EMIT_VERTEX) N(EMIT_CUT_VERTEX) N(CUT_VERTEX) N(KILL)
        N(WAIT_ACK) N(TEX_ACK) N(VTX_ACK) N(VTX_TC_ACK) N(MEM_STREAM0) N(MEM_STREAM1) N(MEM_STREAM2)
        N(MEM_STREAM3) N(MEM_SCRATCH) N(MEM_REDUCTION) N(MEM_RING) N(EXPORT) N(EXPORT_DONE) N(ALU)
        N(ALU_PUSH_BEFORE) N(ALU_POP_AFTER) N(ALU_POP2_AFTER) N(ALU_CONTINUE) N(ALU_BREAK) N(ALU_ELSE_AFTER)
#undef N
        default: return nullptr;
    }
}

const char* tex_name(uint8_t inst) {
    switch (inst) {
#define N(x) case TEX_##x: return #x;
        N(VTX_FETCH) N(VTX_SEMANTIC) N(LD) N(GET_TEXTURE_INFO) N(GET_NUMBER_OF_SAMPLES) N(GET_COMP_TEX_LOD)
        N(GET_GRADIENTS_H) N(GET_GRADIENTS_V) N(GET_LERP) N(KEEP_GRADIENTS) N(SET_GRADIENTS_H) N(SET_GRADIENTS_V)
        N(PASS) N(SET_CUBEMAP_INDEX) N(FETCH4) N(SAMPLE) N(SAMPLE_L) N(SAMPLE_LB) N(SAMPLE_LZ) N(SAMPLE_G)
        N(SAMPLE_G_L) N(SAMPLE_G_LB) N(SAMPLE_G_LZ) N(SAMPLE_C) N(SAMPLE_C_L) N(SAMPLE_C_LB) N(SAMPLE_C_LZ)
        N(SAMPLE_C_G) N(SAMPLE_C_G_L) N(SAMPLE_C_G_LB) N(SAMPLE_C_G_LZ)
#undef N
        default: return nullptr;
    }
}

void appendf(std::string& s, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    s += buf;
}

void err(Program& p, const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    p.errors.emplace_back(buf);
}

AluSrc src_from(uint32_t sel, uint32_t rel, uint32_t chan, uint32_t neg) {
    AluSrc s;
    s.sel = static_cast<uint16_t>(sel);
    s.rel = rel != 0;
    s.chan = static_cast<uint8_t>(chan);
    s.neg = neg != 0;
    return s;
}

void decode_alu_clause(Program& p, Cf& cf, const uint8_t* data, size_t size) {
    const size_t begin = size_t{cf.addr} * 8, end = begin + size_t{cf.count} * 8;
    if (end > size) return err(p, "CF %u: ALU clause [0x%zx, 0x%zx) outside program", cf.index, begin, end);
    uint32_t slot = 0;
    while (slot < cf.count) {
        AluGroup g;
        g.slot = slot;
        bool used[5] = {};
        int literal_chans = 0;
        for (;;) {
            if (slot >= cf.count) return err(p, "CF %u: ALU group runs past the clause end", cf.index);
            const uint8_t* q = data + begin + size_t{slot} * 8;
            const uint32_t w0 = le32(q), w1 = le32(q + 4);
            ++slot;
            AluInst a;
            a.src[0] = src_from(bits(w0, 0, 9), bits(w0, 9, 1), bits(w0, 10, 2), bits(w0, 12, 1));
            a.src[1] = src_from(bits(w0, 13, 9), bits(w0, 22, 1), bits(w0, 23, 2), bits(w0, 25, 1));
            a.index_mode = static_cast<uint8_t>(bits(w0, 26, 3));
            a.pred_sel = static_cast<uint8_t>(bits(w0, 29, 2));
            a.last = bits(w0, 31, 1) != 0;
            a.op3 = bits(w1, 15, 3) != 0;
            if (a.op3) {
                a.src[2] = src_from(bits(w1, 0, 9), bits(w1, 9, 1), bits(w1, 10, 2), bits(w1, 12, 1));
                a.op = static_cast<uint16_t>(bits(w1, 13, 5));
                a.write = true;
            } else {
                a.src[0].abs = bits(w1, 0, 1) != 0;
                a.src[1].abs = bits(w1, 1, 1) != 0;
                a.update_exec_mask = bits(w1, 2, 1) != 0;
                a.update_pred = bits(w1, 3, 1) != 0;
                a.write = bits(w1, 4, 1) != 0;
                a.omod = static_cast<uint8_t>(bits(w1, 5, 2));
                a.op = static_cast<uint16_t>(bits(w1, 7, 11));
            }
            a.bank_swizzle = static_cast<uint8_t>(bits(w1, 18, 3));
            a.dst_gpr = static_cast<uint8_t>(bits(w1, 21, 7));
            a.dst_rel = bits(w1, 28, 1) != 0;
            a.dst_chan = static_cast<uint8_t>(bits(w1, 29, 2));
            a.clamp = bits(w1, 31, 1) != 0;
            if (!a.name()) err(p, "CF %u slot %u: unknown %s opcode 0x%X", cf.index, slot - 1, a.op3 ? "OP3" : "OP2", a.op);
            if (a.op3 || !is_trans_only(a.op)) a.unit = used[a.dst_chan] ? 4 : a.dst_chan;
            else a.unit = 4;
            if (used[a.unit]) err(p, "CF %u slot %u: ALU unit %c used twice in one group", cf.index, slot - 1, "xyzwt"[a.unit]);
            used[a.unit] = true;
            for (int i = 0; i < a.num_src(); ++i)
                if (a.src[i].sel == SEL_LITERAL) literal_chans = std::max(literal_chans, a.src[i].chan >= 2 ? 4 : 2);
            g.insts.push_back(a);
            if (a.last) break;
        }
        if (literal_chans) {
            const uint32_t nslots = literal_chans / 2;
            if (slot + nslots > cf.count) return err(p, "CF %u: literals past the clause end", cf.index);
            for (int i = 0; i < literal_chans; ++i) g.literals[i] = le32(data + begin + size_t{slot} * 8 + i * 4);
            slot += nslots;
        }
        cf.alu.push_back(std::move(g));
    }
}

VtxFetch decode_vtx(const uint8_t* q) {
    const uint32_t w0 = le32(q), w1 = le32(q + 4), w2 = le32(q + 8);
    VtxFetch v;
    v.inst = static_cast<uint8_t>(bits(w0, 0, 5));
    v.fetch_type = static_cast<uint8_t>(bits(w0, 5, 2));
    v.fetch_whole_quad = bits(w0, 7, 1) != 0;
    v.buffer_id = static_cast<uint8_t>(bits(w0, 8, 8));
    v.src_gpr = static_cast<uint8_t>(bits(w0, 16, 7));
    v.src_rel = bits(w0, 23, 1) != 0;
    v.src_sel_x = static_cast<uint8_t>(bits(w0, 24, 2));
    v.mega_fetch_count = static_cast<uint8_t>(bits(w0, 26, 6));
    v.dst_gpr = static_cast<uint8_t>(bits(w1, 0, 7));
    v.semantic_id = static_cast<uint8_t>(bits(w1, 0, 8));
    v.dst_rel = bits(w1, 7, 1) != 0;
    for (int c = 0; c < 4; ++c) v.dst_sel[c] = static_cast<uint8_t>(bits(w1, 9 + 3 * c, 3));
    v.use_const_fields = bits(w1, 21, 1) != 0;
    v.data_format = static_cast<uint8_t>(bits(w1, 22, 6));
    v.num_format_all = static_cast<uint8_t>(bits(w1, 28, 2));
    v.format_comp_all = bits(w1, 30, 1) != 0;
    v.srf_mode_all = bits(w1, 31, 1) != 0;
    v.offset = static_cast<uint16_t>(bits(w2, 0, 16));
    v.endian_swap = static_cast<uint8_t>(bits(w2, 16, 2));
    v.const_buf_no_stride = bits(w2, 18, 1) != 0;
    v.mega_fetch = bits(w2, 19, 1) != 0;
    return v;
}

TexFetch decode_tex(const uint8_t* q) {
    const uint32_t w0 = le32(q), w1 = le32(q + 4), w2 = le32(q + 8);
    TexFetch t;
    t.inst = static_cast<uint8_t>(bits(w0, 0, 5));
    t.bc_frac_mode = bits(w0, 5, 1) != 0;
    t.fetch_whole_quad = bits(w0, 7, 1) != 0;
    t.resource_id = static_cast<uint8_t>(bits(w0, 8, 8));
    t.src_gpr = static_cast<uint8_t>(bits(w0, 16, 7));
    t.src_rel = bits(w0, 23, 1) != 0;
    t.alt_const = bits(w0, 24, 1) != 0;
    t.dst_gpr = static_cast<uint8_t>(bits(w1, 0, 7));
    t.dst_rel = bits(w1, 7, 1) != 0;
    for (int c = 0; c < 4; ++c) t.dst_sel[c] = static_cast<uint8_t>(bits(w1, 9 + 3 * c, 3));
    t.lod_bias = sext(bits(w1, 21, 7), 7);
    for (int c = 0; c < 4; ++c) t.coord_normalized[c] = bits(w1, 28 + c, 1) != 0;
    for (int c = 0; c < 3; ++c) t.offset[c] = sext(bits(w2, 5 * c, 5), 5);
    t.sampler_id = static_cast<uint8_t>(bits(w2, 15, 5));
    for (int c = 0; c < 4; ++c) t.src_sel[c] = static_cast<uint8_t>(bits(w2, 20 + 3 * c, 3));
    return t;
}

void decode_fetch_clause(Program& p, Cf& cf, const uint8_t* data, size_t size) {
    const size_t begin = size_t{cf.addr} * 8, end = begin + size_t{cf.count} * 16;
    if (end > size) return err(p, "CF %u: fetch clause [0x%zx, 0x%zx) outside program", cf.index, begin, end);
    for (uint32_t i = 0; i < cf.count; ++i) {
        const uint8_t* q = data + begin + size_t{i} * 16;
        const uint32_t inst = bits(le32(q), 0, 5);
        // A TEX clause can hold vertex (buffer) fetches; a VTX clause only those.
        if (cf.inst != CF_TEX || inst == TEX_VTX_FETCH || inst == TEX_VTX_SEMANTIC) {
            cf.vtx.push_back(decode_vtx(q));
        } else {
            TexFetch t = decode_tex(q);
            if (!t.name()) err(p, "CF %u: unknown TEX instruction %u", cf.index, t.inst);
            cf.tex.push_back(t);
        }
    }
}

}  // namespace

bool is_trans_only(uint16_t op) {
    switch (op) {
        case OP2_MOVA_GPR_INT: case OP2_EXP_IEEE: case OP2_LOG_CLAMPED: case OP2_LOG_IEEE: case OP2_RECIP_CLAMPED:
        case OP2_RECIP_FF: case OP2_RECIP_IEEE: case OP2_RECIPSQRT_CLAMPED: case OP2_RECIPSQRT_FF:
        case OP2_RECIPSQRT_IEEE: case OP2_SQRT_IEEE: case OP2_FLT_TO_INT: case OP2_INT_TO_FLT: case OP2_UINT_TO_FLT:
        case OP2_SIN: case OP2_COS: case OP2_MULLO_INT: case OP2_MULHI_INT: case OP2_MULLO_UINT:
        case OP2_MULHI_UINT: case OP2_RECIP_INT: case OP2_RECIP_UINT: case OP2_FLT_TO_UINT:
            return true;
        default:
            return false;
    }
}

bool is_reduction(const AluInst& i) {
    return !i.op3 && (i.op == OP2_DOT4 || i.op == OP2_DOT4_IEEE || i.op == OP2_CUBE || i.op == OP2_MAX4);
}

int AluInst::num_src() const {
    if (op3) return 3;
    if (op == OP2_NOP) return 0;
    return is_unary(op) ? 1 : 2;
}

const char* AluInst::name() const { return op3 ? op3_name(op) : op2_name(op); }
const char* TexFetch::name() const { return tex_name(inst); }
const char* Cf::name() const { return cf_name(inst); }

Program decode(const uint8_t* data, size_t size) {
    Program p;
    for (uint32_t i = 0;; ++i) {
        if (size_t{i} * 8 + 8 > size) {
            err(p, "CF list runs off the program end (no END_OF_PROGRAM)");
            break;
        }
        Cf cf;
        cf.index = i;
        cf.w0 = le32(data + i * 8);
        cf.w1 = le32(data + i * 8 + 4);
        cf.barrier = bits(cf.w1, 31, 1) != 0;
        cf.whole_quad_mode = bits(cf.w1, 30, 1) != 0;
        if (bits(cf.w1, 29, 1)) {
            cf.kind = CfKind::Alu;
            cf.inst = static_cast<uint8_t>(0x40 | bits(cf.w1, 26, 4));
            cf.addr = bits(cf.w0, 0, 22);
            cf.kcache[0] = {static_cast<uint8_t>(bits(cf.w0, 22, 4)), static_cast<uint8_t>(bits(cf.w0, 30, 2)),
                            static_cast<uint8_t>(bits(cf.w1, 2, 8))};
            cf.kcache[1] = {static_cast<uint8_t>(bits(cf.w0, 26, 4)), static_cast<uint8_t>(bits(cf.w1, 0, 2)),
                            static_cast<uint8_t>(bits(cf.w1, 10, 8))};
            cf.count = bits(cf.w1, 18, 7) + 1;
            cf.alt_const = bits(cf.w1, 25, 1) != 0;
            if (!cf.name()) err(p, "CF %u: unknown ALU CF instruction 0x%X", i, cf.inst);
            else decode_alu_clause(p, cf, data, size);
        } else {
            cf.inst = static_cast<uint8_t>(bits(cf.w1, 23, 7));
            cf.end_of_program = bits(cf.w1, 21, 1) != 0;
            cf.valid_pixel_mode = bits(cf.w1, 22, 1) != 0;
            if (cf.inst >= CF_MEM_STREAM0) {
                cf.kind = CfKind::Export;
                cf.array_base = static_cast<uint16_t>(bits(cf.w0, 0, 13));
                cf.export_type = static_cast<uint8_t>(bits(cf.w0, 13, 2));
                cf.rw_gpr = static_cast<uint8_t>(bits(cf.w0, 15, 7));
                cf.rw_rel = bits(cf.w0, 22, 1) != 0;
                cf.index_gpr = static_cast<uint8_t>(bits(cf.w0, 23, 7));
                cf.elem_size = static_cast<uint8_t>(bits(cf.w0, 30, 2));
                for (int c = 0; c < 4; ++c) cf.sel[c] = static_cast<uint8_t>(bits(cf.w1, 3 * c, 3));
                cf.array_size = static_cast<uint16_t>(bits(cf.w1, 0, 12));
                cf.comp_mask = static_cast<uint8_t>(bits(cf.w1, 12, 4));
                cf.burst_count = static_cast<uint8_t>(bits(cf.w1, 17, 4));
            } else {
                cf.addr = cf.w0;
                cf.pop_count = static_cast<uint8_t>(bits(cf.w1, 0, 3));
                cf.cf_const = static_cast<uint8_t>(bits(cf.w1, 3, 5));
                cf.cond = static_cast<uint8_t>(bits(cf.w1, 8, 2));
                cf.count = (bits(cf.w1, 10, 3) | bits(cf.w1, 19, 1) << 3) + 1;
                cf.call_count = static_cast<uint8_t>(bits(cf.w1, 13, 6));
            }
            if (!cf.name()) err(p, "CF %u: unknown CF instruction 0x%X", i, cf.inst);
            else if (cf.inst == CF_TEX || cf.inst == CF_VTX || cf.inst == CF_VTX_TC) decode_fetch_clause(p, cf, data, size);
        }
        const bool eop = cf.end_of_program;
        p.cf.push_back(std::move(cf));
        if (eop || !p.errors.empty()) break;
    }
    return p;
}

// ---- disassembly ----------------------------------------------------------------------------------------------
namespace {

const char kChan[] = "xyzw";
const char kSel[] = "xyzw01?_";

std::string src_text(const AluSrc& s, const AluGroup& g, const Cf& cf) {
    std::string t;
    if (s.neg) t += '-';
    if (s.abs) t += '|';
    if (s.sel <= SEL_GPR_LAST) {
        appendf(t, "R%u", s.sel);
    } else if (s.sel < SEL_KCACHE1) {
        appendf(t, "KC0[%u]", s.sel - SEL_KCACHE0);
    } else if (s.sel < 192) {
        appendf(t, "KC1[%u]", s.sel - SEL_KCACHE1);
    } else if (s.sel >= SEL_CFILE) {
        appendf(t, "C%u", s.sel - SEL_CFILE);
    } else {
        switch (s.sel) {
            case SEL_0: t += "0.0"; break;
            case SEL_1: t += "1.0"; break;
            case SEL_1_INT: t += "1"; break;
            case SEL_M_1_INT: t += "-1"; break;
            case SEL_0_5: t += "0.5"; break;
            case SEL_LITERAL: {
                const uint32_t v = g.literals[s.chan];
                float f;
                std::memcpy(&f, &v, 4);
                appendf(t, "(0x%08X, %g)", v, f);
                break;
            }
            case SEL_PV: t += "PV"; break;
            case SEL_PS: t += "PS"; break;
            default: appendf(t, "SEL%u", s.sel); break;
        }
    }
    if (s.rel) t += "[AR]";
    if (s.sel <= SEL_KCACHE1 + 31 || s.sel >= SEL_CFILE || s.sel == SEL_PV) appendf(t, ".%c", kChan[s.chan]);
    if (s.abs) t += '|';
    (void)cf;
    return t;
}

}  // namespace

std::string disassemble(const Program& p) {
    std::string out;
    for (const Cf& cf : p.cf) {
        appendf(out, "%02u %s", cf.index, cf.name() ? cf.name() : "???");
        if (cf.kind == CfKind::Alu) {
            appendf(out, ": ADDR(%u) CNT(%u)", cf.addr, cf.count);
            for (int k = 0; k < 2; ++k)
                if (cf.kcache[k].mode)
                    appendf(out, " KCACHE%d(CB%u:%u-%u)", k, cf.kcache[k].bank, cf.kcache[k].addr * 16,
                            cf.kcache[k].addr * 16 + (cf.kcache[k].mode == 2 ? 31 : 15));
        } else if (cf.kind == CfKind::Export) {
            static const char* kType[] = {"PIX", "POS", "PARAM", "?"};
            appendf(out, ": %s%u, R%u.%c%c%c%c", kType[cf.export_type & 3], cf.array_base, cf.rw_gpr, kSel[cf.sel[0]],
                    kSel[cf.sel[1]], kSel[cf.sel[2]], kSel[cf.sel[3]]);
            if (cf.burst_count) appendf(out, " BURST(%u)", cf.burst_count + 1);
            if (cf.rw_rel) out += " REL";
        } else {
            if (cf.inst == CF_TEX || cf.inst == CF_VTX || cf.inst == CF_VTX_TC) appendf(out, ": ADDR(%u) CNT(%u)", cf.addr, cf.count);
            else if (cf.inst != CF_NOP && cf.inst != CF_RETURN) appendf(out, ": ADDR(%u)", cf.addr);
            if (cf.pop_count) appendf(out, " POP_CNT(%u)", cf.pop_count);
            if (cf.cond) appendf(out, " COND(%u)", cf.cond);
            if (cf.cf_const) appendf(out, " CONST(%u)", cf.cf_const);
        }
        if (cf.valid_pixel_mode) out += " VPM";
        if (cf.whole_quad_mode) out += " WQM";
        if (!cf.barrier) out += " NO_BARRIER";
        if (cf.end_of_program) out += " END_OF_PROGRAM";
        out += '\n';
        for (const AluGroup& g : cf.alu) {
            bool first = true;
            for (const AluInst& a : g.insts) {
                appendf(out, first ? "    %3u " : "        ", g.slot);
                first = false;
                appendf(out, "%c: %s", "xyzwt"[a.unit], a.name() ? a.name() : "???");
                static const char* kOmod[] = {"", "*2", "*4", "/2"};
                out += kOmod[a.omod];
                if (a.clamp) out += "_sat";
                out += ' ';
                if (a.write || a.op3) appendf(out, "R%u%s.%c", a.dst_gpr, a.dst_rel ? "[AR]" : "", kChan[a.dst_chan]);
                else out += "____";
                for (int i = 0; i < a.num_src(); ++i) out += ", " + src_text(a.src[i], g, cf);
                if (a.update_exec_mask) out += " UPDATE_EXEC_MASK";
                if (a.update_pred) out += " UPDATE_PRED";
                if (a.pred_sel) appendf(out, " PRED_SEL_%s", a.pred_sel == 2 ? "ZERO" : a.pred_sel == 3 ? "ONE" : "?");
                if (a.index_mode) appendf(out, " IDX%u", a.index_mode);
                out += '\n';
            }
        }
        for (const TexFetch& t : cf.tex) {
            appendf(out, "        %s R%u.%c%c%c%c, R%u.%c%c%c%c, t%u, s%u", t.name(), t.dst_gpr, kSel[t.dst_sel[0]],
                    kSel[t.dst_sel[1]], kSel[t.dst_sel[2]], kSel[t.dst_sel[3]], t.src_gpr, kSel[t.src_sel[0]],
                    kSel[t.src_sel[1]], kSel[t.src_sel[2]], kSel[t.src_sel[3]], t.resource_id, t.sampler_id);
            if (t.lod_bias) appendf(out, " LOD_BIAS(%d)", t.lod_bias);
            if (t.offset[0] || t.offset[1] || t.offset[2]) appendf(out, " OFFSET(%d,%d,%d)", t.offset[0], t.offset[1], t.offset[2]);
            if (!(t.coord_normalized[0] && t.coord_normalized[1])) out += " UNNORM";
            out += '\n';
        }
        for (const VtxFetch& v : cf.vtx) {
            if (v.inst == TEX_VTX_SEMANTIC)
                appendf(out, "        VTX_SEMANTIC SEM%u", v.semantic_id);
            else
                appendf(out, "        VTX_FETCH R%u.%c%c%c%c", v.dst_gpr, kSel[v.dst_sel[0]], kSel[v.dst_sel[1]],
                        kSel[v.dst_sel[2]], kSel[v.dst_sel[3]]);
            appendf(out, ", R%u.%c, b%u, fmt %u num %u off %u swap %u%s%s\n", v.src_gpr, kChan[v.src_sel_x], v.buffer_id,
                    v.data_format, v.num_format_all, v.offset, v.endian_swap, v.fetch_type ? " INST" : "",
                    v.mega_fetch ? " MEGA" : "");
        }
    }
    for (const std::string& e : p.errors) out += "ERROR: " + e + '\n';
    return out;
}

}  // namespace mh3u::latte
