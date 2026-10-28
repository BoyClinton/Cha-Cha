// PPC -> C++ emitter. Turns one guest function (its instruction words) into a C++ function
//   void sub_XXXXXXXX(mh3u::ppc::Context& ctx, uint8_t* base)
// operating on the guest context and big-endian guest memory (recomp/include/ppc_context.h).
//
// Calls run on the host stack: `bl f` = `ctx.lr = pc + 4; sub_f(ctx, base);`, `bclr` = `return` (correct for ABI-
// conforming code, which only uses LR to return), `b`/`bc` to another function's entry = tail call
// (`sub_f(ctx, base); return;`), calls to Cafe OS imports go to `import_<rpl>_<name>` (HLE). `bl` to the next
// instruction (get-PC idiom) only sets LR. Switch `bctr`s (EmitEnv::switch_slots) become `switch (ctx.ctr)` over the
// table slots; other `bctrl`/`bctr`/`bclrl` go through ppc::call_indirect (function table lookup), a plain `bctr`
// as a tail call. Not yet handled (rejected with a reason, never guessed): lwarx/stwcx., multi-range bodies.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mh3u::recomp {

struct EmitResult {
    bool ok = false;
    std::string code;    // the C++ function (ok) or empty
    std::string reason;  // why it was rejected, e.g. "call", "unsupported: mftb", "branch out of body"
    std::string reason_key;  // reason without addresses, for statistics
};

struct CodeRange {
    uint32_t start;
    std::vector<uint32_t> words;  // words[i] is the instruction at start + 4*i
};

// What the emitter needs to know about the rest of the program.
struct EmitEnv {
    std::function<bool(uint32_t)> is_entry;            // address starts a function (call/tail-call target)
    std::function<std::string(uint32_t)> import_call;  // C++ name of the import called by the b/bl at this site, or ""
    // For a bctr that dispatches a switch: the addresses of its table slots (inline `b case` instructions), else null.
    std::function<const std::vector<uint32_t>*(uint32_t)> switch_slots;
    // The b/bl at this site targets an undefined weak symbol ($UNDEF -> address 0).
    std::function<bool(uint32_t)> undefined_call;
};

// `words[i]` is the instruction at `start + 4*i`; `comment` is copied into the function header (e.g. symbol name).
EmitResult emit_function(uint32_t start, const std::vector<uint32_t>& words, const std::string& comment,
                         const EmitEnv& env);
// A body made of several ranges (entry range + chunks from config/functions.csv); emitted as one C++ function.
EmitResult emit_function(uint32_t entry, const std::vector<CodeRange>& ranges, const std::string& comment,
                         const EmitEnv& env);
// Same with an empty environment: any call or branch out of the body is rejected.
EmitResult emit_leaf_function(uint32_t start, const std::vector<uint32_t>& words, const std::string& comment = {});

std::string import_function_name(const std::string& rpl, const std::string& name);  // "import_coreinit_OSCreateThread"

std::string function_name(uint32_t addr);  // "sub_02000E84"

}  // namespace mh3u::recomp
