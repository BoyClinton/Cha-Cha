#include "glsl.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

#include "shader_abi.h"

namespace mh3u::latte {
namespace abi = mh3u::gfx::shader_abi;

// Register-block word indices (gx2/shaders.h)
namespace vsreg {
constexpr int kNumOutId = 3, kOutId = 4, kNumSemantic = 16, kSemantic = 17;
}
namespace psreg {
constexpr int kInControl0 = 2, kInControl1 = 3, kNumInput = 4, kInput = 5;
}
namespace gsreg {
constexpr int kOutPrimType = 1, kVertItemSize = 5, kNumOutId = 7, kOutId = 8;
}

std::vector<uint32_t> vs_param_semantics(const Shader& s) {
    std::vector<uint32_t> out;
    if (s.stage == Stage::Pixel) return out;
    // A GS's copy shader exports the params, with the GS header's spi_vs_out_id.
    const int num = s.stage == Stage::Vertex ? vsreg::kNumOutId : gsreg::kNumOutId;
    const int ids = s.stage == Stage::Vertex ? vsreg::kOutId : gsreg::kOutId;
    const uint32_t n = std::min<uint32_t>(s.regs[num], 10);
    for (uint32_t j = 0; j < n * 4; ++j) out.push_back((s.regs[ids + j / 4] >> (8 * (j % 4))) & 0xFF);
    return out;
}

std::vector<uint32_t> ps_input_semantics(const Shader& s) {
    std::vector<uint32_t> out;
    if (s.stage != Stage::Pixel) return out;
    const uint32_t n = std::min<uint32_t>(s.regs[psreg::kNumInput], 32);
    for (uint32_t i = 0; i < n; ++i) out.push_back(s.regs[psreg::kInput + i] & 0xFF);
    // POSITION_ENA: the window position takes GPR POSITION_ADDR, whose input slot is a placeholder.
    const uint32_t c0 = s.regs[psreg::kInControl0];
    if (c0 & (1u << 8) && (c0 >> 10 & 0x1F) < out.size()) out[c0 >> 10 & 0x1F] = 0xFF;
    return out;
}

namespace {

const char kChan[] = "xyzw";

std::string fmt(const char* f, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

// Exact GLSL spelling of a float bit pattern.
std::string float_lit(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, 4);
    if (!std::isfinite(f) || (f != 0.0f && std::fabs(f) < 1.17549435e-38f)) return fmt("intBitsToFloat(%d)", static_cast<int32_t>(bits));
    std::string s = fmt("%.9g", f);
    if (s.find_first_of(".e") == std::string::npos) s += ".0";
    return s;
}

std::string int_lit(uint32_t bits) {
    const int32_t v = static_cast<int32_t>(bits);
    if (v == INT32_MIN) return "(-2147483647 - 1)";
    return v < -9 || v > 9 ? fmt("0x%X", bits) : std::to_string(v);
}

const char* kHelpers = R"(// Latte MUL / MULADD / DOT4 (non-IEEE): 0 * anything = 0
float mul_l(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }
float dot4_l(vec4 a, vec4 b) {
    vec4 p = mix(a * b, vec4(0.0), bvec4(uvec4(equal(a, vec4(0.0))) | uvec4(equal(b, vec4(0.0)))));
    return p.x + p.y + p.z + p.w;
}
// CUBE: (tc, sc, 2 * major axis, face) of a direction, and the direction back from (s, t, face), s/t in [1, 2]
vec4 cube_op(vec3 v) {
    vec3 a = abs(v);
    if (a.z >= a.x && a.z >= a.y) return vec4(-v.y, v.z < 0.0 ? -v.x : v.x, 2.0 * v.z, v.z < 0.0 ? 5.0 : 4.0);
    if (a.y >= a.x) return vec4(v.y < 0.0 ? -v.z : v.z, v.x, 2.0 * v.y, v.y < 0.0 ? 3.0 : 2.0);
    return vec4(-v.y, v.x < 0.0 ? v.z : -v.z, 2.0 * v.x, v.x < 0.0 ? 1.0 : 0.0);
}
vec3 cube_dir(vec3 c) {
    float u = (c.x - 1.5) * 2.0, v = (c.y - 1.5) * 2.0;
    switch (int(c.z + 0.5)) {
        case 0: return vec3(1.0, -v, -u);
        case 1: return vec3(-1.0, -v, u);
        case 2: return vec3(u, 1.0, v);
        case 3: return vec3(u, -1.0, -v);
        case 4: return vec3(u, -v, 1.0);
        default: return vec3(-u, -v, -1.0);
    }
}
)";

const char* kAlphaTest = R"(layout(push_constant) uniform PushConstants { uint alpha_func; float alpha_ref; } pc;
bool alpha_pass(float a) {
    switch (pc.alpha_func) {
        case 0u: return false;
        case 1u: return a < pc.alpha_ref;
        case 2u: return a == pc.alpha_ref;
        case 3u: return a <= pc.alpha_ref;
        case 4u: return a > pc.alpha_ref;
        case 5u: return a != pc.alpha_ref;
        case 6u: return a >= pc.alpha_ref;
        default: return true;
    }
}
)";

const char* kVertexPushConstants = "layout(push_constant) uniform PushConstants { layout(offset = 16) float point_size; } pc;\n";

class Emitter {
public:
    Emitter(const Shader& s, const Program& p, const VaryingTable& v) : sh_(s), p_(p), vary_(v) {}

    GlslResult run() {
        compute_levels();
        if (errors_.empty()) body();
        if (errors_.empty() && sh_.stage == Stage::Geometry) copy_text_ = copy_function();
        GlslResult r;
        r.errors = errors_;
        if (errors_.empty()) r.source = header() + main_text();
        if (errors_.empty()) r.meta = meta();
        return r;
    }

    std::string meta() const {
        std::string s;
        for (const auto& [b, n] : ubo_vec4s_) s += fmt("ubo %d %u\n", b, n);
        for (const auto& [slot, type] : samplers_) s += fmt("sampler %d %u\n", slot, type);
        for (uint32_t a : vs_inputs_) s += fmt("input %u\n", a);
        return s;
    }

private:
    const Shader& sh_;
    const Program& p_;
    const VaryingTable& vary_;
    std::vector<std::string> errors_;
    std::string out_;
    int indent_ = 1;
    std::vector<int> level_at_;  // stack depth before each CF instruction
    int max_level_ = 0;
    std::set<int> gprs_;
    std::map<int, uint32_t> ubo_vec4s_;  // uniform block -> vec4 count referenced
    std::map<int, uint32_t> samplers_;   // slot -> sampler var type
    std::set<uint32_t> vs_inputs_;       // attribute semantics
    std::map<uint32_t, bool> outputs_;   // pixel: colour target -> written
    bool depth_export_ = false, uses_alpha_test_ = false;
    int loop_counter_ = 0;
    static constexpr uint32_t kRingBuffer = 159;  // buffer id of the ES->GS / GS->VS rings
    std::set<uint32_t> gs_inputs_, es_outputs_;   // ring elements (vec4 index) read by the GS / written by the VS
    uint32_t ring_size_ = 0, copy_reads_ = 0, emits_ = 0;
    std::string copy_text_;

