// Links every generated translation unit (emitted functions, stubs, imports, function table) into one program, so
// every call the emitter produced must resolve. Checks the function table. Exit 77 = skipped (no dump).
#include "ppc_context.h"

#include <cstdio>

extern const mh3u::ppc::FunctionEntry kFunctionTable[];
extern const size_t kFunctionCount;

int main() {
    if (kFunctionCount == 0) {
        std::printf("skipped: generated without the dump\n");
        return 77;
    }
    size_t bad = 0;
    for (size_t i = 0; i < kFunctionCount; ++i) {
        if (!kFunctionTable[i].fn || (i && kFunctionTable[i].addr <= kFunctionTable[i - 1].addr)) ++bad;
    }
    std::printf("function table: %zu entries, %zu bad\n", kFunctionCount, bad);
    return bad ? 1 : 0;
}
