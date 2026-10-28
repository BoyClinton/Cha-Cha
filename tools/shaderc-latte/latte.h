// Latte (R600/R700-family) shader microcode: decoder and disassembler.
//
// A program is a list of 64-bit control-flow (CF) instructions; ALU, TEX and VTX clauses live elsewhere in the
// program and are referenced by address. Encodings follow the AMD "R600-Family / R700-Family Instruction Set
// Architecture" documents. Words are little-endian in the shader binaries.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mh3u::latte {

// ---- CF -------------------------------------------------------------------------------------------------------
enum CfInst : uint8_t {  // CF_WORD1.CF_INST (7 bits)
    CF_NOP = 0x00, CF_TEX = 0x01, CF_VTX = 0x02, CF_VTX_TC = 0x03,
    CF_LOOP_START = 0x04, CF_LOOP_END = 0x05, CF_LOOP_START_DX10 = 0x06, CF_LOOP_START_NO_AL = 0x07,
    CF_LOOP_CONTINUE = 0x08, CF_LOOP_BREAK = 0x09, CF_JUMP = 0x0A, CF_PUSH = 0x0B, CF_PUSH_ELSE = 0x0C,
    CF_ELSE = 0x0D, CF_POP = 0x0E, CF_POP_JUMP = 0x0F, CF_POP_PUSH = 0x10, CF_POP_PUSH_ELSE = 0x11,
    CF_CALL = 0x12, CF_CALL_FS = 0x13, CF_RETURN = 0x14, CF_EMIT_VERTEX = 0x15, CF_EMIT_CUT_VERTEX = 0x16,
    CF_CUT_VERTEX = 0x17, CF_KILL = 0x18, CF_WAIT_ACK = 0x1A, CF_TEX_ACK = 0x1B, CF_VTX_ACK = 0x1C,
    CF_VTX_TC_ACK = 0x1D,
    // export / memory (CF_ALLOC_EXPORT encodings)
    CF_MEM_STREAM0 = 0x20, CF_MEM_STREAM1 = 0x21, CF_MEM_STREAM2 = 0x22, CF_MEM_STREAM3 = 0x23,
    CF_MEM_SCRATCH = 0x24, CF_MEM_REDUCTION = 0x25, CF_MEM_RING = 0x26, CF_EXPORT = 0x27, CF_EXPORT_DONE = 0x28,
    // ALU clause starts (CF_ALU_WORD1.CF_INST, 4 bits, stored here + 0x40)
    CF_ALU = 0x48, CF_ALU_PUSH_BEFORE = 0x49, CF_ALU_POP_AFTER = 0x4A, CF_ALU_POP2_AFTER = 0x4B,
    CF_ALU_CONTINUE = 0x4D, CF_ALU_BREAK = 0x4E, CF_ALU_ELSE_AFTER = 0x4F,
};

enum class CfKind { Normal, Alu, Export };

enum ExportType : uint8_t { EXPORT_PIXEL = 0, EXPORT_POS = 1, EXPORT_PARAM = 2 };

// ---- ALU ------------------------------------------------------------------------------------------------------
enum AluSel : uint16_t {
    SEL_GPR_LAST = 127,
    SEL_KCACHE0 = 128,  // ..159
    SEL_KCACHE1 = 160,  // ..191
    SEL_0 = 248, SEL_1 = 249, SEL_1_INT = 250, SEL_M_1_INT = 251, SEL_0_5 = 252, SEL_LITERAL = 253,
    SEL_PV = 254, SEL_PS = 255,
    SEL_CFILE = 256,  // ..511: uniform register file c[0..255]
};