    void error(const std::string& e) {
        if (errors_.size() < 8) errors_.push_back(e);
    }
    void line(const std::string& s) {
        out_.append(size_t(indent_) * 4, ' ');
        out_ += s;
        out_ += '\n';
    }
    std::string m(int level) const { return "m" + std::to_string(level); }
    std::string gpr(int r) {
        gprs_.insert(r);
        return "R" + std::to_string(r);
    }

    // ---- static stack depth, and the check that dropping JUMPs is sound ---------------------------------------
    void compute_levels() {
        const size_t n = p_.cf.size();
        level_at_.assign(n + 1, 0);
        std::vector<int> loop_of(n + 1, 0);  // innermost loop id at each CF
        std::vector<int> loops;
        int level = 0, next_loop = 1;
        for (size_t i = 0; i < n; ++i) {
            const Cf& cf = p_.cf[i];
            level_at_[i] = level;
            loop_of[i] = loops.empty() ? 0 : loops.back();
            switch (cf.inst) {
                case CF_ALU_PUSH_BEFORE: case CF_PUSH: case CF_LOOP_START_DX10: ++level; break;
                case CF_ALU_POP_AFTER: --level; break;
                case CF_ALU_POP2_AFTER: level -= 2; break;
                case CF_POP: level -= cf.pop_count; break;
                default: break;
            }
            if (cf.inst == CF_LOOP_START_DX10) loops.push_back(next_loop++);
            if (cf.inst == CF_LOOP_END) {
                if (loops.empty()) error(fmt("CF %zu: LOOP_END without a loop", i));
                else loops.pop_back();
                --level;
            }
            if (level < 0) error(fmt("CF %zu: stack underflow", i));
            max_level_ = std::max(max_level_, level);
        }
        level_at_[n] = level;
        for (size_t i = 0; i < n; ++i) {
            const Cf& cf = p_.cf[i];
            if (cf.inst != CF_JUMP && cf.inst != CF_ELSE) continue;
            if (cf.addr > n) {
                error(fmt("CF %zu: jump target %u outside the program", i, cf.addr));
                continue;
            }
            if (cf.addr <= i) error(fmt("CF %zu: backward jump to %u", i, cf.addr));
            const int want = level_at_[i] - cf.pop_count;
            if (level_at_[cf.addr] != want)
                error(fmt("CF %zu: %s to %u expects stack depth %d, sequential path has %d", i, cf.name(), cf.addr, want,
                          level_at_[cf.addr]));
            if (loop_of[i] != loop_of[cf.addr]) error(fmt("CF %zu: jump crosses a loop boundary", i));
        }
        if (level != 0 && errors_.empty()) error(fmt("program ends at stack depth %d", level));
    }

    // ---- operands ---------------------------------------------------------------------------------------------
    std::string kcache(const Cf& cf, int k, int idx) {
        const KCache& kc = cf.kcache[k];
        if (!kc.mode) error(fmt("CF %u: KC%d read without a locked bank", cf.index, k));
        if (kc.mode == 3) error(fmt("CF %u: KCACHE loop-index mode", cf.index));
        const uint32_t v = kc.addr * 16u + idx;
        uint32_t& cnt = ubo_vec4s_[kc.bank];
        cnt = std::max(cnt, v + 1);
        return fmt("ub%u.d[%u]", kc.bank, v);
    }

    // Raw 32-bit value of a source (ignoring neg/abs).
    std::string src_bits(const AluInst& a, int i, const AluGroup& g, const Cf& cf) {
        const AluSrc& s = a.src[i];
        if (s.rel) error(fmt("CF %u: relative source addressing", cf.index));
        const char c = kChan[s.chan];
        if (s.sel <= SEL_GPR_LAST) return gpr(s.sel) + "." + c;
        if (s.sel < SEL_KCACHE1) return kcache(cf, 0, s.sel - SEL_KCACHE0) + "." + c;
        if (s.sel < SEL_KCACHE1 + 32) return kcache(cf, 1, s.sel - SEL_KCACHE1) + "." + c;
        switch (s.sel) {
            case SEL_0: return "0";
            case SEL_1: return "0x3F800000";
            case SEL_1_INT: return "1";
            case SEL_M_1_INT: return "-1";
            case SEL_0_5: return "0x3F000000";
            case SEL_LITERAL: return int_lit(g.literals[s.chan]);
            case SEL_PV: return std::string("PV.") + c;
            case SEL_PS: return "PS";
            default: break;
        }
        error(fmt("CF %u: unsupported source select %u", cf.index, s.sel));
        return "0";
    }

    std::string src_f(const AluInst& a, int i, const AluGroup& g, const Cf& cf) {
        const AluSrc& s = a.src[i];
        std::string v;
        switch (s.sel) {
            case SEL_0: v = "0.0"; break;
            case SEL_1: v = "1.0"; break;
            case SEL_0_5: v = "0.5"; break;
            case SEL_LITERAL: v = float_lit(g.literals[s.chan]); break;
            default: v = "intBitsToFloat(" + src_bits(a, i, g, cf) + ")"; break;
        }
        if (s.abs) v = "abs(" + v + ")";
        if (s.neg) v = "-" + v;
        return v;
    }

    std::string src_i(const AluInst& a, int i, const AluGroup& g, const Cf& cf) {
        if (a.src[i].neg || a.src[i].abs) error(fmt("CF %u: neg/abs on an integer operand of %s", cf.index, a.name()));
        return src_bits(a, i, g, cf);
    }

