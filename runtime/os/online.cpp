// Online play through a NEX server (docs/online.md, option A): with config.ini online_server set, the console is
// connected (nn_ac), the account has a network ID whose NEX service token names that server (nn_act), the friends
// service is logged in with no friends (nn_fp), and nsysnet uses real sockets (nsysnet.cpp). The game's own NEX
// client then logs in to the server with online_pid and online_password.
//
// The token layout (ACTNexAuthenticationResult, 0x25C bytes) is Cemu's nexServiceToken_t
// (src/Cafe/OS/libs/nn_act/nn_act.cpp, commit d4f7205). The approach (token straight to the server, friends logged in
// without a friends server) is mh3u-revival's Cemu fork (https://github.com/Matt-Wood-23/Cemu/tree/mh3u-revival, commit
// eef403b), made for its server https://github.com/Matt-Wood-23/mh3u-revival: online_server=<host>:1223,
// online_password=mh3u_revival_pw, any PID. PretendoNetwork/monster-hunter-3-ultimate works the same way, with the
// password its account service holds for the PID.
#include "hle.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#include <WinSock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#endif

namespace mh3u::os {
namespace {

constexpr uint32_t kSuccess = 0;

OnlineConfig g_cfg;
std::string g_host_ip;  // online_server's host, resolved to dotted IPv4
uint16_t g_port = 0;
bool g_fp_logged_in = false;

void reg(const char* lib, const char* name, ppc::GuestFunction fn) {
    ppc::register_import((std::string(lib) + "." + name).c_str(), fn);
}

void put_string(uint8_t* b, uint32_t a, const std::string& s, size_t room) {
    std::memset(b + a, 0, room);
    std::memcpy(b + a, s.data(), std::min(s.size(), room - 1));
}

// "host:port" -> g_host_ip / g_port. false: not usable.
bool resolve_server(const std::string& server) {
    const size_t colon = server.rfind(':');
    if (colon == std::string::npos) return false;
    const std::string host = server.substr(0, colon);
    g_port = static_cast<uint16_t>(std::atoi(server.c_str() + colon + 1));
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* res = nullptr;
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(res->ai_addr)->sin_addr, ip, sizeof ip);
    freeaddrinfo(res);
    g_host_ip = ip;
    return g_port != 0;
}

// A guest callback void(nn::Result, void* param), called a little later on a thread of its own, as the friends service
// answers on the console (Cemu: ServiceCallAsyncRespond). The game marks the request pending only after the async call
// returns, and its callback ignores an answer that comes before that. nn::Result is a class, so the game's compiler
// passes it by reference: r3 points to the value.
void call_result_callback_later(uint32_t fn, uint32_t param) {
    if (!fn) return;
    constexpr uint32_t kStackSize = 0x10000;
    static const uint32_t result = rt().mem->alloc_hle(4, 4);
    static const uint32_t stack = rt().mem->alloc_hle(kStackSize, 16);
    std::thread([fn, param] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ppc::st32(gbase(), result, kSuccess);
        ppc::Context c{};
        c.r[1] = (stack + kStackSize - 0x20) & ~0xFu;
        c.r[3] = result;
        c.r[4] = param;
        ppc::call_indirect(c, gbase(), fn);
    }).detach();
}

void log_call(const char* what) {
    if (g_cfg.log) std::printf("online: %s\n", what);
}

void register_nn_ac() {
    reg("nn_ac", "Connect__Q2_2nn2acFv", [](ppc::Context& c, uint8_t*) { ret(c, kSuccess); });
    reg("nn_ac", "IsApplicationConnected__Q2_2nn2acFPb", [](ppc::Context& c, uint8_t* b) {
        if (arg(c, 0)) b[arg(c, 0)] = 1;
        ret(c, kSuccess);
    });
    reg("nn_ac", "GetAssignedAddress__Q2_2nn2acFPUl", [](ppc::Context& c, uint8_t* b) {
        if (arg(c, 0)) ppc::st32(b, arg(c, 0), 0x7F000001);
        ret(c, kSuccess);
    });
    reg("nn_ac", "GetLastErrorCode__Q2_2nn2acFPUi", [](ppc::Context& c, uint8_t* b) {
        if (arg(c, 0)) ppc::st32(b, arg(c, 0), 0);
        ret(c, kSuccess);
    });
}