enum AluOp2 : uint16_t {
    OP2_ADD = 0x00, OP2_MUL = 0x01, OP2_MUL_IEEE = 0x02, OP2_MAX = 0x03, OP2_MIN = 0x04, OP2_MAX_DX10 = 0x05,
    OP2_MIN_DX10 = 0x06, OP2_SETE = 0x08, OP2_SETGT = 0x09, OP2_SETGE = 0x0A, OP2_SETNE = 0x0B,
    OP2_SETE_DX10 = 0x0C, OP2_SETGT_DX10 = 0x0D, OP2_SETGE_DX10 = 0x0E, OP2_SETNE_DX10 = 0x0F,
    OP2_FRACT = 0x10, OP2_TRUNC = 0x11, OP2_CEIL = 0x12, OP2_RNDNE = 0x13, OP2_FLOOR = 0x14, OP2_MOVA = 0x15,
    OP2_MOVA_FLOOR = 0x16, OP2_MOVA_INT = 0x18, OP2_MOV = 0x19, OP2_NOP = 0x1A,
    OP2_PRED_SETGT_UINT = 0x1E, OP2_PRED_SETGE_UINT = 0x1F,
    OP2_PRED_SETE = 0x20, OP2_PRED_SETGT = 0x21, OP2_PRED_SETGE = 0x22, OP2_PRED_SETNE = 0x23,
    OP2_PRED_SET_INV = 0x24, OP2_PRED_SET_POP = 0x25, OP2_PRED_SET_CLR = 0x26, OP2_PRED_SET_RESTORE = 0x27,
    OP2_PRED_SETE_PUSH = 0x28, OP2_PRED_SETGT_PUSH = 0x29, OP2_PRED_SETGE_PUSH = 0x2A, OP2_PRED_SETNE_PUSH = 0x2B,
    OP2_KILLE = 0x2C, OP2_KILLGT = 0x2D, OP2_KILLGE = 0x2E, OP2_KILLNE = 0x2F,
    OP2_AND_INT = 0x30, OP2_OR_INT = 0x31, OP2_XOR_INT = 0x32, OP2_NOT_INT = 0x33, OP2_ADD_INT = 0x34,
    OP2_SUB_INT = 0x35, OP2_MAX_INT = 0x36, OP2_MIN_INT = 0x37, OP2_MAX_UINT = 0x38, OP2_MIN_UINT = 0x39,
    OP2_SETE_INT = 0x3A, OP2_SETGT_INT = 0x3B, OP2_SETGE_INT = 0x3C, OP2_SETNE_INT = 0x3D,
    OP2_SETGT_UINT = 0x3E, OP2_SETGE_UINT = 0x3F, OP2_KILLGT_UINT = 0x40, OP2_KILLGE_UINT = 0x41,
    OP2_PRED_SETE_INT = 0x42, OP2_PRED_SETGT_INT = 0x43, OP2_PRED_SETGE_INT = 0x44, OP2_PRED_SETNE_INT = 0x45,
    OP2_KILLE_INT = 0x46, OP2_KILLGT_INT = 0x47, OP2_KILLGE_INT = 0x48, OP2_KILLNE_INT = 0x49,
    OP2_PRED_SETE_PUSH_INT = 0x4A, OP2_PRED_SETGT_PUSH_INT = 0x4B, OP2_PRED_SETGE_PUSH_INT = 0x4C,
    OP2_PRED_SETNE_PUSH_INT = 0x4D, OP2_PRED_SETLT_PUSH_INT = 0x4E, OP2_PRED_SETLE_PUSH_INT = 0x4F,
    OP2_DOT4 = 0x50, OP2_DOT4_IEEE = 0x51, OP2_CUBE = 0x52, OP2_MAX4 = 0x53, OP2_MOVA_GPR_INT = 0x60,
    OP2_EXP_IEEE = 0x61, OP2_LOG_CLAMPED = 0x62, OP2_LOG_IEEE = 0x63, OP2_RECIP_CLAMPED = 0x64,
    OP2_RECIP_FF = 0x65, OP2_RECIP_IEEE = 0x66, OP2_RECIPSQRT_CLAMPED = 0x67, OP2_RECIPSQRT_FF = 0x68,
    OP2_RECIPSQRT_IEEE = 0x69, OP2_SQRT_IEEE = 0x6A, OP2_FLT_TO_INT = 0x6B, OP2_INT_TO_FLT = 0x6C,
    OP2_UINT_TO_FLT = 0x6D, OP2_SIN = 0x6E, OP2_COS = 0x6F, OP2_ASHR_INT = 0x70, OP2_LSHR_INT = 0x71,
    OP2_LSHL_INT = 0x72, OP2_MULLO_INT = 0x73, OP2_MULHI_INT = 0x74, OP2_MULLO_UINT = 0x75,
    OP2_MULHI_UINT = 0x76, OP2_RECIP_INT = 0x77, OP2_RECIP_UINT = 0x78, OP2_FLT_TO_UINT = 0x79,
};

enum AluOp3 : uint16_t {
    OP3_MUL_LIT = 0x0C, OP3_MUL_LIT_M2 = 0x0D, OP3_MUL_LIT_M4 = 0x0E, OP3_MUL_LIT_D2 = 0x0F,
    OP3_MULADD = 0x10, OP3_MULADD_M2 = 0x11, OP3_MULADD_M4 = 0x12, OP3_MULADD_D2 = 0x13,
    OP3_MULADD_IEEE = 0x14, OP3_MULADD_IEEE_M2 = 0x15, OP3_MULADD_IEEE_M4 = 0x16, OP3_MULADD_IEEE_D2 = 0x17,
    OP3_CNDE = 0x18, OP3_CNDGT = 0x19, OP3_CNDGE = 0x1A, OP3_CNDE_INT = 0x1C, OP3_CNDGT_INT = 0x1D,
    OP3_CNDGE_INT = 0x1E,
};

struct AluSrc {
    uint16_t sel = 0;
    uint8_t chan = 0;
    bool rel = false, neg = false, abs = false;
};

struct AluInst {
    bool op3 = false;
    uint16_t op = 0;
    AluSrc src[3];
    uint8_t dst_gpr = 0, dst_chan = 0, omod = 0, index_mode = 0, pred_sel = 0, bank_swizzle = 0;
    bool dst_rel = false, write = false, clamp = false, last = false, update_exec_mask = false, update_pred = false;
    uint8_t unit = 0;  // 0..3 = x,y,z,w vector slot, 4 = t (trans)
    int num_src() const;
    const char* name() const;
};