    // Float result -> bits, with output modifier and clamp.
    std::string fres(const AluInst& a, std::string v) {
        static const char* kOmod[] = {"", " * 2.0", " * 4.0", " * 0.5"};
        if (a.omod) v = "(" + v + ")" + kOmod[a.omod];
        if (a.clamp) v = "clamp(" + v + ", 0.0, 1.0)";
        return "floatBitsToInt(" + v + ")";
    }
    std::string ires(const AluInst& a, const Cf& cf, std::string v) {
        if (a.omod || a.clamp) error(fmt("CF %u: omod/clamp on integer %s", cf.index, a.name()));
        return v;
    }

    bool modified(const AluInst& a, int i) const { return a.src[i].neg || a.src[i].abs; }

    // ---- ALU --------------------------------------------------------------------------------------------------
    // Returns the instruction's 32-bit result expression; side effects (kill, exec mask) go to `pre`.
    std::string alu_value(const AluInst& a, const AluGroup& g, const Cf& cf, std::vector<std::string>& pre, bool& sets_exec) {
        const auto F = [&](int i) { return src_f(a, i, g, cf); };
        const auto I = [&](int i) { return src_i(a, i, g, cf); };
        const auto U = [&](int i) { return "uint(" + src_i(a, i, g, cf) + ")"; };
        const auto cmp_f = [&](const char* op) { return "(" + F(0) + " " + op + " " + F(1) + ")"; };
        const auto cmp_i = [&](const char* op) { return "(" + I(0) + " " + op + " " + I(1) + ")"; };
        const auto cmp_u = [&](const char* op) { return "(" + U(0) + " " + op + " " + U(1) + ")"; };
        const auto exec = [&](const std::string& cond) {
            if (a.update_exec_mask) {
                pre.push_back("exec_next = " + cond + ";");
                sets_exec = true;
            }
            return fres(a, cond + " ? 0.0 : 1.0");
        };
        const auto kill = [&](const std::string& cond) {
            if (sh_.stage != Stage::Pixel) error(fmt("CF %u: KILL outside a pixel shader", cf.index));
            pre.push_back("if " + cond + " demote;");
            return fres(a, cond + " ? 1.0 : 0.0");
        };
        if (a.op3) {
            const auto cnd = [&](const std::string& cond) {
                if (a.omod || a.clamp || modified(a, 1) || modified(a, 2)) return fres(a, cond + " ? " + F(1) + " : " + F(2));
                return cond + " ? " + src_bits(a, 1, g, cf) + " : " + src_bits(a, 2, g, cf);
            };
            switch (a.op) {
                case OP3_MULADD: return fres(a, "mul_l(" + F(0) + ", " + F(1) + ") + " + F(2));
                case OP3_MULADD_M2: return fres(a, "(mul_l(" + F(0) + ", " + F(1) + ") + " + F(2) + ") * 2.0");
                case OP3_MULADD_M4: return fres(a, "(mul_l(" + F(0) + ", " + F(1) + ") + " + F(2) + ") * 4.0");
                case OP3_MULADD_D2: return fres(a, "(mul_l(" + F(0) + ", " + F(1) + ") + " + F(2) + ") * 0.5");
                case OP3_MULADD_IEEE: return fres(a, F(0) + " * " + F(1) + " + " + F(2));
                case OP3_MULADD_IEEE_M2: return fres(a, "(" + F(0) + " * " + F(1) + " + " + F(2) + ") * 2.0");
                case OP3_MULADD_IEEE_M4: return fres(a, "(" + F(0) + " * " + F(1) + " + " + F(2) + ") * 4.0");
                case OP3_MULADD_IEEE_D2: return fres(a, "(" + F(0) + " * " + F(1) + " + " + F(2) + ") * 0.5");
                case OP3_CNDE: return cnd("(" + F(0) + " == 0.0)");
                case OP3_CNDGT: return cnd("(" + F(0) + " > 0.0)");
                case OP3_CNDGE: return cnd("(" + F(0) + " >= 0.0)");
                case OP3_CNDE_INT: return cnd("(" + I(0) + " == 0)");
                case OP3_CNDGT_INT: return cnd("(" + I(0) + " > 0)");
                case OP3_CNDGE_INT: return cnd("(" + I(0) + " >= 0)");
                default: break;
            }
            error(fmt("CF %u: unsupported OP3 %s", cf.index, a.name()));
            return "0";
        }
        switch (a.op) {
            case OP2_ADD: return fres(a, F(0) + " + " + F(1));
            case OP2_MUL: return fres(a, "mul_l(" + F(0) + ", " + F(1) + ")");
            case OP2_MUL_IEEE: return fres(a, F(0) + " * " + F(1));
            case OP2_MAX: case OP2_MAX_DX10: return fres(a, "max(" + F(0) + ", " + F(1) + ")");
            case OP2_MIN: case OP2_MIN_DX10: return fres(a, "min(" + F(0) + ", " + F(1) + ")");
            case OP2_SETE: return fres(a, cmp_f("==") + " ? 1.0 : 0.0");
            case OP2_SETGT: return fres(a, cmp_f(">") + " ? 1.0 : 0.0");
            case OP2_SETGE: return fres(a, cmp_f(">=") + " ? 1.0 : 0.0");
            case OP2_SETNE: return fres(a, cmp_f("!=") + " ? 1.0 : 0.0");
            case OP2_SETE_DX10: return ires(a, cf, cmp_f("==") + " ? -1 : 0");
            case OP2_SETGT_DX10: return ires(a, cf, cmp_f(">") + " ? -1 : 0");
            case OP2_SETGE_DX10: return ires(a, cf, cmp_f(">=") + " ? -1 : 0");
            case OP2_SETNE_DX10: return ires(a, cf, cmp_f("!=") + " ? -1 : 0");
            case OP2_FRACT: return fres(a, "fract(" + F(0) + ")");
            case OP2_TRUNC: return fres(a, "trunc(" + F(0) + ")");
            case OP2_CEIL: return fres(a, "ceil(" + F(0) + ")");
            case OP2_RNDNE: return fres(a, "roundEven(" + F(0) + ")");
            case OP2_FLOOR: return fres(a, "floor(" + F(0) + ")");
            case OP2_MOV:
                if (!a.omod && !a.clamp && !modified(a, 0)) return src_bits(a, 0, g, cf);
                return fres(a, F(0));
            case OP2_PRED_SETE: return exec(cmp_f("=="));
            case OP2_PRED_SETGT: return exec(cmp_f(">"));
            case OP2_PRED_SETGE: return exec(cmp_f(">="));
            case OP2_PRED_SETNE: return exec(cmp_f("!="));
            case OP2_PRED_SETE_INT: return exec(cmp_i("=="));
            case OP2_PRED_SETGT_INT: return exec(cmp_i(">"));
            case OP2_PRED_SETGE_INT: return exec(cmp_i(">="));
            case OP2_PRED_SETNE_INT: return exec(cmp_i("!="));
            case OP2_PRED_SETGT_UINT: return exec(cmp_u(">"));
            case OP2_PRED_SETGE_UINT: return exec(cmp_u(">="));
            case OP2_KILLE: return kill(cmp_f("=="));
            case OP2_KILLGT: return kill(cmp_f(">"));
            case OP2_KILLGE: return kill(cmp_f(">="));
            case OP2_KILLNE: return kill(cmp_f("!="));
            case OP2_KILLE_INT: return kill(cmp_i("=="));
            case OP2_KILLGT_INT: return kill(cmp_i(">"));
            case OP2_KILLGE_INT: return kill(cmp_i(">="));
            case OP2_KILLNE_INT: return kill(cmp_i("!="));
            case OP2_KILLGT_UINT: return kill(cmp_u(">"));
            case OP2_KILLGE_UINT: return kill(cmp_u(">="));
            case OP2_AND_INT: return ires(a, cf, I(0) + " & " + I(1));
            case OP2_OR_INT: return ires(a, cf, I(0) + " | " + I(1));
            case OP2_XOR_INT: return ires(a, cf, I(0) + " ^ " + I(1));
            case OP2_NOT_INT: return ires(a, cf, "~" + I(0));
            case OP2_ADD_INT: return ires(a, cf, I(0) + " + " + I(1));
            case OP2_SUB_INT: return ires(a, cf, I(0) + " - " + I(1));
            case OP2_MAX_INT: return ires(a, cf, "max(" + I(0) + ", " + I(1) + ")");
            case OP2_MIN_INT: return ires(a, cf, "min(" + I(0) + ", " + I(1) + ")");
            case OP2_MAX_UINT: return ires(a, cf, "int(max(" + U(0) + ", " + U(1) + "))");
            case OP2_MIN_UINT: return ires(a, cf, "int(min(" + U(0) + ", " + U(1) + "))");
            case OP2_SETE_INT: return ires(a, cf, cmp_i("==") + " ? -1 : 0");
            case OP2_SETGT_INT: return ires(a, cf, cmp_i(">") + " ? -1 : 0");
            case OP2_SETGE_INT: return ires(a, cf, cmp_i(">=") + " ? -1 : 0");
            case OP2_SETNE_INT: return ires(a, cf, cmp_i("!=") + " ? -1 : 0");
            case OP2_SETGT_UINT: return ires(a, cf, cmp_u(">") + " ? -1 : 0");
            case OP2_SETGE_UINT: return ires(a, cf, cmp_u(">=") + " ? -1 : 0");
            case OP2_ASHR_INT: return ires(a, cf, I(0) + " >> (" + I(1) + " & 31)");
            case OP2_LSHR_INT: return ires(a, cf, "int(" + U(0) + " >> uint(" + I(1) + " & 31))");
            case OP2_LSHL_INT: return ires(a, cf, I(0) + " << (" + I(1) + " & 31)");
            case OP2_MULLO_INT: return ires(a, cf, I(0) + " * " + I(1));
            case OP2_MULLO_UINT: return ires(a, cf, "int(" + U(0) + " * " + U(1) + ")");
            case OP2_EXP_IEEE: return fres(a, "exp2(" + F(0) + ")");
            case OP2_LOG_IEEE: return fres(a, "log2(" + F(0) + ")");
            case OP2_RECIP_IEEE: return fres(a, "1.0 / " + F(0));
            case OP2_RECIPSQRT_IEEE: return fres(a, "inversesqrt(" + F(0) + ")");
            case OP2_SQRT_IEEE: return fres(a, "sqrt(" + F(0) + ")");
            case OP2_SIN: return fres(a, "sin(" + F(0) + " * 6.28318548)");  // R700: input in revolutions
            case OP2_COS: return fres(a, "cos(" + F(0) + " * 6.28318548)");
            case OP2_FLT_TO_INT: return ires(a, cf, "int(" + F(0) + ")");
            case OP2_FLT_TO_UINT: return ires(a, cf, "int(uint(" + F(0) + "))");
            case OP2_INT_TO_FLT: return fres(a, "float(" + I(0) + ")");
            case OP2_UINT_TO_FLT: return fres(a, "float(" + U(0) + ")");
            default: break;
        }
        error(fmt("CF %u: unsupported ALU %s", cf.index, a.name()));
        return "0";
    }