void register_nn_act() {
    reg("nn_act", "HasNfsAccount__Q2_2nn3actFv", [](ppc::Context& c, uint8_t*) { ret(c, 1); });
    reg("nn_act", "GetPrincipalId__Q2_2nn3actFv", [](ppc::Context& c, uint8_t*) { ret(c, g_cfg.pid); });
    reg("nn_act", "GetPrincipalIdEx__Q2_2nn3actFPUiUc", [](ppc::Context& c, uint8_t* b) {
        if (arg(c, 0)) ppc::st32(b, arg(c, 0), g_cfg.pid);
        ret(c, kSuccess);
    });
    reg("nn_act", "GetAccountId__Q2_2nn3actFPc", [](ppc::Context& c, uint8_t* b) {
        if (arg(c, 0)) put_string(b, arg(c, 0), g_cfg.name, 17);
        ret(c, kSuccess);
    });
    // Result AcquireNexServiceToken(ACTNexAuthenticationResult* out, u32 game_server_id)
    reg("nn_act", "AcquireNexServiceToken__Q2_2nn3actFP26ACTNexAuthenticationResultUi", [](ppc::Context& c, uint8_t* b) {
        const uint32_t out = arg(c, 0);
        if (g_cfg.log)
            std::printf("online: NEX token for game server 0x%08X -> %s:%u pid %u\n", arg(c, 1), g_host_ip.c_str(), g_port, g_cfg.pid);
        std::memset(b + out, 0, 0x25C);
        put_string(b, out + 0x000, "chacha", 0x201);  // token: not checked by a Wii U NEX server
        put_string(b, out + 0x204, g_cfg.password, 0x41);
        put_string(b, out + 0x248, g_host_ip, 0x10);
        ppc::st16(b, out + 0x258, g_port);
        ret(c, kSuccess);
    });
}

