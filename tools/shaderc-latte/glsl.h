// Latte program -> GLSL 4.50 (Vulkan) source, compiled to SPIR-V with glslc. Bindings follow
// runtime/gfx/shader_abi.h.
//
// Model: GPRs are ivec4 bit patterns (R0..R127, float ops bitcast); each VLIW group computes all results from the
// old register state, then writes GPRs and PV/PS. Branching is emulated per invocation with the hardware's
// active-mask stack (bool m0..mN): clauses run under `if (m<level>)`, PRED_SET*+UPDATE_EXEC_MASK sets the mask,
// ELSE inverts it against the parent level, POP drops levels. JUMPs are only skips for fully inactive wavefronts,
// so they are dropped after a static check that the stack depth at each target matches. DX10 loops become `for`
// loops with ALU_BREAK leaving them.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "gfd.h"
#include "latte.h"

namespace mh3u::latte {

// Varying semantic id -> location, shared by every shader of the game.
using VaryingTable = std::map<uint32_t, uint32_t>;

std::vector<uint32_t> vs_param_semantics(const Shader& s);  // param export j -> semantic
std::vector<uint32_t> ps_input_semantics(const Shader& s);  // PS input i (GPR i) -> semantic

struct GlslResult {
    std::string source;
    // Resource use for the runtime's descriptor layouts, one item per line: "ubo <block> <vec4s>",
    // "sampler <slot> <GX2SamplerVarType>", "input <attribute semantic>".
    std::string meta;
    std::vector<std::string> errors;
};

GlslResult emit_glsl(const Shader& shader, const Program& program, const VaryingTable& varyings);

}  // namespace mh3u::latte
