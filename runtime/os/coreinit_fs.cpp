// coreinit FS*: the synchronous Cafe file API on top of runtime/fs OverlayFs (/vol/content = update over base, read
// from the user's .wua or dump folder; /vol/save = writable host folder). Only the synchronous calls are imported by this game.
// Status codes: 0 OK, -2 END, -4 ALREADY_OPEN, -5 EXISTS, -6 NOT_FOUND, -7 NOT_FILE, -8 NOT_DIR, -9 ACCESS_ERROR.
#include "hle.h"
#include "overlay_fs.h"
#include "save_reads.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>

namespace mh3u::os {
namespace {

namespace stdfs = std::filesystem;

constexpr int32_t kOk = 0, kEnd = -2, kExists = -5, kNotFound = -6, kNotFile = -7, kNotDir = -8, kAccess = -9;

// Read-only files come from the game image (a .wua or dump folder) through a positional reader; files opened for
// writing (saves) are host files.
struct OpenFile {
    std::unique_ptr<fs::FileReader> reader;
    uint64_t pos = 0;
    std::fstream f;
    uint64_t size = 0;
    bool writable = false;
    std::string guest_path;
};
struct OpenDir {
    std::vector<fs::DirEntry> entries;
    size_t next = 0;
};

std::mutex g_m;
std::map<uint32_t, std::unique_ptr<OpenFile>> g_files;
std::map<uint32_t, std::unique_ptr<OpenDir>> g_dirs;
uint32_t g_next_handle = 1;
int32_t g_last_error = 0;

int32_t status(ppc::Context& ctx, int32_t s) {
    g_last_error = s;
    ret(ctx, static_cast<uint32_t>(s));
    return s;
}

// FSStat (0x64 bytes): flags(+0) permission(+4) owner(+8) group(+0xC) size(+0x10) allocSize(+0x14) quota(+0x18)
// entryId(+0x20) created(+0x24) modified(+0x2C) attributes(+0x34)
void write_stat(uint8_t* base, uint32_t out, bool is_dir, uint64_t size) {
    std::memset(base + out, 0, 0x64);
    constexpr uint32_t kDirFlag = 0x80000000, kFileFlags = 0x01000000;  // directory / "is file" flag values assumed
    ppc::st32(base, out, is_dir ? kDirFlag : kFileFlags);
    ppc::st32(base, out + 4, 0x660);  // owner/group read+write
    ppc::st32(base, out + 0x10, static_cast<uint32_t>(size));
    ppc::st32(base, out + 0x14, static_cast<uint32_t>(size));
}

std::string guest_path(uint32_t addr) { return guest_cstr(addr); }

void trace_fs(const char* what, const std::string& path, int32_t s) {
    if (rt().trace_imports) std::fprintf(stderr, "  %s(\"%s\") -> %d\n", what, path.c_str(), s);
}

HLE_FUNC(coreinit, FSInit) {}
HLE_FUNC(coreinit, FSShutdown) {}
// FSStatus FSAddClient(FSClient*, FSErrorFlag) / FSDelClient(FSClient*, FSErrorFlag)
HLE_FUNC(coreinit, FSAddClient) { status(ctx, kOk); }
HLE_FUNC(coreinit, FSDelClient) { status(ctx, kOk); }
HLE_FUNC(coreinit, FSInitCmdBlock) { std::memset(base + arg(ctx, 0), 0, 0xA80); }  // sizeof(FSCmdBlock)
HLE_FUNC(coreinit, FSSetStateChangeNotification) {}
HLE_FUNC(coreinit, FSGetCurrentCmdBlock) { ret(ctx, 0); }
// user data pointer stored in the command block (offset assumed: last word of the block)
HLE_FUNC(coreinit, FSSetUserData) { ppc::st32(base, arg(ctx, 0) + 0xA7C, arg(ctx, 1)); }
HLE_FUNC(coreinit, FSGetUserData) { ret(ctx, ppc::ld32(base, arg(ctx, 0) + 0xA7C)); }
HLE_FUNC(coreinit, FSGetLastError) { ret(ctx, static_cast<uint32_t>(g_last_error)); }
HLE_FUNC(coreinit, FSGetLastErrorCodeForViewer) { ret(ctx, g_last_error ? 1503000 : 0); }

// FSStatus FSOpenFile(FSClient*, FSCmdBlock*, const char* path, const char* mode, FSFileHandle* out, FSErrorFlag)
HLE_FUNC(coreinit, FSOpenFile) {
    std::string path = guest_path(arg(ctx, 2)), mode = guest_cstr(arg(ctx, 3));
    const bool write = mode.find_first_of("wa+") != std::string::npos;
    auto of = std::make_unique<OpenFile>();
    if (write) {
        std::optional<stdfs::path> host = rt().vfs->resolve_for_write(path);
        if (!host) return trace_fs("FSOpenFile", path, status(ctx, kAccess)), void();
        std::error_code ec;
        if (stdfs::is_directory(*host, ec)) return trace_fs("FSOpenFile", path, status(ctx, kNotFile)), void();
        std::ios::openmode m = std::ios::binary | std::ios::in | std::ios::out;
        if (mode.find('w') != std::string::npos) m |= std::ios::trunc;
        else if (mode.find('a') != std::string::npos) m |= std::ios::app;
        if (!stdfs::exists(*host, ec)) std::ofstream(*host, std::ios::binary).close();
        of->f.open(*host, m);
        if (!of->f) return trace_fs("FSOpenFile", path, status(ctx, kAccess)), void();
        of->size = stdfs::file_size(*host, ec);
    } else {
        const auto node = rt().vfs->stat(path);
        if (!node) return trace_fs("FSOpenFile", path, status(ctx, kNotFound)), void();
        if (node->is_dir) return trace_fs("FSOpenFile", path, status(ctx, kNotFile)), void();
        of->reader = rt().vfs->open(path);
        if (!of->reader) return trace_fs("FSOpenFile", path, status(ctx, kAccess)), void();
        of->size = of->reader->size();
    }
    of->writable = write;
    of->guest_path = path;
    std::lock_guard g(g_m);
    uint32_t h = g_next_handle++;
    g_files[h] = std::move(of);
    ppc::st32(base, arg(ctx, 4), h);
    trace_fs("FSOpenFile", path, status(ctx, kOk));
}

OpenFile* file(uint32_t h) {
    std::lock_guard g(g_m);
    auto it = g_files.find(h);
    return it == g_files.end() ? nullptr : it->second.get();
}

// save_reads.h: the last reads of each save file
std::mutex g_save_reads_m;
std::map<std::string, std::vector<SaveRead>> g_save_reads;

void note_save_read(const OpenFile* f, uint32_t dest, uint64_t pos, uint64_t got) {
    if (!got || f->guest_path.compare(0, 10, "/vol/save/") != 0) return;
    const std::string name = f->guest_path.substr(f->guest_path.find_last_of('/') + 1);
    std::lock_guard g(g_save_reads_m);
    auto& v = g_save_reads[name];
    v.push_back({dest, pos, static_cast<uint32_t>(got)});
    if (name.compare(0, 5, "quest") == 0)  // when the game loads its DLC quests (MODS -> QUESTS keeps this copy in step)
        std::printf("save: game read %s bytes 0x%llX-0x%llX to 0x%08X\n", name.c_str(), static_cast<unsigned long long>(pos),
                    static_cast<unsigned long long>(pos + got), dest);
    if (v.size() > 64) v.erase(v.begin());
}

int32_t read_into(uint8_t* base, OpenFile* f, uint32_t dest, uint32_t size, uint32_t count) {
    const uint64_t want = uint64_t{size} * count;
    const uint64_t pos = f->reader ? f->pos : static_cast<uint64_t>(std::max<std::streamoff>(0, f->f.tellg()));
    if (f->reader) {
        const size_t got = f->reader->read(f->pos, base + dest, static_cast<size_t>(want));
        f->pos += got;
        note_save_read(f, dest, pos, got);
        return size ? static_cast<int32_t>(got / size) : 0;
    }
    f->f.read(reinterpret_cast<char*>(base + dest), static_cast<std::streamsize>(want));
    const auto got = static_cast<uint64_t>(f->f.gcount());
    f->f.clear();
    note_save_read(f, dest, pos, got);
    return size ? static_cast<int32_t>(got / size) : 0;
}

// s32 FSReadFile(client, block, u8* dest, u32 size, u32 count, FSFileHandle, u32 flag, FSErrorFlag) -> elements read
HLE_FUNC(coreinit, FSReadFile) {
    OpenFile* f = file(arg(ctx, 5));
    if (!f) return status(ctx, kAccess), void();
    ret(ctx, static_cast<uint32_t>(read_into(base, f, arg(ctx, 2), arg(ctx, 3), arg(ctx, 4))));
}
// s32 FSReadFileWithPos(client, block, u8* dest, u32 size, u32 count, u32 pos, FSFileHandle, u32 flag, FSErrorFlag)
HLE_FUNC(coreinit, FSReadFileWithPos) {
    OpenFile* f = file(arg(ctx, 6));
    if (!f) return status(ctx, kAccess), void();
    f->pos = arg(ctx, 5);
    if (!f->reader) f->f.seekg(arg(ctx, 5));
    ret(ctx, static_cast<uint32_t>(read_into(base, f, arg(ctx, 2), arg(ctx, 3), arg(ctx, 4))));
}
// s32 FSWriteFile(client, block, const u8* src, u32 size, u32 count, FSFileHandle, u32 flag, FSErrorFlag)
HLE_FUNC(coreinit, FSWriteFile) {
    OpenFile* f = file(arg(ctx, 5));
    if (!f || !f->writable) return status(ctx, kAccess), void();
    f->f.write(reinterpret_cast<const char*>(base + arg(ctx, 2)), static_cast<std::streamsize>(uint64_t{arg(ctx, 3)} * arg(ctx, 4)));
    ret(ctx, f->f ? arg(ctx, 4) : 0);
}
// FSStatus FSSetPosFile(client, block, FSFileHandle, u32 pos, FSErrorFlag)
HLE_FUNC(coreinit, FSSetPosFile) {
    OpenFile* f = file(arg(ctx, 2));
    if (!f) return status(ctx, kAccess), void();
    f->pos = arg(ctx, 3);
    if (!f->reader) {
        f->f.seekg(arg(ctx, 3));
        f->f.seekp(arg(ctx, 3));
    }
    status(ctx, kOk);
}
// FSStatus FSGetStatFile(client, block, FSFileHandle, FSStat* out, FSErrorFlag)
HLE_FUNC(coreinit, FSGetStatFile) {
    OpenFile* f = file(arg(ctx, 2));
    if (!f) return status(ctx, kAccess), void();
    write_stat(base, arg(ctx, 3), false, f->size);
    status(ctx, kOk);
}
HLE_FUNC(coreinit, FSFlushFile) {
    if (OpenFile* f = file(arg(ctx, 2))) f->f.flush();
    status(ctx, kOk);
}
// FSStatus FSCloseFile(client, block, FSFileHandle, FSErrorFlag)
HLE_FUNC(coreinit, FSCloseFile) {
    std::lock_guard g(g_m);
    g_files.erase(arg(ctx, 2));
    status(ctx, kOk);
}
// FSStatus FSGetStat(client, block, const char* path, FSStat* out, FSErrorFlag)
HLE_FUNC(coreinit, FSGetStat) {
    std::string path = guest_path(arg(ctx, 2));
    const auto node = rt().vfs->stat(path);
    if (!node) return trace_fs("FSGetStat", path, status(ctx, kNotFound)), void();
    write_stat(base, arg(ctx, 3), node->is_dir, node->size);
    trace_fs("FSGetStat", path, status(ctx, kOk));
}
// FSStatus FSOpenDir(client, block, const char* path, FSDirHandle* out, FSErrorFlag)
HLE_FUNC(coreinit, FSOpenDir) {
    std::string path = guest_path(arg(ctx, 2));
    const auto node = rt().vfs->stat(path);
    if (!node) return trace_fs("FSOpenDir", path, status(ctx, kNotFound)), void();
    if (!node->is_dir) return trace_fs("FSOpenDir", path, status(ctx, kNotDir)), void();
    auto d = std::make_unique<OpenDir>();
    d->entries = rt().vfs->list(path);
    std::lock_guard g(g_m);
    uint32_t h = g_next_handle++;
    g_dirs[h] = std::move(d);
    ppc::st32(base, arg(ctx, 3), h);
    trace_fs("FSOpenDir", path, status(ctx, kOk));
}
// FSStatus FSReadDir(client, block, FSDirHandle, FSDirEntry* out /* FSStat + char name[256] */, FSErrorFlag)
HLE_FUNC(coreinit, FSReadDir) {
    std::lock_guard g(g_m);
    auto it = g_dirs.find(arg(ctx, 2));
    if (it == g_dirs.end()) return status(ctx, kAccess), void();
    OpenDir& d = *it->second;
    if (d.next >= d.entries.size()) return status(ctx, kEnd), void();
    const fs::DirEntry& e = d.entries[d.next++];
    const uint32_t out = arg(ctx, 3);
    write_stat(base, out, e.is_dir, e.size);
    std::memset(base + out + 0x64, 0, 256);
    std::memcpy(base + out + 0x64, e.name.c_str(), std::min<size_t>(e.name.size(), 255));
    status(ctx, kOk);
}
HLE_FUNC(coreinit, FSCloseDir) {
    std::lock_guard g(g_m);
    g_dirs.erase(arg(ctx, 2));
    status(ctx, kOk);
}
// FSStatus FSMakeDir(client, block, const char* path, FSErrorFlag)
HLE_FUNC(coreinit, FSMakeDir) {
    std::string path = guest_path(arg(ctx, 2));
    auto host = rt().vfs->resolve_for_write(path + "/.");
    if (!host) return trace_fs("FSMakeDir", path, status(ctx, kAccess)), void();
    std::error_code ec;
    if (stdfs::exists(host->parent_path(), ec) && rt().vfs->stat(path)) return trace_fs("FSMakeDir", path, status(ctx, kExists)), void();
    stdfs::create_directories(host->parent_path(), ec);
    trace_fs("FSMakeDir", path, status(ctx, ec ? kAccess : kOk));
}
// FSStatus FSRemove(client, block, const char* path, FSErrorFlag)
HLE_FUNC(coreinit, FSRemove) {
    std::string path = guest_path(arg(ctx, 2));
    auto host = rt().vfs->resolve_for_write(path);
    std::error_code ec;
    if (!host || !stdfs::exists(*host, ec)) return trace_fs("FSRemove", path, status(ctx, kNotFound)), void();
    stdfs::remove(*host, ec);
    trace_fs("FSRemove", path, status(ctx, ec ? kAccess : kOk));
}
// void FSTimeToCalendarTime(FSTime /* u64 microseconds since 2000 */, OSCalendarTime*): delegated to OSTicks... not
// needed yet; writes zeros.
HLE_FUNC(coreinit, FSTimeToCalendarTime) { std::memset(base + arg(ctx, 2), 0, 40); }

}  // namespace

void register_coreinit_fs(Runtime&) {
    HLE_REGISTER(coreinit, FSInit);
    HLE_REGISTER(coreinit, FSShutdown);
    HLE_REGISTER(coreinit, FSAddClient);
    HLE_REGISTER(coreinit, FSDelClient);
    HLE_REGISTER(coreinit, FSInitCmdBlock);
    HLE_REGISTER(coreinit, FSSetStateChangeNotification);
    HLE_REGISTER(coreinit, FSGetCurrentCmdBlock);
    HLE_REGISTER(coreinit, FSSetUserData);
    HLE_REGISTER(coreinit, FSGetUserData);
    HLE_REGISTER(coreinit, FSGetLastError);
    HLE_REGISTER(coreinit, FSGetLastErrorCodeForViewer);
    HLE_REGISTER(coreinit, FSOpenFile);
    HLE_REGISTER(coreinit, FSReadFile);
    HLE_REGISTER(coreinit, FSReadFileWithPos);
    HLE_REGISTER(coreinit, FSWriteFile);
    HLE_REGISTER(coreinit, FSSetPosFile);
    HLE_REGISTER(coreinit, FSGetStatFile);
    HLE_REGISTER(coreinit, FSFlushFile);
    HLE_REGISTER(coreinit, FSCloseFile);
    HLE_REGISTER(coreinit, FSGetStat);
    HLE_REGISTER(coreinit, FSOpenDir);
    HLE_REGISTER(coreinit, FSReadDir);
    HLE_REGISTER(coreinit, FSCloseDir);
    HLE_REGISTER(coreinit, FSMakeDir);
    HLE_REGISTER(coreinit, FSRemove);
    HLE_REGISTER(coreinit, FSTimeToCalendarTime);
}

std::vector<SaveRead> save_reads(const std::string& file_name) {
    std::lock_guard g(g_save_reads_m);
    auto it = g_save_reads.find(file_name);
    return it != g_save_reads.end() ? it->second : std::vector<SaveRead>{};
}

}  // namespace mh3u::os
