// nn_save: save-data paths on top of the FS HLE. Account slot 0xFF = common data (/vol/save/common), otherwise the
// account's persistent id (/vol/save/80000001 for the single local account, see nn_act.cpp).
// Each SAVE* call builds the full guest path and forwards to the matching FS* implementation.
#include "hle.h"
#include "overlay_fs.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace mh3u::os {
namespace {

uint32_t g_path_buf = 0;  // guest scratch for the forwarded path (single guest thread for now)

uint32_t full_path(uint8_t* base, uint32_t slot, uint32_t rel) {
    std::string p = slot == 0xFF ? "/vol/save/common" : "/vol/save/80000001";
    std::string r = guest_cstr(rel);
    if (!r.empty() && r[0] != '/') p += '/';
    p += r;
    std::memcpy(base + g_path_buf, p.c_str(), std::min<size_t>(p.size() + 1, 511));
    base[g_path_buf + 511] = 0;
    return g_path_buf;
}

void forward(const char* fs_name, ppc::Context& ctx, uint8_t* base) {
    static_cast<void>(base);
    ppc::GuestFunction fn = ppc::resolve_import(fs_name);
    if (!fn) ppc::unimplemented_import(fs_name);
    fn(ctx, base);
}

HLE_FUNC(nn_save, SAVEInit) { ret(ctx, 0); }
HLE_FUNC(nn_save, SAVEShutdown) {}
// SAVEStatus SAVEInitSaveDir(u8 slot): make sure the account's directory exists
HLE_FUNC(nn_save, SAVEInitSaveDir) {
    std::string dir = arg(ctx, 0) == 0xFF ? "/vol/save/common/." : "/vol/save/80000001/.";
    rt().vfs->resolve_for_write(dir);  // creates the parent directories
    ret(ctx, 0);
}
// SAVEOpenFile(client, block, u8 slot, const char* path, const char* mode, FSFileHandle*, FSErrorFlag)
//   -> FSOpenFile(client, block, path, mode, FSFileHandle*, FSErrorFlag)
HLE_FUNC(nn_save, SAVEOpenFile) {
    ctx.r[5] = full_path(base, arg(ctx, 2), arg(ctx, 3));
    ctx.r[6] = ctx.r[7];
    ctx.r[7] = ctx.r[8];
    ctx.r[8] = ctx.r[9];
    forward("coreinit.FSOpenFile", ctx, base);
}
// SAVEGetStat(client, block, u8 slot, const char* path, FSStat*, FSErrorFlag) -> FSGetStat(client, block, path, stat, err)
HLE_FUNC(nn_save, SAVEGetStat) {
    ctx.r[5] = full_path(base, arg(ctx, 2), arg(ctx, 3));
    ctx.r[6] = ctx.r[7];
    ctx.r[7] = ctx.r[8];
    forward("coreinit.FSGetStat", ctx, base);
}
// SAVEMakeDir(client, block, u8 slot, const char* path, FSErrorFlag) -> FSMakeDir(client, block, path, err)
HLE_FUNC(nn_save, SAVEMakeDir) {
    ctx.r[5] = full_path(base, arg(ctx, 2), arg(ctx, 3));
    ctx.r[6] = ctx.r[7];
    forward("coreinit.FSMakeDir", ctx, base);
}
// SAVERemove(client, block, u8 slot, const char* path, FSErrorFlag) -> FSRemove(client, block, path, err)
HLE_FUNC(nn_save, SAVERemove) {
    ctx.r[5] = full_path(base, arg(ctx, 2), arg(ctx, 3));
    ctx.r[6] = ctx.r[7];
    forward("coreinit.FSRemove", ctx, base);
}
// SAVEGetFreeSpaceSize(client, block, u8 slot, u64* out, FSErrorFlag): report 64 MB free
HLE_FUNC(nn_save, SAVEGetFreeSpaceSize) {
    if (arg(ctx, 3)) ppc::st64(base, arg(ctx, 3), 64ull << 20);
    ret(ctx, 0);
}
HLE_FUNC(nn_save, SAVEFlushQuota) { ret(ctx, 0); }

// proc_ui: the app is always in the foreground; no HOME-menu / power events yet.
HLE_FUNC(proc_ui, ProcUIInit) {}
HLE_FUNC(proc_ui, ProcUIShutdown) {}
HLE_FUNC(proc_ui, ProcUIRegisterCallback) {}
HLE_FUNC(proc_ui, ProcUISetMEM1Storage) {}
HLE_FUNC(proc_ui, ProcUISetBucketStorage) {}
HLE_FUNC(proc_ui, ProcUIDrawDoneRelease) {}
// ProcUIStatus ProcUIProcessMessages(BOOL block): 0 = PROCUI_STATUS_IN_FOREGROUND
HLE_FUNC(proc_ui, ProcUIProcessMessages) { ret(ctx, 0); }

}  // namespace

void register_nn_save(Runtime& r) {
    g_path_buf = r.mem->alloc_hle(512, 4);
    HLE_REGISTER(nn_save, SAVEInit);
    HLE_REGISTER(nn_save, SAVEShutdown);
    HLE_REGISTER(nn_save, SAVEInitSaveDir);
    HLE_REGISTER(nn_save, SAVEOpenFile);
    HLE_REGISTER(nn_save, SAVEGetStat);
    HLE_REGISTER(nn_save, SAVEMakeDir);
    HLE_REGISTER(nn_save, SAVERemove);
    HLE_REGISTER(nn_save, SAVEGetFreeSpaceSize);
    HLE_REGISTER(nn_save, SAVEFlushQuota);
    HLE_REGISTER(proc_ui, ProcUIInit);
    HLE_REGISTER(proc_ui, ProcUIShutdown);
    HLE_REGISTER(proc_ui, ProcUIRegisterCallback);
    HLE_REGISTER(proc_ui, ProcUISetMEM1Storage);
    HLE_REGISTER(proc_ui, ProcUISetBucketStorage);
    HLE_REGISTER(proc_ui, ProcUIDrawDoneRelease);
    HLE_REGISTER(proc_ui, ProcUIProcessMessages);
}

}  // namespace mh3u::os