struct AluGroup {
    std::vector<AluInst> insts;
    uint32_t literals[4] = {};
    uint32_t slot = 0;  // index of the group's first 64-bit slot in the clause
};

// ---- TEX / VTX fetch ------------------------------------------------------------------------------------------
enum TexInst : uint8_t {
    TEX_VTX_FETCH = 0, TEX_VTX_SEMANTIC = 1, TEX_LD = 3, TEX_GET_TEXTURE_INFO = 4, TEX_GET_NUMBER_OF_SAMPLES = 5,
    TEX_GET_COMP_TEX_LOD = 6, TEX_GET_GRADIENTS_H = 7, TEX_GET_GRADIENTS_V = 8, TEX_GET_LERP = 9,
    TEX_KEEP_GRADIENTS = 10, TEX_SET_GRADIENTS_H = 11, TEX_SET_GRADIENTS_V = 12, TEX_PASS = 13,
    TEX_SET_CUBEMAP_INDEX = 14, TEX_FETCH4 = 15, TEX_SAMPLE = 16, TEX_SAMPLE_L = 17, TEX_SAMPLE_LB = 18,
    TEX_SAMPLE_LZ = 19, TEX_SAMPLE_G = 20, TEX_SAMPLE_G_L = 21, TEX_SAMPLE_G_LB = 22, TEX_SAMPLE_G_LZ = 23,
    TEX_SAMPLE_C = 24, TEX_SAMPLE_C_L = 25, TEX_SAMPLE_C_LB = 26, TEX_SAMPLE_C_LZ = 27, TEX_SAMPLE_C_G = 28,
    TEX_SAMPLE_C_G_L = 29, TEX_SAMPLE_C_G_LB = 30, TEX_SAMPLE_C_G_LZ = 31,
};

struct TexFetch {
    uint8_t inst = 0, resource_id = 0, sampler_id = 0, src_gpr = 0, dst_gpr = 0;
    bool src_rel = false, dst_rel = false, fetch_whole_quad = false, bc_frac_mode = false, alt_const = false;
    uint8_t dst_sel[4] = {}, src_sel[4] = {};
    bool coord_normalized[4] = {};
    int8_t lod_bias = 0;  // signed 7-bit, 1/2-units? (raw field)
    int8_t offset[3] = {};  // signed 5-bit, raw field
    const char* name() const;
};

struct VtxFetch {
    uint8_t inst = 0, fetch_type = 0, buffer_id = 0, src_gpr = 0, src_sel_x = 0, mega_fetch_count = 0;
    bool src_rel = false, fetch_whole_quad = false;
    // word1
    uint8_t dst_gpr = 0, semantic_id = 0;
    bool dst_rel = false;
    uint8_t dst_sel[4] = {}, data_format = 0, num_format_all = 0;
    bool use_const_fields = false, format_comp_all = false, srf_mode_all = false;
    // word2
    uint16_t offset = 0;
    uint8_t endian_swap = 0;
    bool const_buf_no_stride = false, mega_fetch = false;
};

// ---- CF instruction with its clause decoded -------------------------------------------------------------------
struct KCache {
    uint8_t bank = 0, mode = 0;  // mode 0 none, 1 lock 16 constants, 2 lock 32, 3 lock loop index
    uint8_t addr = 0;            // in units of 16 constants
};

struct Cf {
    uint32_t index = 0;
    uint32_t w0 = 0, w1 = 0;
    CfKind kind = CfKind::Normal;
    uint8_t inst = 0;
    bool barrier = false, whole_quad_mode = false, end_of_program = false, valid_pixel_mode = false;
    // normal
    uint32_t addr = 0;
    uint8_t pop_count = 0, cf_const = 0, cond = 0, call_count = 0;
    uint32_t count = 0;  // clause length (instructions / ALU slots), already +1
    // ALU
    KCache kcache[2];
    bool alt_const = false;
    // export
    uint8_t export_type = 0, rw_gpr = 0, index_gpr = 0, elem_size = 0, burst_count = 0, comp_mask = 0;
    uint16_t array_base = 0, array_size = 0;
    bool rw_rel = false;
    uint8_t sel[4] = {};
    // decoded clause body
    std::vector<AluGroup> alu;
    std::vector<TexFetch> tex;
    std::vector<VtxFetch> vtx;
    const char* name() const;
};

struct Program {
    std::vector<Cf> cf;
    std::vector<std::string> errors;
    bool ok() const { return errors.empty(); }
};

// Decodes a whole program (CF list until END_OF_PROGRAM, plus all referenced clauses).
Program decode(const uint8_t* data, size_t size);

// Text listing in the style of the AMD docs / Cemu's shader dumps.
std::string disassemble(const Program& p);

bool is_trans_only(uint16_t op2);
bool is_reduction(const AluInst& i);  // DOT4, DOT4_IEEE, CUBE, MAX4: occupy all four vector slots

}  // namespace mh3u::latte
