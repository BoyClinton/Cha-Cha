// The few Microsoft C runtime functions the code base uses, for builds on other systems (CMake force-includes this
// header there). Same signatures and return conventions as the MSVC versions.
#pragma once
#ifndef _WIN32
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

inline int _dupenv_s(char** out, size_t* len, const char* name) {
    const char* v = std::getenv(name);
    *out = v ? strdup(v) : nullptr;
    if (len) *len = v ? std::strlen(v) + 1 : 0;
    return 0;
}
inline int fopen_s(FILE** f, const char* path, const char* mode) {
    *f = std::fopen(path, mode);
    return *f ? 0 : errno;
}
inline int gmtime_s(std::tm* out, const std::time_t* t) { return gmtime_r(t, out) ? 0 : errno; }
inline std::time_t _mkgmtime(std::tm* t) { return timegm(t); }
inline int _putenv_s(const char* name, const char* value) { return setenv(name, value, 1); }
#define sscanf_s std::sscanf
#endif
