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
#include <mutex>
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

// nn_fp output sizes (Cemu iosu_fpd.h / iosu_fpd.cpp, commit d4f7205): MyPresence is SelfPresence, Profile is FPProfile
constexpr uint32_t kMyPresenceSize = 0x130, kProfileSize = 0x4;
constexpr uint32_t kScreenNameChars = 11;  // with the terminator; UTF-16BE
constexpr uint32_t kFriendPresenceSize = 0x34, kScreenNameSize = kScreenNameChars * 2, kAccountIdSize = 0x11,
                   kMiiSize = 0x60, kDateTimeSize = 8;
// ACTNexAuthenticationResult (Cemu nexServiceToken_t, nn_act.cpp, commit d4f7205)
constexpr uint32_t kNexTokenSize = 0x25C;
constexpr uint32_t kNexTokenToken = 0x000, kNexTokenTokenSize = 0x201;
constexpr uint32_t kNexTokenPassword = 0x204, kNexTokenPasswordSize = 0x41;
constexpr uint32_t kNexTokenHost = 0x248, kNexTokenHostSize = 0x10;
constexpr uint32_t kNexTokenPort = 0x258;

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

// "host:port" -> g_host_ip / g_port. false: not usable (the reason is printed).
bool resolve_server(const std::string& server) {
    const size_t colon = server.rfind(':');
    char* port_end = nullptr;
    const unsigned long port = colon == std::string::npos ? 0 : std::strtoul(server.c_str() + colon + 1, &port_end, 10);
    if (port == 0 || port > 0xFFFF || *port_end != '\0') {
        std::printf("online: online_server=%s is not host:port\n", server.c_str());
        return false;
    }
    g_port = static_cast<uint16_t>(port);
    const std::string host = server.substr(0, colon);
#ifdef _WIN32
    WSADATA wsa;
    if (const int err = WSAStartup(MAKEWORD(2, 2), &wsa); err != 0) {
        std::printf("online: WSAStartup failed (%d)\n", err);
        return false;
    }
#endif
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* res = nullptr;
    if (const int err = getaddrinfo(host.c_str(), nullptr, &hints, &res); err != 0) {
        std::printf("online: online_server host %s does not resolve (%s)\n", host.c_str(), gai_strerror(err));
        return false;
    }
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(res->ai_addr)->sin_addr, ip, sizeof ip);
    freeaddrinfo(res);
    g_host_ip = ip;
    return g_port != 0;
}