    void emit_group(const AluGroup& g, const Cf& cf) {
        std::vector<std::string> pre, vals;
        const AluInst* unit[5] = {};
        for (const AluInst& a : g.insts) unit[a.unit] = &a;
        std::string res[5];
        bool sets_exec = false;
        // Reductions use all four vector slots.
        const AluInst* red = nullptr;
        for (const AluInst& a : g.insts)
            if (is_reduction(a)) red = &a;
        if (red) {
            for (int u = 0; u < 4; ++u)
                if (!unit[u] || unit[u]->op != red->op) error(fmt("CF %u: %s does not fill all vector slots", cf.index, red->name()));
            if (!errors_.empty()) return;
            const auto v4 = [&](int s) {
                return "vec4(" + src_f(*unit[0], s, g, cf) + ", " + src_f(*unit[1], s, g, cf) + ", " + src_f(*unit[2], s, g, cf) +
                       ", " + src_f(*unit[3], s, g, cf) + ")";
            };
            if (red->op == OP2_DOT4) pre.push_back("float red = dot4_l(" + v4(0) + ", " + v4(1) + ");");
            else if (red->op == OP2_DOT4_IEEE) pre.push_back("float red = dot(" + v4(0) + ", " + v4(1) + ");");
            else if (red->op == OP2_CUBE)
                pre.push_back("vec4 red = cube_op(vec3(" + src_f(*unit[2], 0, g, cf) + ", " + src_f(*unit[3], 0, g, cf) + ", " +
                              src_f(*unit[0], 0, g, cf) + "));");
            else error(fmt("CF %u: unsupported reduction %s", cf.index, red->name()));
            for (int u = 0; u < 4; ++u) res[u] = fres(*unit[u], red->op == OP2_CUBE ? fmt("red.%c", kChan[u]) : "red");
        }
        for (const AluInst& a : g.insts) {
            if (red && a.unit < 4) continue;
            if (!a.op3 && a.op == OP2_NOP) continue;
            res[a.unit] = alu_value(a, g, cf, pre, sets_exec);
        }
        line("{");
        ++indent_;
        for (const std::string& s : pre) line(s);
        static const char* kUnit[] = {"rx", "ry", "rz", "rw", "rt"};
        for (int u = 0; u < 5; ++u)
            if (!res[u].empty()) line(fmt("int %s = %s;", kUnit[u], res[u].c_str()));
        for (int u = 0; u < 5; ++u) {
            const AluInst* a = unit[u];
            if (!a || res[u].empty() || !a->write) continue;
            if (a->dst_rel) error(fmt("CF %u: relative destination", cf.index));
            line(fmt("%s.%c = %s;", gpr(a->dst_gpr).c_str(), kChan[a->dst_chan], kUnit[u]));
        }
        for (int u = 0; u < 4; ++u)
            if (!res[u].empty()) line(fmt("PV.%c = %s;", kChan[u], kUnit[u]));
        if (!res[4].empty()) line("PS = rt;");
        --indent_;
        line("}");
        (void)sets_exec;
    }

