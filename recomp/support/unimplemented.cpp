// Default handlers for generated stubs: report and stop. Reaching one means a function the emitter rejected, or a
// Cafe OS import that has no HLE implementation yet.
#include "ppc_context.h"

#include <cstdio>
#include <cstdlib>

namespace mh3u::ppc {

void unimplemented_function(uint32_t addr) {
    std::fprintf(stderr, "recomp: function %08X was not recompiled (see recomp/out/emit_report.txt)\n", addr);
    std::fflush(stderr);
    std::_Exit(2);
}

void trap(uint32_t pc) {
    std::fprintf(stderr, "recomp: guest trap (tw/twi) at %08X\n", pc);
    std::fflush(stderr);
    std::_Exit(2);
}

void unimplemented_import(const char* name) {
    std::fprintf(stderr, "recomp: import %s has no HLE implementation\n", name);
    std::fflush(stderr);
    std::_Exit(2);
}

}  // namespace mh3u::ppc