// The friends service: logged in, no friends. Calls are logged so the first runs show what the game asks for.
void register_nn_fp() {
    for (const char* n : {"Initialize__Q2_2nn2fpFv", "Finalize__Q2_2nn2fpFv",
                          "SetNotificationHandler__Q2_2nn2fpFUiPFQ3_2nn2fp16NotificationTypeUiPv_vPv",
                          "UpdateGameMode__Q2_2nn2fpFPCQ3_2nn2fp8GameModePCw",
                          "AddRecentPlayRecord__Q2_2nn2fpFPCQ3_2nn2fp16RecentPlayRecordUi",
                          "ResultToErrorCode__Q2_2nn2fpFQ2_2nn6Result", "IsJoinable__Q2_2nn2fpFPCQ3_2nn2fp14FriendPresenceUL"})
        reg("nn_fp", n, [](ppc::Context& c, uint8_t*) { ret(c, kSuccess); });
    reg("nn_fp", "Logout__Q2_2nn2fpFv", [](ppc::Context& c, uint8_t*) {
        g_fp_logged_in = false;
        ret(c, kSuccess);
    });
    reg("nn_fp", "IsInitialized__Q2_2nn2fpFv", [](ppc::Context& c, uint8_t*) { ret(c, 1); });
    reg("nn_fp", "HasLoggedIn__Q2_2nn2fpFv", [](ppc::Context& c, uint8_t*) { ret(c, g_fp_logged_in ? 1 : 0); });
    for (const char* n : {"LoginAsync__Q2_2nn2fpFPFQ2_2nn6ResultPv_vPv", "RegisterAccountAsync__Q2_2nn2fpFPFQ2_2nn6ResultPv_vPv"})
        reg("nn_fp", n, [](ppc::Context& c, uint8_t*) {
            log_call("nn_fp login");
            g_fp_logged_in = true;
            call_result_callback_later(arg(c, 0), arg(c, 1));
            ret(c, kSuccess);
        });
    reg("nn_fp", "GetMyPrincipalId__Q2_2nn2fpFv", [](ppc::Context& c, uint8_t*) { ret(c, g_cfg.pid); });
    reg("nn_fp", "GetMyAccountId__Q2_2nn2fpFPc", [](ppc::Context& c, uint8_t* b) {
        if (arg(c, 0)) put_string(b, arg(c, 0), g_cfg.name, 17);
        ret(c, kSuccess);
    });
    // GetMyScreenName(wchar_t out[11]): UTF-16BE
    reg("nn_fp", "GetMyScreenName__Q2_2nn2fpFPw", [](ppc::Context& c, uint8_t* b) {
        const uint32_t out = arg(c, 0);
        for (size_t i = 0; i < 11; ++i) ppc::st16(b, out + 2 * static_cast<uint32_t>(i), i < 10 && i < g_cfg.name.size() ? g_cfg.name[i] : 0);
        ret(c, kSuccess);
    });
    reg("nn_fp", "GetMyMii__Q2_2nn2fpFP12FFLStoreData", [](ppc::Context& c, uint8_t* b) {
        if (arg(c, 0)) std::memset(b + arg(c, 0), 0, 0x60);
        ret(c, kSuccess);
    });
    // GetFriendList / GetBlackList(u32* pids, u32* count, u32 start, u32 max): none
    for (const char* n : {"GetFriendList__Q2_2nn2fpFPUiT1UiT3", "GetBlackList__Q2_2nn2fpFPUiT1UiT3"})
        reg("nn_fp", n, [](ppc::Context& c, uint8_t* b) {
            if (arg(c, 1)) ppc::st32(b, arg(c, 1), 0);
            ret(c, kSuccess);
        });
    // the rest: not expected with an empty friend list; logged
    for (const char* n : {"GetMyPresence__Q2_2nn2fpFPQ3_2nn2fp10MyPresence", "GetMyProfile__Q2_2nn2fpFPQ3_2nn2fp7Profile",
                          "GetFriendPresence__Q2_2nn2fpFPQ3_2nn2fp14FriendPresencePCUiUi",
                          "GetFriendProfile__Q2_2nn2fpFPQ3_2nn2fp7ProfilePCUiUi",
                          "GetFriendScreenName__Q2_2nn2fpFPA11_wPCUiUibPUc",
                          "GetFriendAccountId__Q2_2nn2fpFPA17_cPCUiUi", "GetFriendMii__Q2_2nn2fpFP12FFLStoreDataPCUiUi",
                          "GetFriendApprovalTime__Q2_2nn2fpFPQ3_2nn2fp8DateTimePCUiUi",
                          "GetBlackListAccountId__Q2_2nn2fpFPA17_cPCUiUi",
                          "GetBlackListAdditionalTime__Q2_2nn2fpFPQ3_2nn2fp8DateTimePCUiUi"})
        reg("nn_fp", n, [](ppc::Context& c, uint8_t*) {
            log_call("nn_fp getter (no data written)");
            ret(c, kSuccess);
        });
}

}  // namespace

bool register_online(Runtime& r, const OnlineConfig& cfg) {
    g_cfg = cfg;
    if (g_cfg.server.empty()) return false;
    if (!resolve_server(g_cfg.server)) {
        std::printf("online: online_server=%s is not host:port or does not resolve; staying offline\n", g_cfg.server.c_str());
        return false;
    }
    if (g_cfg.name.empty()) g_cfg.name = "ChaCha";
    register_nsysnet(r, g_cfg.log, g_host_ip);
    register_nn_ac();
    register_nn_act();
    register_nn_fp();
    std::printf("online: NEX server %s:%u, pid %u\n", g_host_ip.c_str(), g_port, g_cfg.pid);
    return true;
}

}  // namespace mh3u::os
