// nn_act (accounts): one local account in slot 1 without a Nintendo Network ID – an offline console.
// nn::Result is returned in r3: success when bit 31 is clear. Server-backed calls fail with kOfflineResult.
#include "hle.h"

#include <cstring>

namespace mh3u::os {
namespace {

constexpr uint32_t kSuccess = 0;
constexpr uint32_t kOfflineResult = 0xA0B12C80;  // a failure Result (bit 31 set); exact module/description assumed
constexpr uint8_t kSlot = 1;

void reg(const char* mangled, ppc::GuestFunction fn) {
    ppc::register_import((std::string("nn_act.") + mangled).c_str(), fn);
}

}  // namespace

void register_nn_act(Runtime&) {
    reg("Initialize__Q2_2nn3actFv", [](ppc::Context& c, uint8_t*) { ret(c, kSuccess); });
    reg("Finalize__Q2_2nn3actFv", [](ppc::Context& c, uint8_t*) { ret(c, kSuccess); });
    reg("GetSlotNo__Q2_2nn3actFv", [](ppc::Context& c, uint8_t*) { ret(c, kSlot); });
    reg("GetNumOfAccounts__Q2_2nn3actFv", [](ppc::Context& c, uint8_t*) { ret(c, 1); });
    reg("IsSlotOccupied__Q2_2nn3actFUc", [](ppc::Context& c, uint8_t*) { ret(c, arg(c, 0) == kSlot ? 1 : 0); });
    reg("GetPersistentId__Q2_2nn3actFv", [](ppc::Context& c, uint8_t*) { ret(c, 0x80000001); });
    reg("GetPrincipalId__Q2_2nn3actFv", [](ppc::Context& c, uint8_t*) { ret(c, 0); });  // no NNID
    reg("GetPrincipalIdEx__Q2_2nn3actFPUiUc", [](ppc::Context& c, uint8_t* b) {
        if (arg(c, 0)) ppc::st32(b, arg(c, 0), 0);
        ret(c, kSuccess);
    });
    reg("HasNfsAccount__Q2_2nn3actFv", [](ppc::Context& c, uint8_t*) { ret(c, 0); });
    // Result GetAccountId(char* out /* 17 bytes */): no NNID -> empty string
    reg("GetAccountId__Q2_2nn3actFPc", [](ppc::Context& c, uint8_t* b) {
        if (arg(c, 0)) b[arg(c, 0)] = 0;
        ret(c, kSuccess);
    });
    // u64 GetTransferableId(u32 unique_id) -> r3:r4
    reg("GetTransferableId__Q2_2nn3actFUi", [](ppc::Context& c, uint8_t*) {
        c.r[3] = 0x00000000;
        c.r[4] = 0x4D483355;  // arbitrary but stable per install
    });
    // Result GetMii(FFLStoreData* out /* 0x60 bytes */): zeroed – the game has to cope with a blank Mii
    reg("GetMii__Q2_2nn3actFP12FFLStoreData", [](ppc::Context& c, uint8_t* b) {
        if (arg(c, 0)) std::memset(b + arg(c, 0), 0, 0x60);
        ret(c, kSuccess);
    });
    reg("GetErrorCode__Q2_2nn3actFRCQ2_2nn6Result", [](ppc::Context& c, uint8_t*) { ret(c, 1022500); });
    // server-backed: fail as when offline
    reg("GetNfsPasswordEx__Q2_2nn3actFPcUc", [](ppc::Context& c, uint8_t*) { ret(c, kOfflineResult); });
    reg("GetHostServerSettings__Q2_2nn3actFP11ACTNnasTypeP10ACTNfsTypePUcUc",
        [](ppc::Context& c, uint8_t*) { ret(c, kOfflineResult); });
    reg("AcquireNexServiceToken__Q2_2nn3actFP26ACTNexAuthenticationResultUi",
        [](ppc::Context& c, uint8_t*) { ret(c, kOfflineResult); });
    reg("AcquireIndependentServiceToken__Q2_2nn3actFPcPCc", [](ppc::Context& c, uint8_t*) { ret(c, kOfflineResult); });
}

}  // namespace mh3u::os