    void emit_alu_clause(const Cf& cf, int level) {
        bool exec = false;
        for (const AluGroup& g : cf.alu)
            for (const AluInst& a : g.insts) exec |= a.update_exec_mask;
        line(fmt("if (%s) {  // CF %u %s", m(level).c_str(), cf.index, cf.name()));
        ++indent_;
        if (exec) line("bool exec_next = true;");
        for (const AluGroup& g : cf.alu) emit_group(g, cf);
        if (exec) line(m(level) + " = exec_next;");
        --indent_;
        line("}");
    }

    // ---- fetches ----------------------------------------------------------------------------------------------
    std::string coord_comp(const TexFetch& t, int c) {
        switch (t.src_sel[c]) {
            case 4: return "0.0";
            case 5: return "1.0";
            case 7: return "0.0";
            default: return fmt("intBitsToFloat(%s.%c)", gpr(t.src_gpr).c_str(), kChan[t.src_sel[c]]);
        }
    }

    void emit_tex(const TexFetch& t, const Cf& cf) {
        if (t.src_rel || t.dst_rel) error(fmt("CF %u: relative TEX addressing", cf.index));
        if (t.lod_bias || t.offset[0] || t.offset[1] || t.offset[2]) error(fmt("CF %u: TEX lod bias/offset", cf.index));
        if (t.resource_id != t.sampler_id) error(fmt("CF %u: TEX resource %u != sampler %u", cf.index, t.resource_id, t.sampler_id));
        const SamplerVar* var = nullptr;
        for (const SamplerVar& s : sh_.samplers)
            if (s.location == t.sampler_id) var = &s;
        if (!var) {
            error(fmt("CF %u: no sampler var at slot %u", cf.index, t.sampler_id));
            return;
        }
        samplers_[t.sampler_id] = var->type;
        const std::string s = fmt("tex%u", t.sampler_id);
        std::string crd = "vec4(" + coord_comp(t, 0) + ", " + coord_comp(t, 1) + ", " + coord_comp(t, 2) + ", " + coord_comp(t, 3) + ")";
        std::string coord;
        switch (var->type) {
            case 0: coord = "c.x"; break;
            case 1: case 6: coord = "c.xy"; break;
            case 4: coord = "cube_dir(c.xyz)"; break;
            default: error(fmt("CF %u: sampler type %u", cf.index, var->type)); return;
        }
        std::string expr;
        const bool shadow = var->type == 6;
        switch (t.inst) {
            case TEX_SAMPLE: expr = shadow ? "" : "texture(" + s + ", " + coord + ")"; break;
            case TEX_SAMPLE_L: expr = shadow ? "" : "textureLod(" + s + ", " + coord + ", c.w)"; break;
            case TEX_SAMPLE_LB: expr = shadow ? "" : "texture(" + s + ", " + coord + ", c.w)"; break;
            case TEX_SAMPLE_LZ: expr = shadow ? "" : "textureLod(" + s + ", " + coord + ", 0.0)"; break;
            case TEX_SAMPLE_C: expr = shadow ? "vec4(texture(" + s + ", vec3(c.xy, c.w)))" : ""; break;
            case TEX_SAMPLE_C_LZ: expr = shadow ? "vec4(textureLod(" + s + ", vec3(c.xy, c.w), 0.0))" : ""; break;
            default: break;
        }
        if (expr.empty()) {
            error(fmt("CF %u: unsupported %s on sampler type %u", cf.index, t.name(), var->type));
            return;
        }
        line("{");
        ++indent_;
        line("vec4 c = " + crd + ";");
        line("ivec4 f = floatBitsToInt(" + expr + ");");
        for (int c = 0; c < 4; ++c) {
            const uint8_t sel = t.dst_sel[c];
            if (sel == 7) continue;
            const std::string v = sel < 4 ? fmt("f.%c", kChan[sel]) : sel == 5 ? "0x3F800000" : "0";
            line(fmt("%s.%c = %s;", gpr(t.dst_gpr).c_str(), kChan[c], v.c_str()));
        }
        --indent_;
        line("}");
    }

    void emit_vtx(const VtxFetch& v, const Cf& cf) {
        if (v.buffer_id == kRingBuffer && sh_.stage == Stage::Geometry) {
            // ES->GS ring: R0.x holds the ring offset of input vertex 0 (the only one these shaders read).
            if (v.src_gpr != 0 || v.src_sel_x != 0 || v.offset % 16) {
                error(fmt("CF %u: GS ring fetch not relative to vertex 0", cf.index));
                return;
            }
            gs_inputs_.insert(v.offset / 16u);
            write_fetched(v.dst_gpr, v.dst_sel, fmt("floatBitsToInt(es%u[0])", v.offset / 16u));
            return;
        }
        if (v.inst != TEX_VTX_FETCH || v.buffer_id < 0x80 || v.buffer_id >= 0x80 + abi::kMaxUniformBlocks) {
            error(fmt("CF %u: unsupported vertex fetch (inst %u, buffer %u)", cf.index, v.inst, v.buffer_id));
            return;
        }
        if (v.src_rel || v.dst_rel || v.offset % 16) error(fmt("CF %u: unsupported uniform-buffer fetch form", cf.index));
        const uint32_t block = v.buffer_id - 0x80u;
        const auto it = std::find_if(sh_.uniform_blocks.begin(), sh_.uniform_blocks.end(),
                                     [&](const UniformBlock& b) { return b.index == block; });
        uint32_t& cnt = ubo_vec4s_[block];
        cnt = std::max(cnt, it != sh_.uniform_blocks.end() ? (it->size + 15) / 16 : 1u);
        write_fetched(v.dst_gpr, v.dst_sel, fmt("ub%u.d[%s.%c + %u]", block, gpr(v.src_gpr).c_str(), kChan[v.src_sel_x], v.offset / 16));
    }

