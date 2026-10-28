// Interface between translated Latte shaders (tools/shaderc-latte) and the runtime's Vulkan pipelines.
// Both sides include this header; changing it requires regenerating build/shaders.
#pragma once

#include <cstdint>

namespace mh3u::gfx::shader_abi {

// Descriptor set per stage.
constexpr uint32_t kSetVertex = 0, kSetPixel = 1, kSetGeometry = 2;

// Uniform block b (KCACHE bank b, buffer id 0x80 + b) -> binding b. Contents are the guest block as is: the game
// stores uniform blocks already in GPU (little-endian) word order. Declared as ivec4 d[] (std140).
constexpr uint32_t kMaxUniformBlocks = 16;
constexpr uint32_t uniform_block_binding(uint32_t block) { return block; }

// Texture/sampler slot s (GX2 sampler location = TEX resource id = sampler id) -> combined image sampler.
// Image view type follows the shader's sampler var: 1D, 2D, cube, 2D + depth compare (shadow).
constexpr uint32_t kMaxSamplers = 18;
constexpr uint32_t sampler_binding(uint32_t slot) { return 32 + slot; }

// Vertex inputs: GPR i+1 is loaded from attribute semantic sq_vtx_semantic[i] (= GX2 attrib location), declared
// `layout(location = semantic) in uvec4`. The runtime supplies each attribute already converted to the 32-bit
// values the game's fetch shader would leave in the GPR (R32G32B32A32_UINT bit patterns).
// Varyings: each VS param / PS input semantic id is given a dense location by the translator; the table is
// written to build/shaders/varyings.txt and is the same for every shader.

// Pixel-shader push constants (offset 0): GX2 alpha test (GX2SetAlphaTest), applied to colour target 0 before export.
struct PixelPushConstants {
    uint32_t alpha_func;  // GX2CompareFunction: 0 never .. 7 always
    float alpha_ref;
};
// Vertex-shader push constants (offset 16): point size for point lists (GX2SetPointSize), written to gl_PointSize.
constexpr uint32_t kVertexPushOffset = 16;
struct VertexPushConstants {
    float point_size;
};

// Next to each <key>.<stage>.spv the translator writes <key>.<stage>.meta listing what the shader uses:
// "ubo <block> <vec4 count>", "sampler <slot> <type>" (GX2SamplerVarType: 0 1D, 1 2D, 4 cube, 6 2D shadow),
// "input <attribute semantic>".

}  // namespace mh3u::gfx::shader_abi
