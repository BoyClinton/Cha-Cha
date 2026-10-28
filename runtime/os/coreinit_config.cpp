// coreinit UC* (system configuration) and MCP (title/product info).
// UCSysConfig entry (0x54 bytes): char name[64]; u32 access; u32 dataType; s32 error; u32 dataSize; void* data.
// Values: English (cafe.language = 1), USA (cafe.cntry_reg = 49), no parental restrictions; unknown names -> 0.
#include "hle.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

namespace mh3u::os {
namespace {

const std::map<std::string, uint32_t> kConfig = {
    {"cafe.language", 1},     // 0 JP, 1 EN, 2 FR, 3 DE, 4 IT, 5 ES, 6 ZH, 7 KO, 8 NL, 9 PT, 10 RU, 11 TW
    {"cafe.cntry_reg", 49},   // USA
    {"cafe.eula_agree", 1},
    {"cafe.initial_launch", 2},
};

HLE_FUNC(coreinit, UCOpen) { ret(ctx, 1); }
HLE_FUNC(coreinit, UCClose) { ret(ctx, 0); }
// UCError UCReadSysConfig(IOSHandle, u32 count, UCSysConfig* entries)
HLE_FUNC(coreinit, UCReadSysConfig) {
    const uint32_t count = arg(ctx, 1), entries = arg(ctx, 2);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t e = entries + i * 0x54;
        std::string name = guest_cstr(e);
        const uint32_t size = ppc::ld32(base, e + 0x4C), data = ppc::ld32(base, e + 0x50);
        auto it = kConfig.find(name);
        uint32_t v = it == kConfig.end() ? 0 : it->second;
        if (data && size) {
            std::memset(base + data, 0, size);
            if (size == 1) base[data] = static_cast<uint8_t>(v);
            else if (size == 2) ppc::st16(base, data, static_cast<uint16_t>(v));
            else if (size >= 4) ppc::st32(base, data, v);
        }
        ppc::st32(base, e + 0x48, 0);  // error = OK
        if (rt().trace_imports)
            std::fprintf(stderr, "  UCReadSysConfig %s (size %u) -> %u%s\n", name.c_str(), size, v,
                         it == kConfig.end() ? " (default)" : "");
    }
    ret(ctx, 0);
}
HLE_FUNC(coreinit, UCWriteSysConfig) { ret(ctx, 0); }

// MCP: IOSHandle MCP_Open(); MCP_Close(handle)
HLE_FUNC(coreinit, MCP_Open) { ret(ctx, 2); }
HLE_FUNC(coreinit, MCP_Close) { ret(ctx, 0); }
// MCPError MCP_GetSysProdSettings(handle, MCPSysProdSettings* out): product_area u32 @+0x00, game_region u32 @+0x08
// (layout from public Wii U headers, unconfirmed in this binary); region bits: 1 JPN, 2 USA, 4 EUR
HLE_FUNC(coreinit, MCP_GetSysProdSettings) {
    const uint32_t out = arg(ctx, 1);
    std::memset(base + out, 0, 0x46);
    ppc::st32(base, out + 0x00, 2);
    ppc::st32(base, out + 0x08, 2);
    ret(ctx, 0);
}
HLE_FUNC(coreinit, UCDeleteSysConfig) { ret(ctx, 0); }

}  // namespace

void register_coreinit_config(Runtime&) {
    HLE_REGISTER(coreinit, UCOpen);
    HLE_REGISTER(coreinit, UCClose);
    HLE_REGISTER(coreinit, UCReadSysConfig);
    HLE_REGISTER(coreinit, UCWriteSysConfig);
    HLE_REGISTER(coreinit, UCDeleteSysConfig);
    HLE_REGISTER(coreinit, MCP_Open);
    HLE_REGISTER(coreinit, MCP_Close);
    HLE_REGISTER(coreinit, MCP_GetSysProdSettings);
}

}  // namespace mh3u::os
