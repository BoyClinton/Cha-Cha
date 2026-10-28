// Sampling profiler for finding where frame time goes (MH3U_PROFILE=1). A host thread suspends every other thread
// of the process about 1000 times a second, walks its stack, and counts (a) the function it is in and (b) for samples
// outside Cha-Cha.exe (OS waits, sleeps, driver), the first callers in Cha-Cha.exe – the blocking site. report() prints the
// top entries per thread, named by the thread's description (guest threads set one) and symbolized from the PDB
// (build RelWithDebInfo for useful names; recompiled game code shows as sub_<ADDR> or its config/symbols.txt name).
#pragma once

namespace mh3u::profiler {

// Starts sampling if MH3U_PROFILE is set. Main thread, once.
void start_from_env();
bool running();
// Stops sampling and prints the report to stdout (no-op if not started).
void report();
// Names the calling thread for the report and debuggers (Windows thread description).
void name_this_thread(const wchar_t* name);

}  // namespace mh3u::profiler
