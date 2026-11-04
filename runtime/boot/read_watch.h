// Read watchpoint for reverse engineering (MH3U_WATCH_READ): which recompiled functions read a guest word.
//   MH3U_WATCH_READ="1234ABCD"        the word at guest address 0x1234ABCD
//   MH3U_WATCH_READ="@10312BB0+6C"    the word at (u32 at 0x10312BB0) + 0x6C, e.g. a singleton's field; resolved
//                                     once the pointer is non-zero
// The page holding the word is made a guard page; every access to the word is counted per reading function (and
// its two callers), the page re-armed after a single step. report() prints them symbolized (RelWithDebInfo build).
#pragma once

#include <cstdint>

namespace mh3u::read_watch {

// Parses MH3U_WATCH_READ; base = guest memory base.
void start_from_env(uint8_t* base);
// Watches the word at a guest address from code (re-targets when called with another address); writes_only counts
// only writes and keeps a per-second timeline of the writers (MH3U_WATCH_WRITES=1 does the same for the env form).
void watch(uint8_t* base, uint32_t guest, bool writes_only);
// Main thread, periodically: resolves the address and (re)arms the guard page.
void poll();
void report();

}  // namespace mh3u::read_watch
