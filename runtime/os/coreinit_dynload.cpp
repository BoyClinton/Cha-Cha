// OSDynLoad: the game loads swkbd/erreula at run time (53 exports, config/imports.csv kind dynfunc).
// Acquire hands out a module id per name; FindExport returns the guest address of the registered HLE function
// ("swkbd.Swkbd_Create"), or of a reporter that names the export if the game calls one nobody implemented yet.
// Data exports get a zeroed 256-byte guest block.
#include "hle.h"

#include <cstdio>
#include <map>
#include <mutex>
#include <string>

namespace mh3u::os {
namespace {

std::mutex g_m;
std::map<std::string, uint32_t> g_modules;      // name -> module id
std::map<uint32_t, std::string> g_module_names;
std::map<std::string, uint32_t> g_exports;      // "rpl.name" -> guest address
std::map<uint32_t, std::string> g_missing;      // address of an unimplemented export -> its name

std::string module_name(std::string n) {
    if (n.size() > 4 && n.compare(n.size() - 4, 4, ".rpl") == 0) n.resize(n.size() - 4);
    return n;
}

void missing_export(ppc::Context&, uint8_t*) {
    std::string name;
    {
        std::lock_guard g(g_m);
        auto it = g_missing.find(ppc::current_indirect_target());
        name = it == g_missing.end() ? "?" : it->second;
    }
    ppc::unimplemented_import(name.c_str());
}

HLE_FUNC(coreinit, OSDynLoad_SetAllocator) { ret(ctx, 0); }
// OSDynLoad_Error OSDynLoad_Acquire(const char* name, OSDynLoad_Module* out)
HLE_FUNC(coreinit, OSDynLoad_Acquire) {
    std::string name = module_name(guest_cstr(arg(ctx, 0)));
    std::lock_guard g(g_m);
    auto [it, fresh] = g_modules.emplace(name, static_cast<uint32_t>(g_modules.size() + 1));
    if (fresh) g_module_names[it->second] = name;
    ppc::st32(base, arg(ctx, 1), it->second);
    if (rt().trace_imports) std::fprintf(stderr, "  OSDynLoad_Acquire(\"%s\") -> %u\n", name.c_str(), it->second);
    ret(ctx, 0);
}
HLE_FUNC(coreinit, OSDynLoad_Release) {}
// OSDynLoad_Error OSDynLoad_FindExport(OSDynLoad_Module, BOOL isData, const char* name, void** out)
HLE_FUNC(coreinit, OSDynLoad_FindExport) {
    std::string mod;
    {
        std::lock_guard g(g_m);
        auto it = g_module_names.find(arg(ctx, 0));
        mod = it == g_module_names.end() ? "?" : it->second;
    }
    const std::string qualified = mod + "." + guest_cstr(arg(ctx, 2));
    uint32_t addr;
    {
        std::lock_guard g(g_m);
        auto it = g_exports.find(qualified);
        if (it != g_exports.end()) {
            addr = it->second;
        } else if (arg(ctx, 1)) {
            addr = rt().mem->alloc_hle(256, 8);
            g_exports[qualified] = addr;
        } else if (ppc::GuestFunction fn = ppc::resolve_import(qualified.c_str())) {
            addr = hle_function_address(fn);
            g_exports[qualified] = addr;
        } else {
            addr = hle_function_address(&missing_export);
            g_missing[addr] = qualified;
            g_exports[qualified] = addr;
        }
    }
    ppc::st32(base, arg(ctx, 3), addr);
    ret(ctx, 0);
}

}  // namespace

void register_coreinit_dynload(Runtime&) {
    HLE_REGISTER(coreinit, OSDynLoad_SetAllocator);
    HLE_REGISTER(coreinit, OSDynLoad_Acquire);
    HLE_REGISTER(coreinit, OSDynLoad_Release);
    HLE_REGISTER(coreinit, OSDynLoad_FindExport);
}

}  // namespace mh3u::os
