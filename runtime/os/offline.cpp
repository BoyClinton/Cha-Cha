// Peripherals and network services as on an offline console with nothing attached:
//   nsyshid / mic / usb_mic: no devices; nsysnet / nlibcurl / NSSL: no network (sockets and handles fail);
//   nn_ac: not connected; nn_uds (local wireless): unavailable; nn_ndm: accepted; nn_acp network time: unavailable;
//   sysapp launches: ignored; h264: see h264.cpp. dmae copies are real.
#include "hle.h"

#include <cstring>
#include <string>

namespace mh3u::os {
namespace {

constexpr uint32_t kFail = 0xFFFFFFFF;        // -1: error / invalid handle
constexpr uint32_t kResultFail = 0xA0B12C80;  // failing nn::Result (bit 31 set); exact value assumed

void ret0(ppc::Context& c, uint8_t*) { ret(c, 0); }
void ret_fail(ppc::Context& c, uint8_t*) { ret(c, kFail); }
void ret_result_fail(ppc::Context& c, uint8_t*) { ret(c, kResultFail); }
void identity(ppc::Context&, uint8_t*) {}  // r3 already holds the argument (big-endian guest = network order)

uint32_t guest_string(const char* s) {
    uint32_t a = rt().mem->alloc_hle(static_cast<uint32_t>(std::strlen(s) + 1), 4);
    std::memcpy(gbase() + a, s, std::strlen(s) + 1);
    return a;
}
uint32_t g_zero_ip = 0, g_curl_offline = 0;

void reg(const std::string& lib, const char* name, ppc::GuestFunction fn) {
    ppc::register_import((lib + "." + name).c_str(), fn);
}

// u64 DMAECopyMem(void* dst, const void* src, u32 words, DMAEEndianSwapMode) -> timestamp; 1 = swap16, 2 = swap32
void dmae_copy(ppc::Context& c, uint8_t* b) {
    const uint32_t dst = arg(c, 0), src = arg(c, 1), words = arg(c, 2), mode = arg(c, 3);
    std::memmove(b + dst, b + src, size_t{words} * 4);
    if (mode == 1)
        for (uint32_t i = 0; i < words * 2; ++i) ppc::st16(b, dst + 2 * i, ppc::ld16_le(b, dst + 2 * i));
    else if (mode == 2)
        for (uint32_t i = 0; i < words; ++i) ppc::st32(b, dst + 4 * i, ppc::ld32_le(b, dst + 4 * i));
    static uint32_t stamp = 0;
    c.r[3] = 0;
    c.r[4] = ++stamp;
}

}  // namespace

void register_offline(Runtime&) {
    g_zero_ip = guest_string("0.0.0.0");
    g_curl_offline = guest_string("network unavailable (offline)");

    // nsyshid: setup works, no device ever attaches
    for (const char* n : {"HIDSetup", "HIDTeardown", "HIDAddClient", "HIDDelClient"}) reg("nsyshid", n, ret0);
    for (const char* n : {"HIDGetIdle", "HIDRead", "HIDSetIdle", "HIDSetProtocol", "HIDSetReport"}) reg("nsyshid", n, ret_fail);
    // microphones: none
    for (const char* n : {"MICClose", "MICGetState", "MICGetStatus", "MICInit", "MICOpen", "MICSetDataConsumed",
                          "MICSetState", "MICUninit"})
        reg("mic", n, ret_fail);
    for (const char* n : {"USBMICClose", "USBMICGetState", "USBMICGetStatus", "USBMICInit", "USBMICOpen",
                          "USBMICSetDataConsumed", "USBMICSetState", "USBMICStart", "USBMICStop"})
        reg("usb_mic", n, ret_fail);
    // dmae
    reg("dmae", "DMAECopyMem", dmae_copy);
    reg("dmae", "DMAEWaitDone", [](ppc::Context& c, uint8_t*) { ret(c, 1); });
    // h264: see h264.cpp
    // nsysnet: library initialises, no sockets
    for (const char* n : {"socket_lib_init", "socket_lib_finish", "set_resolver_allocator", "NSSLInit", "NSSLFinish"})
        reg("nsysnet", n, ret0);
    for (const char* n : {"socket", "bind", "connect", "recv", "recvfrom", "select", "send", "sendto", "setsockopt",
                          "shutdown", "socketclose", "getsockname", "NSSLCreateContext", "NSSLDestroyContext",
                          "NSSLAddServerPKI", "NSSLAddServerPKIGroups"})
        reg("nsysnet", n, ret_fail);
    for (const char* n : {"htonl", "htons", "ntohl", "ntohs"}) reg("nsysnet", n, identity);
    reg("nsysnet", "socketlasterr", [](ppc::Context& c, uint8_t*) { ret(c, 50); });  // ENETDOWN
    reg("nsysnet", "gethostbyname", ret0);                                         // NULL
    reg("nsysnet", "inet_pton", ret0);
    reg("nsysnet", "inet_ntoa", [](ppc::Context& c, uint8_t*) { ret(c, g_zero_ip); });
    // nlibcurl: global init succeeds, no handles
    for (const char* n : {"curl_global_init", "curl_global_init_mem", "curl_global_cleanup", "curl_easy_cleanup",
                          "curl_easy_reset", "curl_multi_cleanup", "curl_share_cleanup", "curl_slist_free_all"})
        reg("nlibcurl", n, ret0);
    for (const char* n : {"curl_easy_init", "curl_multi_init", "curl_share_init", "curl_slist_append",
                          "curl_multi_info_read"})
        reg("nlibcurl", n, ret0);  // NULL
    for (const char* n : {"curl_easy_perform", "curl_easy_setopt", "curl_easy_getinfo", "curl_easy_pause",
                          "curl_multi_add_handle", "curl_multi_remove_handle", "curl_multi_perform", "curl_multi_fdset",
                          "curl_multi_timeout", "curl_share_setopt"})
        reg("nlibcurl", n, [](ppc::Context& c, uint8_t*) { ret(c, 7); });  // CURLE_COULDNT_CONNECT
    reg("nlibcurl", "curl_easy_strerror", [](ppc::Context& c, uint8_t*) { ret(c, g_curl_offline); });
    // nn_ac: initialises, never connected
    for (const char* n : {"Initialize__Q2_2nn2acFv", "Finalize__Q2_2nn2acFv", "Close__Q2_2nn2acFv"}) reg("nn_ac", n, ret0);
    for (const char* n : {"Connect__Q2_2nn2acFv", "GetAssignedAddress__Q2_2nn2acFPUl", "GetAssignedSubnet__Q2_2nn2acFPUl",
                          "GetMacAddress__Q2_2nn2acFPQ3_2nn2ac10MacAddress",
                          "GetNetworkInterface__Q2_2nn2acFPC16netconf_profile_",
                          "ReadConfig__Q2_2nn2acFQ3_2nn2ac11ConfigIdNumP16netconf_profile_"})
        reg("nn_ac", n, ret_result_fail);
    reg("nn_ac", "IsApplicationConnected__Q2_2nn2acFPb", [](ppc::Context& c, uint8_t* b) {
        if (arg(c, 0)) b[arg(c, 0)] = 0;
        ret(c, 0);
    });
    reg("nn_ac", "IsConfigExisting__Q2_2nn2acFQ3_2nn2ac11ConfigIdNumPb", [](ppc::Context& c, uint8_t* b) {
        if (arg(c, 1)) b[arg(c, 1)] = 0;
        ret(c, 0);
    });
    reg("nn_ac", "GetStartupId__Q2_2nn2acFPQ3_2nn2ac11ConfigIdNum", [](ppc::Context& c, uint8_t* b) {
        if (arg(c, 0)) ppc::st32(b, arg(c, 0), 0);
        ret(c, 0);
    });
    reg("nn_ac", "GetLastErrorCode__Q2_2nn2acFPUi", [](ppc::Context& c, uint8_t* b) {
        if (arg(c, 0)) ppc::st32(b, arg(c, 0), 1159908);  // "not connected" style viewer code (assumed)
        ret(c, 0);
    });
    // nn_ndm: daemon control accepted
    for (const char* n : {"EnableConcurrentConnection__Q2_2nn3ndmFv", "Finalize__Q2_2nn3ndmFv", "Initialize__Q2_2nn3ndmFv",
                          "ResumeDaemons__Q2_2nn3ndmFUi", "SuspendDaemons__Q2_2nn3ndmFUi"})
        reg("nn_ndm", n, ret0);
    // nn_acp network time: unavailable
    reg("nn_acp", "ACPGetNetworkTime", ret_fail);
    reg("nn_acp", "ACPConvertNetworkTimeToOSCalendarTime", ret_fail);
    // sysapp: launching system settings / account apps is ignored
    reg("sysapp", "SYSLaunchSettings", ret0);
    reg("sysapp", "SYSLaunchAccount", ret0);
    // nn_uds (local wireless): every call fails (the static initialiser is a no-op registered by the boot code)
    for (const char* n : {
             "AddOutsideClient__Q3_2nn3uds4CafeFRQ4_2nn3uds4Cafe24NodeInformationForBridge",
             "AllowToConnect__Q3_2nn3uds4CafeFv",
             "AttachForBridge__Q3_2nn3uds4CafeFPQ4_2nn3uds4Cafe18EndpointDescriptor",
             "Attach__Q3_2nn3uds4CafeFPQ4_2nn3uds4Cafe18EndpointDescriptorUsUcUi",
             "CreateBridgeNetwork__Q3_2nn3uds4CafeFPCQ4_2nn3uds4Cafe24BridgeNetworkInformationUc",
             "CreateEndpoint__Q3_2nn3uds4CafeFPQ4_2nn3uds4Cafe18EndpointDescriptor",
             "CreateLocalCommunicationId__Q3_2nn3uds4CafeFUib",
             "CreateNetwork__Q3_2nn3uds4CafeFUcT1UiPCcT3T1",
             "DeleteOutsideClient__Q3_2nn3uds4CafeFUs",
             "DestroyBridgeNetwork__Q3_2nn3uds4CafeFv",
             "DestroyEndpoint__Q3_2nn3uds4CafeFPQ4_2nn3uds4Cafe18EndpointDescriptor",
             "DestroyNetwork__Q3_2nn3uds4CafeFv",
             "DisallowToConnect__Q3_2nn3uds4CafeFb",
             "EjectClient__Q3_2nn3uds4CafeFUs",
             "Finalize__Q3_2nn3uds4CafeFv",
             "GetApplicationDataFromBeacon__Q3_2nn3uds4CafeFPvPUiUi",
             "GetConnectionStatusForBridge__Q3_2nn3uds4CafeFPQ4_2nn3uds4Cafe25ConnectionStatusForBridge",
             "GetConnectionStatus__Q3_2nn3uds4CafeFPQ4_2nn3uds4Cafe16ConnectionStatus",
             "GetNodeInformationForBridge__Q3_2nn3uds4CafeFPQ4_2nn3uds4Cafe24NodeInformationForBridgeUs",
             "GetNodeInformation__Q3_2nn3uds4CafeFPQ4_2nn3uds4Cafe15NodeInformationUs",
             "Initialize__Q3_2nn3uds4CafeFPvUi",
             "PollStateChange__Q3_2nn3uds4CafeFUc",
             "ReceiveBridgedPacket__Q3_2nn3uds4CafeFRCQ4_2nn3uds4Cafe18EndpointDescriptorPvPUiPUsT4PUcUiUc",
             "ReceiveFrom__Q3_2nn3uds4CafeFRCQ4_2nn3uds4Cafe18EndpointDescriptorPvPUiPUsUiUc",
             "SendBridgedPacket__Q3_2nn3uds4CafeFQ4_2nn3uds4Cafe18EndpointDescriptorPCvUiUsT4UcT6",
             "SendTo__Q3_2nn3uds4CafeFRCQ4_2nn3uds4Cafe18EndpointDescriptorPCvUiUsUcT5",
             "SetApplicationDataToBeacon__Q3_2nn3uds4CafeFPCvUi"})
        reg("nn_uds", n, ret_result_fail);
}

}  // namespace mh3u::os