    void write_fetched(uint32_t dst, const uint8_t* dst_sel, const std::string& value) {
        line("{");
        ++indent_;
        line("ivec4 f = " + value + ";");
        for (int c = 0; c < 4; ++c) {
            const uint8_t sel = dst_sel[c];
            if (sel == 7) continue;
            const std::string val = sel < 4 ? fmt("f.%c", kChan[sel]) : sel == 5 ? "0x3F800000" : "0";
            line(fmt("%s.%c = %s;", gpr(int(dst)).c_str(), kChan[c], val.c_str()));
        }
        --indent_;
        line("}");
    }

    // ---- ring buffers (VS+GS pairs) ---------------------------------------------------------------------------
    // MEM_RING writes vec4 elements at dword offsets: from the VS into the ES->GS ring (-> outputs es<i>, read by
    // the GS as inputs), from the GS into the GS->VS ring (-> local gsr[], read back by copy_vertex()).
    void emit_ring(const Cf& cf, int level) {
        if (cf.export_type != 0 || cf.elem_size != 3 || cf.array_base % 4 || cf.rw_rel) {
            error(fmt("CF %u: MEM_RING form type %u elem %u base %u", cf.index, cf.export_type, cf.elem_size, cf.array_base));
            return;
        }
        if (sh_.stage == Stage::Pixel) {
            error(fmt("CF %u: MEM_RING in a pixel shader", cf.index));
            return;
        }
        line(fmt("if (%s) {  // CF %u MEM_RING", m(level).c_str(), cf.index));
        ++indent_;
        for (uint32_t b = 0; b <= cf.burst_count; ++b) {
            const uint32_t idx = cf.array_base / 4u + b, r = cf.rw_gpr + b;
            for (int c = 0; c < 4; ++c) {
                if (!(cf.comp_mask >> c & 1)) continue;
                if (sh_.stage == Stage::Vertex)
                    line(fmt("es%u.%c = intBitsToFloat(%s.%c);", idx, kChan[c], gpr(int(r)).c_str(), kChan[c]));
                else
                    line(fmt("gsr[%u].%c = %s.%c;", idx, kChan[c], gpr(int(r)).c_str(), kChan[c]));
            }
            if (sh_.stage == Stage::Vertex) es_outputs_.insert(idx);
            else ring_size_ = std::max(ring_size_, idx + 1);
        }
        --indent_;
        line("}");
    }

    // The GS copy shader, run per emitted vertex: fetches from the GS->VS ring, exports position and params.
    std::string copy_function() {
        const Program cp = decode(sh_.copy_program.data(), sh_.copy_program.size());
        if (!cp.ok()) {
            error("copy shader: " + cp.errors[0]);
            return "";
        }
        const std::vector<uint32_t> sem = vs_param_semantics(sh_);
        std::set<uint32_t> regs;
        std::string body;
        const auto reg = [&](uint32_t r) {
            regs.insert(r);
            return "C" + std::to_string(r);
        };
        for (const Cf& cf : cp.cf) {
            if (cf.inst == CF_VTX || cf.inst == CF_TEX) {
                for (const VtxFetch& v : cf.vtx) {
                    if (v.buffer_id != kRingBuffer || v.src_gpr != 0 || v.src_sel_x != 0 || v.offset % 16) {
                        error("copy shader: unsupported fetch");
                        continue;
                    }
                    for (int c = 0; c < 4; ++c)
                        if (v.dst_sel[c] < 4)
                            body += fmt("    %s.%c = gsr[base + %u].%c;\n", reg(v.dst_gpr).c_str(), kChan[c], v.offset / 16u, kChan[v.dst_sel[c]]);
                    copy_reads_ = std::max(copy_reads_, v.offset / 16u + 1);
                }
                if (!cf.tex.empty()) error("copy shader: texture fetch");
            } else if (cf.inst == CF_EXPORT || cf.inst == CF_EXPORT_DONE) {
                for (uint32_t b = 0; b <= cf.burst_count; ++b) {
                    const uint32_t base = cf.array_base + b, r = cf.rw_gpr + b;
                    std::string v = "intBitsToFloat(ivec4(";
                    for (int c = 0; c < 4; ++c) {
                        const uint8_t s = cf.sel[c];
                        v += (c ? ", " : "") + (s < 4 ? fmt("%s.%c", reg(r).c_str(), kChan[s]) : s == 5 ? "0x3F800000" : "0");
                    }
                    v += "))";
                    if (cf.export_type == EXPORT_POS && base == 60) body += "    gl_Position = " + v + ";\n";
                    else if (cf.export_type == EXPORT_PARAM && base < sem.size() && sem[base] != 0xFF)
                        body += fmt("    v%u = %s;\n", vary_.at(sem[base]), v.c_str());
                    else error(fmt("copy shader: export type %u base %u", cf.export_type, base));
                }
            } else if (cf.inst != CF_NOP) {
                error(fmt("copy shader: unsupported %s", cf.name()));
            }
        }
        std::string s = "void copy_vertex(int base) {\n";
        for (uint32_t r : regs) s += fmt("    ivec4 C%u = ivec4(0);\n", r);
        return s + body + "}\n";
    }

    // ---- exports ----------------------------------------------------------------------------------------------
    std::string export_value(const Cf& cf, uint32_t r) {
        std::string v = "intBitsToFloat(ivec4(";
        for (int c = 0; c < 4; ++c) {
            if (c) v += ", ";
            const uint8_t sel = cf.sel[c];
            v += sel < 4 ? fmt("%s.%c", gpr(r).c_str(), kChan[sel]) : sel == 5 ? std::string("0x3F800000") : std::string("0");
        }
        return v + "))";
    }

