// GFD ("Gfx2") shader containers: the GX2VertexShader / GX2PixelShader / GX2GeometryShader header blocks and
// their program blocks. Struct layouts from wut gx2/shaders.h; pointers inside a header block are stored as
// 0xD06xxxxx (data) / 0xCA7xxxxx (string) with the low 20 bits an offset into that block.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mh3u::latte {

enum class Stage { Vertex, Pixel, Geometry };

struct UniformBlock {
    std::string name;
    uint32_t index = 0, size = 0;  // index = constant-buffer slot (KCACHE bank / buffer id 0x80+index)
};
struct UniformVar {
    std::string name;
    uint32_t type = 0, count = 0, offset = 0;  // offset in 32-bit words
    int32_t block = -1;                         // -1: register file (uniform-register mode)
};
struct InitialValue {
    float value[4] = {};
    uint32_t offset = 0;
};
struct LoopVar {
    uint32_t offset = 0, value = 0;
};
struct SamplerVar {
    std::string name;
    uint32_t type = 0, location = 0;
};
struct AttribVar {
    std::string name;
    uint32_t type = 0, count = 0, location = 0;
};

struct Shader {
    Stage stage = Stage::Pixel;
    std::vector<uint32_t> regs;  // the header's register block (VS 0xD0, PS 0xA4, GS 0x4C bytes)
    std::vector<uint8_t> program;       // little-endian Latte microcode
    std::vector<uint8_t> copy_program;  // GS only: the VS-side copy shader
    uint32_t mode = 0;                  // GX2ShaderMode: 0 uniform register, 1 uniform block, 2 geometry
    uint32_t ring_item_size = 0;
    std::vector<UniformBlock> uniform_blocks;
    std::vector<UniformVar> uniform_vars;
    std::vector<InitialValue> initial_values;
    std::vector<LoopVar> loop_vars;
    std::vector<SamplerVar> samplers;
    std::vector<AttribVar> attribs;

    // Lookup key shared with the runtime: FNV-1a 64 over the register block (big-endian words, as in guest
    // memory) followed by the program bytes.
    uint64_t key() const;
};

// Parses every shader in a GFD (a VS+GS file yields two). Returns false with a message on malformed input.
bool parse_gfd(const uint8_t* data, size_t size, std::vector<Shader>& out, std::string* error);

// Register-block sizes in bytes, i.e. the offset of the program size field in the guest struct.
constexpr uint32_t kVsRegsBytes = 0xD0, kPsRegsBytes = 0xA4, kGsRegsBytes = 0x4C;

uint64_t shader_key(const uint8_t* regs_be, size_t regs_size, const uint8_t* program, size_t program_size);

}  // namespace mh3u::latte