// A guest callback void(nn::Result, void* param), called a little later on a thread of its own, as the friends service
// answers on the console (Cemu: ServiceCallAsyncRespond). The game marks the request pending only after the async call
// returns, and its callback ignores an answer that comes before that. nn::Result is a class, so the game's compiler
// passes it by reference: r3 points to the value. The callbacks share one guest stack, so they run one at a time.
void call_result_callback_later(uint32_t fn, uint32_t param) {
    if (!fn) return;
    constexpr uint32_t kStackSize = 0x10000;
    static const uint32_t result = rt().mem->alloc_hle(4, 4);
    static const uint32_t stack = rt().mem->alloc_hle(kStackSize, 16);
    static std::mutex stack_in_use;
    std::thread([fn, param] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::lock_guard lock(stack_in_use);
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

void register_nn_act() {
    reg("nn_act", "HasNfsAccount__Q2_2nn3actFv", [](ppc::Context& c, uint8_t*) { ret(c, 1); });
    reg("nn_act", "GetPrincipalId__Q2_2nn3actFv", [](ppc::Context& c, uint8_t*) { ret(c, g_cfg.pid); });
    reg("nn_act", "GetPrincipalIdEx__Q2_2nn3actFPUiUc", [](ppc::Context& c, uint8_t* b) {
        if (arg(c, 0)) ppc::st32(b, arg(c, 0), g_cfg.pid);
        ret(c, kSuccess);
    });
    reg("nn_act", "GetAccountId__Q2_2nn3actFPc", [](ppc::Context& c, uint8_t* b) {
        if (arg(c, 0)) put_string(b, arg(c, 0), g_cfg.name, kAccountIdSize);
        ret(c, kSuccess);
    });
    // Result AcquireNexServiceToken(ACTNexAuthenticationResult* out, u32 game_server_id)
    reg("nn_act", "AcquireNexServiceToken__Q2_2nn3actFP26ACTNexAuthenticationResultUi", [](ppc::Context& c, uint8_t* b) {
        const uint32_t out = arg(c, 0);
        if (g_cfg.log)
            std::printf("online: NEX token for game server 0x%08X -> %s:%u pid %u\n", arg(c, 1), g_host_ip.c_str(), g_port, g_cfg.pid);
        std::memset(b + out, 0, kNexTokenSize);
        put_string(b, out + kNexTokenToken, "chacha", kNexTokenTokenSize);  // not checked by a Wii U NEX server
        put_string(b, out + kNexTokenPassword, g_cfg.password, kNexTokenPasswordSize);
        put_string(b, out + kNexTokenHost, g_host_ip, kNexTokenHostSize);
        ppc::st16(b, out + kNexTokenPort, g_port);
        ret(c, kSuccess);
    });
}

// Fills a getter's output with zeros, as Cemu's friends service does when it has no data: size bytes, or size per
// friend when count_arg names the friend count argument.
void zero_output(ppc::Context& c, uint8_t* b, uint32_t size, int count_arg = -1) {
    const uint32_t out = arg(c, 0);
    const uint32_t count = count_arg < 0 ? 1 : arg(c, count_arg);
    if (out) std::memset(b + out, 0, size_t{size} * count);
    ret(c, kSuccess);
}

template <uint32_t EntrySize>
void zero_friend_list(ppc::Context& c, uint8_t* b) {
    constexpr int kCountArg = 2;
    zero_output(c, b, EntrySize, kCountArg);
}

// The friends service: logged in, no friends.
void register_nn_fp() {
    for (const char* n : {"Initialize__Q2_2nn2fpFv", "Finalize__Q2_2nn2fpFv",
                          "SetNotificationHandler__Q2_2nn2fpFUiPFQ3_2nn2fp16NotificationTypeUiPv_vPv",
                          "UpdateGameMode__Q2_2nn2fpFPCQ3_2nn2fp8GameModePCw",
                          "AddRecentPlayRecord__Q2_2nn2fpFPCQ3_2nn2fp16RecentPlayRecordUi"})
        reg("nn_fp", n, [](ppc::Context& c, uint8_t*) { ret(c, kSuccess); });
    // u32 ResultToErrorCode(Result): 0, the game only shows it for a failed call and these do not fail
    reg("nn_fp", "ResultToErrorCode__Q2_2nn2fpFQ2_2nn6Result", [](ppc::Context& c, uint8_t*) { ret(c, 0); });
    // bool IsJoinable(const FriendPresence*, u64): there are no friends to join
    reg("nn_fp", "IsJoinable__Q2_2nn2fpFPCQ3_2nn2fp14FriendPresenceUL", [](ppc::Context& c, uint8_t*) { ret(c, 0); });
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
        if (arg(c, 0)) put_string(b, arg(c, 0), g_cfg.name, kAccountIdSize);
        ret(c, kSuccess);
    });
    reg("nn_fp", "GetMyScreenName__Q2_2nn2fpFPw", [](ppc::Context& c, uint8_t* b) {
        const uint32_t out = arg(c, 0);
        for (uint32_t i = 0; i < kScreenNameChars; ++i) {
            const bool in_name = i + 1 < kScreenNameChars && i < g_cfg.name.size();
            ppc::st16(b, out + 2 * i, in_name ? static_cast<uint8_t>(g_cfg.name[i]) : 0);
        }
        ret(c, kSuccess);
    });
    reg("nn_fp", "GetMyMii__Q2_2nn2fpFP12FFLStoreData", [](ppc::Context& c, uint8_t* b) { zero_output(c, b, kMiiSize); });
    // GetFriendList / GetBlackList(u32* pids, u32* count, u32 start, u32 max): none
    for (const char* n : {"GetFriendList__Q2_2nn2fpFPUiT1UiT3", "GetBlackList__Q2_2nn2fpFPUiT1UiT3"})
        reg("nn_fp", n, [](ppc::Context& c, uint8_t* b) {
            if (arg(c, 1)) ppc::st32(b, arg(c, 1), 0);
            ret(c, kSuccess);
        });
    reg("nn_fp", "GetMyPresence__Q2_2nn2fpFPQ3_2nn2fp10MyPresence",
        [](ppc::Context& c, uint8_t* b) { zero_output(c, b, kMyPresenceSize); });
    reg("nn_fp", "GetMyProfile__Q2_2nn2fpFPQ3_2nn2fp7Profile", [](ppc::Context& c, uint8_t* b) { zero_output(c, b, kProfileSize); });
    // per-friend getters (out, const u32* pids, u32 count, ...)
    reg("nn_fp", "GetFriendPresence__Q2_2nn2fpFPQ3_2nn2fp14FriendPresencePCUiUi", zero_friend_list<kFriendPresenceSize>);
    reg("nn_fp", "GetFriendProfile__Q2_2nn2fpFPQ3_2nn2fp7ProfilePCUiUi", zero_friend_list<kProfileSize>);
    reg("nn_fp", "GetFriendScreenName__Q2_2nn2fpFPA11_wPCUiUibPUc", zero_friend_list<kScreenNameSize>);
    reg("nn_fp", "GetFriendAccountId__Q2_2nn2fpFPA17_cPCUiUi", zero_friend_list<kAccountIdSize>);
    reg("nn_fp", "GetFriendMii__Q2_2nn2fpFP12FFLStoreDataPCUiUi", zero_friend_list<kMiiSize>);
    reg("nn_fp", "GetFriendApprovalTime__Q2_2nn2fpFPQ3_2nn2fp8DateTimePCUiUi", zero_friend_list<kDateTimeSize>);
    reg("nn_fp", "GetBlackListAccountId__Q2_2nn2fpFPA17_cPCUiUi", zero_friend_list<kAccountIdSize>);
    reg("nn_fp", "GetBlackListAdditionalTime__Q2_2nn2fpFPQ3_2nn2fp8DateTimePCUiUi", zero_friend_list<kDateTimeSize>);
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
    register_nn_ac_connected();
    register_nn_act();
    register_nn_fp();
    std::printf("online: NEX server %s:%u, pid %u\n", g_host_ip.c_str(), g_port, g_cfg.pid);
    return true;
}

}  // namespace mh3u::os