    void emit_export(const Cf& cf, int level) {
        if (level != 0) error(fmt("CF %u: export at stack depth %d", cf.index, level));
        if (cf.rw_rel) error(fmt("CF %u: relative export", cf.index));
        for (uint32_t b = 0; b <= cf.burst_count; ++b) {
            const uint32_t base = cf.array_base + b, r = cf.rw_gpr + b;
            if (sh_.stage == Stage::Pixel && cf.export_type == EXPORT_PIXEL) {
                if (base < 8) {
                    outputs_[base] = true;
                    line(fmt("o%u = %s;", base, export_value(cf, r).c_str()));
                    if (base == 0) {
                        uses_alpha_test_ = true;
                        line("if (!alpha_pass(o0.a)) demote;");
                    }
                } else if (base == 61) {
                    depth_export_ = true;
                    line(fmt("gl_FragDepth = intBitsToFloat(%s.%c);", gpr(r).c_str(), kChan[cf.sel[0] & 3]));
                } else {
                    error(fmt("CF %u: pixel export to %u", cf.index, base));
                }
            } else if (sh_.stage == Stage::Vertex && cf.export_type == EXPORT_POS) {
                if (base == 60) line("gl_Position = " + export_value(cf, r) + ";");
                else error(fmt("CF %u: position export to %u", cf.index, base));
            } else if (sh_.stage == Stage::Vertex && cf.export_type == EXPORT_PARAM) {
                const std::vector<uint32_t> sem = vs_param_semantics(sh_);
                if (base >= sem.size() || sem[base] == 0xFF) {
                    error(fmt("CF %u: param %u has no semantic", cf.index, base));
                    continue;
                }
                line(fmt("v%u = %s;", vary_.at(sem[base]), export_value(cf, r).c_str()));
            } else {
                error(fmt("CF %u: unsupported export type %u", cf.index, cf.export_type));
            }
        }
    }

    // ---- program ----------------------------------------------------------------------------------------------
    void body() {
        int level = 0;
        for (const Cf& cf : p_.cf) {
            switch (cf.inst) {
                case CF_ALU: case CF_ALU_PUSH_BEFORE: case CF_ALU_POP_AFTER: case CF_ALU_POP2_AFTER:
                case CF_ALU_ELSE_AFTER: case CF_ALU_BREAK:
                    if (cf.inst == CF_ALU_PUSH_BEFORE) {
                        line(m(level + 1) + " = " + m(level) + ";");
                        ++level;
                    }
                    emit_alu_clause(cf, level);
                    if (cf.inst == CF_ALU_POP_AFTER) level -= 1;
                    if (cf.inst == CF_ALU_POP2_AFTER) level -= 2;
                    if (cf.inst == CF_ALU_ELSE_AFTER) line(fmt("%s = %s && !%s;", m(level).c_str(), m(level - 1).c_str(), m(level).c_str()));
                    if (cf.inst == CF_ALU_BREAK) {
                        if (!loop_counter_ || level != loop_level_) error(fmt("CF %u: ALU_BREAK outside its loop level", cf.index));
                        line("if (!" + m(level) + ") break;");
                    }
                    break;
                case CF_TEX: case CF_VTX: case CF_VTX_TC:
                    line(fmt("if (%s) {  // CF %u %s", m(level).c_str(), cf.index, cf.name()));
                    ++indent_;
                    for (const TexFetch& t : cf.tex) emit_tex(t, cf);
                    for (const VtxFetch& v : cf.vtx) emit_vtx(v, cf);
                    --indent_;
                    line("}");
                    break;
                case CF_JUMP: case CF_NOP:
                    break;
                case CF_ELSE:
                    if (level < 1) error(fmt("CF %u: ELSE at depth 0", cf.index));
                    line(fmt("%s = %s && !%s;  // CF %u ELSE", m(level).c_str(), m(level - 1).c_str(), m(level).c_str(), cf.index));
                    break;
                case CF_PUSH:
                    line(m(level + 1) + " = " + m(level) + ";");
                    ++level;
                    break;
                case CF_POP:
                    level -= cf.pop_count;
                    break;
                case CF_LOOP_START_DX10:
                    if (loop_counter_) error(fmt("CF %u: nested loop", cf.index));
                    line(m(level + 1) + " = " + m(level) + ";");
                    ++level;
                    ++loop_counter_;
                    loop_level_ = level;
                    line(fmt("for (int it = 0; it < 4096 && %s; ++it) {  // CF %u LOOP_START_DX10", m(level).c_str(), cf.index));
                    ++indent_;
                    break;
                case CF_LOOP_END:
                    --indent_;
                    line("}");
                    --level;
                    --loop_counter_;
                    break;
                case CF_MEM_RING:
                    emit_ring(cf, level);
                    break;
                case CF_EMIT_VERTEX: case CF_EMIT_CUT_VERTEX: case CF_CUT_VERTEX:
                    if (sh_.stage != Stage::Geometry || loop_counter_) error(fmt("CF %u: %s outside straight GS code", cf.index, cf.name()));
                    line(fmt("if (%s) {  // CF %u %s", m(level).c_str(), cf.index, cf.name()));
                    if (cf.inst != CF_CUT_VERTEX) {
                        // the copy shader reads vertex k at k * item size, k = vertices emitted so far (branches
                        // reuse the same ring offsets, so k is dynamic)
                        ++emits_;
                        line("    copy_vertex(gs_emitted * GS_ITEMS);");
                        line("    EmitVertex();");
                        line("    ++gs_emitted;");
                    }
                    if (cf.inst != CF_EMIT_VERTEX) line("    EndPrimitive();");
                    line("}");
                    break;
                case CF_CALL_FS:
                    if (sh_.stage != Stage::Vertex) error(fmt("CF %u: CALL_FS outside a vertex shader", cf.index));
                    load_attributes();
                    break;
                case CF_EXPORT: case CF_EXPORT_DONE:
                    emit_export(cf, level);
                    break;
                default:
                    error(fmt("CF %u: unsupported %s", cf.index, cf.name()));
                    break;
            }
            if (errors_.size() >= 8) return;
        }
    }
    int loop_level_ = 0;

    void load_attributes() {
        const uint32_t n = std::min<uint32_t>(sh_.regs[vsreg::kNumSemantic], 32);
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t sem = sh_.regs[vsreg::kSemantic + i] & 0xFF;
            if (sem == 0xFF) continue;
            if (sem >= 32) error(fmt("attribute semantic %u", sem));
            vs_inputs_.insert(sem);
            line(fmt("%s = ivec4(a%u);", gpr(int(i + 1)).c_str(), sem));
        }
    }

    std::string main_text() {
        std::string s = "void main() {\n";
        for (int r : gprs_) s += fmt("    ivec4 R%d = ivec4(0);\n", r);
        s += "    ivec4 PV = ivec4(0);\n    int PS = 0;\n";
        for (int l = 0; l <= max_level_; ++l) s += fmt("    bool m%d = %s;\n", l, l ? "false" : "true");
        if (sh_.stage == Stage::Geometry) s += "    int gs_emitted = 0;\n";
        if (sh_.stage == Stage::Vertex) {
            if (gprs_.count(0)) s += "    R0 = ivec4(gl_VertexIndex, 0, 0, gl_InstanceIndex);\n";
            if (es_outputs_.empty()) s += "    gl_Position = vec4(0.0);\n    gl_PointSize = pc.point_size;\n";
        } else if (sh_.stage == Stage::Pixel) {
            const std::vector<uint32_t> sem = ps_input_semantics(sh_);
            for (size_t i = 0; i < sem.size(); ++i)
                if (gprs_.count(int(i)) && sem[i] != 0xFF) s += fmt("    R%zu = floatBitsToInt(v%u);\n", i, vary_.at(sem[i]));
            const uint32_t c0 = sh_.regs[psreg::kInControl0];
            if (c0 & (1u << 8) && gprs_.count(int(c0 >> 10 & 0x1F))) s += fmt("    R%u = floatBitsToInt(gl_FragCoord);\n", c0 >> 10 & 0x1F);
            for (const auto& [k, w] : outputs_) s += fmt("    o%u = vec4(0.0);\n", k), (void)w;
        }
        return s + out_ + "}\n";
    }

    std::string header() {
        static const char* kStage[] = {"vertex", "pixel", "geometry"};
        const uint32_t set = sh_.stage == Stage::Vertex ? abi::kSetVertex : sh_.stage == Stage::Pixel ? abi::kSetPixel : abi::kSetGeometry;
        std::string s = "#version 450\n";
        if (sh_.stage == Stage::Pixel) s += "#extension GL_EXT_demote_to_helper_invocation : require\n";
        s += fmt("// Latte %s shader %016llx, translated by shaderc-latte\n\n", kStage[int(sh_.stage)],
                 static_cast<unsigned long long>(sh_.key()));
        for (const auto& [b, n] : ubo_vec4s_) {
            std::string name;
            for (const UniformBlock& u : sh_.uniform_blocks)
                if (u.index == uint32_t(b)) name = u.name;
            s += fmt("layout(std140, set = %u, binding = %u) uniform UB%d { ivec4 d[%u]; } ub%d;  // %s\n", set,
                     abi::uniform_block_binding(b), b, n, b, name.c_str());
        }
        for (const auto& [slot, type] : samplers_) {
            const char* t = type == 0 ? "sampler1D" : type == 4 ? "samplerCube" : type == 6 ? "sampler2DShadow" : "sampler2D";
            std::string name;
            for (const SamplerVar& v : sh_.samplers)
                if (v.location == uint32_t(slot)) name = v.name;
            s += fmt("layout(set = %u, binding = %u) uniform %s tex%d;  // %s\n", set, abi::sampler_binding(slot), t, slot, name.c_str());
        }
        if (sh_.stage == Stage::Vertex) {
            for (uint32_t a : vs_inputs_) s += fmt("layout(location = %u) in uvec4 a%u;\n", a, a);
            if (es_outputs_.empty()) s += param_outputs() + kVertexPushConstants;
            for (uint32_t e : es_outputs_) s += fmt("layout(location = %u) out vec4 es%u;  // ES->GS ring\n", e, e);
        } else if (sh_.stage == Stage::Geometry) {
            static const char* kPrim[] = {"points", "line_strip", "triangle_strip"};
            const uint32_t prim = sh_.regs[gsreg::kOutPrimType];
            const uint32_t item_dw = sh_.regs[gsreg::kVertItemSize];
            if (prim > 2) error(fmt("GS output primitive type %u", prim));
            if (item_dw % 4 || item_dw / 4 < copy_reads_) error(fmt("GS vertex item size %u dwords, copy shader reads %u vec4", item_dw, copy_reads_));
            s += fmt("layout(points) in;\nlayout(%s, max_vertices = %u) out;\n", kPrim[prim % 3], std::max(emits_, 1u));
            for (uint32_t e : gs_inputs_) s += fmt("layout(location = %u) in vec4 es%u[];  // ES->GS ring\n", e, e);
            s += param_outputs();
            s += fmt("const int GS_ITEMS = %u;  // vec4s per vertex in the GS->VS ring\n", item_dw / 4);
            s += fmt("ivec4 gsr[%u];\n", std::max(ring_size_, std::max(emits_, 1u) * (item_dw / 4)));
        } else if (sh_.stage == Stage::Pixel) {
            const std::vector<uint32_t> sem = ps_input_semantics(sh_);
            for (size_t i = 0; i < sem.size(); ++i) {
                if (sem[i] == 0xFF) continue;
                const uint32_t cntl = sh_.regs[psreg::kInput + i];
                std::string q;
                if (cntl & (1u << 10)) q += "flat ";
                if (cntl & (1u << 11)) q += "centroid ";
                if (cntl & (1u << 12)) q += "noperspective ";
                s += fmt("layout(location = %u) %sin vec4 v%u;\n", vary_.at(sem[i]), q.c_str(), vary_.at(sem[i]));
            }
            for (const auto& [k, w] : outputs_) s += fmt("layout(location = %u) out vec4 o%u;\n", k, k), (void)w;
            if (uses_alpha_test_) s += kAlphaTest;
        }
        return s + "\n" + kHelpers + "\n" + copy_text_ + (copy_text_.empty() ? "" : "\n");
    }

    std::string param_outputs() {
        std::set<uint32_t> outs;
        for (uint32_t sem : vs_param_semantics(sh_))
            if (sem != 0xFF) outs.insert(vary_.at(sem));
        std::string s;
        for (uint32_t l : outs) s += fmt("layout(location = %u) out vec4 v%u;\n", l, l);
        return s;
    }
};

}  // namespace

GlslResult emit_glsl(const Shader& shader, const Program& program, const VaryingTable& varyings) {
    if (shader.stage == Stage::Pixel) {
        const uint32_t c0 = shader.regs[psreg::kInControl0], c1 = shader.regs[psreg::kInControl1];
        if ((c0 >> 15 & 0xF) || c1 & (1u << 8) || c1 & 1u || c1 & (1u << 24))
            return {"", {fmt("pixel inputs need front face/param gen/fixed-point position (spi_ps_in_control %08X %08X)", c0, c1)}};
    }
    if (shader.mode != 1 && shader.mode != 2) return {"", {fmt("shader mode %u (uniform registers) is not supported", shader.mode)}};
    return Emitter(shader, program, varyings).run();
}

}  // namespace mh3u::latte
