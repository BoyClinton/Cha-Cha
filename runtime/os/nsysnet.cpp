// nsysnet: BSD sockets on the host's network, for online play (NEX over UDP). Registered after offline.cpp, whose
// stubs it replaces, only when online play is configured (online.cpp).
//
// Ported from Cemu (https://github.com/cemu-project/Cemu, src/Cafe/OS/libs/nsysnet/nsysnet.cpp and
// src/Common/socket.h, commit d4f7205), MPL-2.0: the Cafe OS constants, the socket handle table, the fd_set bit mask,
// the error translation and the blocking and non-blocking paths. Changed for Cha-Cha: every guest thread is a host
// thread, so a blocking call waits on the host socket (in short select() slices, so the game can still exit), and the
// error for socketlasterr is kept per thread.
//
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL was not
// distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "hle.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#ifdef _WIN32
#include <WinSock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
#define GETLASTERR WSAGetLastError()
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#define WSAEWOULDBLOCK EWOULDBLOCK
#define WSAEINPROGRESS EINPROGRESS
#define WSAESHUTDOWN ESHUTDOWN
#define WSAECONNABORTED ECONNABORTED
#define WSAENOTCONN ENOTCONN
#define GETLASTERR errno
using SOCKET = int;
#define closesocket close
#define SOCKET_ERROR (-1)
#define INVALID_SOCKET (-1)
#endif

namespace mh3u::os {
namespace {

// Cafe OS values (Cemu)
constexpr int kAfInet = 2;
constexpr int kSockStream = 1, kSockDgram = 2;
constexpr int kIpprotoIp = 0, kIpprotoTcp = 6, kIpprotoUdp = 17;
constexpr int kSolSocket = -1;
constexpr int kSoReuseAddr = 0x0004, kSoBroadcast = 0x0020, kSoSndBuf = 0x1001, kSoRcvBuf = 0x1002,
              kSoNbio = 0x1014, kSoBio = 0x1015, kSoNonBlock = 0x1016;
constexpr int kTcpNoDelay = 0x2004;
constexpr int kMsgPeek = 0x02, kMsgDontWait = 0x20;
constexpr int kErrSuccess = 0x00, kErrWouldBlock = 0x06, kErrNotConn = 0x09, kErrInval = 0x0B, kErrShutdown = 0x0F,
              kErrInProgress = 0x16, kErrAfNoSupport = 0x21, kErrNotInitialised = 0x2B, kErrUnknown = 99999;

// A guest handle is 1..kSocketLimit; select's fd_set is one big-endian u32 with bit n = handle n, so 31 at most.
constexpr int kSocketLimit = 31;
constexpr int kWaitSliceMs = 10;

struct VirtualSocket {
    SOCKET s = INVALID_SOCKET;
    int type = 0;
    bool non_blocking = false;
    bool shutdown_recv = false;
};

std::mutex g_lock;
VirtualSocket* g_sockets[kSocketLimit] = {};
bool g_lib_ready = false;
bool g_log = false;
std::string g_nat_check_host;  // where the game's NAT check (nncs*.app.nintendowifi.net) goes instead
thread_local int t_last_error = kErrSuccess;

VirtualSocket* find(int handle) {
    std::lock_guard lk(g_lock);
    return handle >= 1 && handle <= kSocketLimit ? g_sockets[handle - 1] : nullptr;
}

void set_nonblocking(SOCKET s, bool on) {
#ifdef _WIN32
    u_long mode = on ? 1 : 0;
    ioctlsocket(s, FIONBIO, &mode);
#else
    const int flags = fcntl(s, F_GETFL);
    fcntl(s, F_SETFL, on ? flags | O_NONBLOCK : flags & ~O_NONBLOCK);
#endif
}

// Cemu's _translateError: 0 / >= 0 is success, else the host error becomes the Cafe OS one.
int fail_from_host(int host_error, bool connecting = false) {
    switch (host_error) {
    case 0: t_last_error = kErrSuccess; break;
    case WSAEWOULDBLOCK: t_last_error = connecting ? kErrInProgress : kErrWouldBlock; break;
    case WSAEINPROGRESS: t_last_error = kErrInProgress; break;
    case WSAESHUTDOWN: t_last_error = kErrShutdown; break;
    case WSAENOTCONN: t_last_error = kErrNotConn; break;
    default: t_last_error = kErrUnknown;
    }
    return -1;
}

// The guest sockaddr is the BSD one with a big-endian family; the port and address are network order in both.
sockaddr guest_to_host_addr(const uint8_t* b, uint32_t a) {
    sockaddr h{};
    h.sa_family = ppc::ld16(b, a);
    std::memcpy(h.sa_data, b + a + 2, 14);
    return h;
}
void host_to_guest_addr(uint8_t* b, uint32_t a, const sockaddr& h) {
    ppc::st16(b, a, h.sa_family);
    std::memcpy(b + a + 2, h.sa_data, 14);
}

std::string addr_text(const sockaddr& h) {
    const auto* d = reinterpret_cast<const uint8_t*>(h.sa_data);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%u.%u.%u.%u:%u", d[2], d[3], d[4], d[5], (d[0] << 8) | d[1]);
    return buf;
}

void log_bytes(const char* what, int handle, const uint8_t* p, int n, const sockaddr* peer) {
    if (!g_log || n <= 0) return;
    std::printf("nsysnet: %s %d %d bytes%s%s:", what, handle, n, peer ? " " : "", peer ? addr_text(*peer).c_str() : "");
    for (int i = 0; i < n && i < 48; ++i) std::printf(" %02x", p[i]);
    std::printf(n > 48 ? " ...\n" : "\n");
}

// Waits until s is readable (or writable), in slices so a closed socket or the game's exit ends the wait.
// true: ready; false: the socket went away.
bool wait_ready(int handle, SOCKET s, bool for_write) {
    for (;;) {
        if (rt().exit_requested.load() || !find(handle) || find(handle)->s != s) return false;
        fd_set set;
        FD_ZERO(&set);
        FD_SET(s, &set);
        timeval tv{0, kWaitSliceMs * 1000};
        const int r = select(static_cast<int>(s) + 1, for_write ? nullptr : &set, for_write ? &set : nullptr, nullptr, &tv);
        if (r != 0) return true;  // ready, or an error the following call reports
    }
}

// int socket(int family, int type, int protocol)
void sock_socket(ppc::Context& c, uint8_t*) {
    const int family = static_cast<int>(arg(c, 0)), type = static_cast<int>(arg(c, 1)),
              protocol = static_cast<int>(arg(c, 2));
    if (!g_lib_ready) {
        t_last_error = kErrNotInitialised;
        return ret(c, ~0u);
    }
    if (family != kAfInet || (type != kSockStream && type != kSockDgram) ||
        (protocol != kIpprotoTcp && protocol != kIpprotoUdp && protocol != kIpprotoIp)) {
        t_last_error = kErrAfNoSupport;
        return ret(c, ~0u);
    }
    const SOCKET s = ::socket(AF_INET, type == kSockStream ? SOCK_STREAM : SOCK_DGRAM, protocol);
    if (s == INVALID_SOCKET) return ret(c, static_cast<uint32_t>(fail_from_host(GETLASTERR)));
#ifdef _WIN32
    if (type == kSockDgram) {  // no WSAECONNRESET on UDP after an ICMP port unreachable (Cemu)
        BOOL report = FALSE;
        DWORD unused = 0;
        WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12), &report, sizeof report, nullptr, 0, &unused, nullptr, nullptr);
    }
#endif
    std::lock_guard lk(g_lock);
    for (int i = 0; i < kSocketLimit; ++i) {
        if (g_sockets[i]) continue;
        g_sockets[i] = new VirtualSocket{s, type};
        if (g_log) std::printf("nsysnet: socket %d (%s)\n", i + 1, type == kSockStream ? "tcp" : "udp");
        t_last_error = kErrSuccess;
        return ret(c, static_cast<uint32_t>(i + 1));
    }
    closesocket(s);
    t_last_error = kErrInval;
    ret(c, ~0u);
}

// int socketclose(int s)
void sock_close(ppc::Context& c, uint8_t*) {
    const int handle = static_cast<int>(arg(c, 0));
    std::lock_guard lk(g_lock);
    if (handle >= 1 && handle <= kSocketLimit && g_sockets[handle - 1]) {
        closesocket(g_sockets[handle - 1]->s);
        delete g_sockets[handle - 1];
        g_sockets[handle - 1] = nullptr;
    }
    ret(c, 0);
}

// int shutdown(int s, int how): 0 receive, 1 send, 2 both
void sock_shutdown(ppc::Context& c, uint8_t*) {
    VirtualSocket* vs = find(static_cast<int>(arg(c, 0)));
    const int how = static_cast<int>(arg(c, 1));
    if (!vs) {
        t_last_error = kErrInval;
        return ret(c, ~0u);
    }
    const int r = ::shutdown(vs->s, how);
    if (how == 0 || how == 2) vs->shutdown_recv = true;
    t_last_error = kErrSuccess;
    ret(c, static_cast<uint32_t>(r));
}

// int setsockopt(int s, int level, int optname, const void* optval, int optlen)
void sock_setsockopt(ppc::Context& c, uint8_t* b) {
    VirtualSocket* vs = find(static_cast<int>(arg(c, 0)));
    const int level = static_cast<int>(arg(c, 1)), name = static_cast<int>(arg(c, 2));
    const uint32_t optval = arg(c, 3), optlen = arg(c, 4);
    if (!vs) {
        t_last_error = kErrInval;
        return ret(c, ~0u);
    }
    const int value = optval && optlen == 4 ? static_cast<int>(ppc::ld32(b, optval)) : 0;
    if (level == kSolSocket) {
        if (name == kSoReuseAddr) setsockopt(vs->s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&value), 4);
        else if (name == kSoBroadcast) setsockopt(vs->s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&value), 4);
        else if (name == kSoRcvBuf) setsockopt(vs->s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&value), 4);
        else if (name == kSoSndBuf) setsockopt(vs->s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&value), 4);
        else if (name == kSoNbio || name == kSoBio) vs->non_blocking = name == kSoNbio;  // the value is ignored
        else if (name == kSoNonBlock) vs->non_blocking = value != 0;
        else if (g_log) std::printf("nsysnet: setsockopt SOL_SOCKET 0x%x not handled\n", name);
    } else if (level == kIpprotoTcp && name == kTcpNoDelay) {
        setsockopt(vs->s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&value), 4);
    } else if (g_log) {
        std::printf("nsysnet: setsockopt level %d 0x%x not handled\n", level, name);
    }
    t_last_error = kErrSuccess;
    ret(c, 0);
}

// int bind(int s, const sockaddr* addr, int len)
void sock_bind(ppc::Context& c, uint8_t* b) {
    VirtualSocket* vs = find(static_cast<int>(arg(c, 0)));
    if (!vs || !arg(c, 1)) {
        t_last_error = kErrInval;
        return ret(c, ~0u);
    }
    const sockaddr h = guest_to_host_addr(b, arg(c, 1));
    const int r = ::bind(vs->s, &h, sizeof h);
    if (g_log) std::printf("nsysnet: bind %d %s -> %d\n", static_cast<int>(arg(c, 0)), addr_text(h).c_str(), r);
    ret(c, static_cast<uint32_t>(r == 0 ? (t_last_error = kErrSuccess, 0) : fail_from_host(GETLASTERR)));
}

// int connect(int s, const sockaddr* addr, int len)
void sock_connect(ppc::Context& c, uint8_t* b) {
    const int handle = static_cast<int>(arg(c, 0));
    VirtualSocket* vs = find(handle);
    if (!vs || !arg(c, 1)) {
        t_last_error = kErrInval;
        return ret(c, ~0u);
    }
    const sockaddr h = guest_to_host_addr(b, arg(c, 1));
    if (g_log) std::printf("nsysnet: connect %d %s\n", handle, addr_text(h).c_str());
    set_nonblocking(vs->s, vs->non_blocking);
    const int r = ::connect(vs->s, &h, sizeof h);
    set_nonblocking(vs->s, false);
    ret(c, static_cast<uint32_t>(r == 0 ? (t_last_error = kErrSuccess, 0) : fail_from_host(GETLASTERR, true)));
}

// int send(int s, const void* msg, int len, int flags) / int sendto(..., const sockaddr* to, int tolen)
void send_common(ppc::Context& c, uint8_t* b, bool to) {
    const int handle = static_cast<int>(arg(c, 0));
    VirtualSocket* vs = find(handle);
    if (!vs) {
        t_last_error = kErrInval;
        return ret(c, ~0u);
    }
    const auto* msg = reinterpret_cast<const char*>(b + arg(c, 1));
    const int len = static_cast<int>(arg(c, 2));
    const bool dont_wait = vs->non_blocking || (arg(c, 3) & kMsgDontWait);
    sockaddr peer{};
    if (to) peer = guest_to_host_addr(b, arg(c, 4));
    if (!dont_wait && !wait_ready(handle, vs->s, true)) {
        t_last_error = kErrShutdown;
        return ret(c, ~0u);
    }
    set_nonblocking(vs->s, true);
    const int r = to ? ::sendto(vs->s, msg, len, 0, &peer, sizeof peer) : ::send(vs->s, msg, len, 0);
    const int err = GETLASTERR;
    set_nonblocking(vs->s, false);
    log_bytes(to ? "sendto" : "send", handle, reinterpret_cast<const uint8_t*>(msg), r, to ? &peer : nullptr);
    ret(c, static_cast<uint32_t>(r >= 0 ? (t_last_error = kErrSuccess, r) : fail_from_host(err)));
}

// int recv(int s, void* buf, int len, int flags) / int recvfrom(..., sockaddr* from, int* fromlen)
void recv_common(ppc::Context& c, uint8_t* b, bool from) {
    const int handle = static_cast<int>(arg(c, 0));
    VirtualSocket* vs = find(handle);
    if (!vs) {
        t_last_error = kErrInval;
        return ret(c, ~0u);
    }
    if (vs->shutdown_recv) {
        t_last_error = kErrShutdown;
        return ret(c, ~0u);
    }
    auto* buf = reinterpret_cast<char*>(b + arg(c, 1));
    const int len = static_cast<int>(arg(c, 2)), flags = static_cast<int>(arg(c, 3));
    const bool dont_wait = vs->non_blocking || (flags & kMsgDontWait);
    if (!dont_wait && !wait_ready(handle, vs->s, false)) {
        t_last_error = kErrShutdown;
        return ret(c, ~0u);
    }
    sockaddr peer{};
    socklen_t peer_len = sizeof peer;
    set_nonblocking(vs->s, true);
    const int host_flags = (flags & kMsgPeek) ? MSG_PEEK : 0;
    const int r = from ? ::recvfrom(vs->s, buf, len, host_flags, &peer, &peer_len) : ::recv(vs->s, buf, len, host_flags);
    const int err = GETLASTERR;
    set_nonblocking(vs->s, false);
    if (r < 0) return ret(c, static_cast<uint32_t>(fail_from_host(err)));
    if (from && arg(c, 4) && arg(c, 5)) {
        host_to_guest_addr(b, arg(c, 4), peer);
        ppc::st32(b, arg(c, 5), sizeof peer);
    }
    log_bytes(from ? "recvfrom" : "recv", handle, reinterpret_cast<const uint8_t*>(buf), r, from ? &peer : nullptr);
    t_last_error = kErrSuccess;
    ret(c, static_cast<uint32_t>(r));
}

// fd_set masks <-> host fd_set (Cemu's _translateFDSet / _translateFDSetRev)
uint32_t fdset_to_host(uint8_t* b, uint32_t guest_set, fd_set& host, int& max_fd) {
    FD_ZERO(&host);
    if (!guest_set) return 0;
    const uint32_t mask = ppc::ld32(b, guest_set);
    std::lock_guard lk(g_lock);
    for (int h = 1; h <= kSocketLimit; ++h) {
        if (!(mask & (1u << h)) || !g_sockets[h - 1]) continue;
        FD_SET(g_sockets[h - 1]->s, &host);
        max_fd = std::max(max_fd, static_cast<int>(g_sockets[h - 1]->s));
    }
    return mask;
}
void fdset_to_guest(uint8_t* b, uint32_t guest_set, const fd_set& host) {
    if (!guest_set) return;
    uint32_t mask = 0;
    std::lock_guard lk(g_lock);
    for (int h = 1; h <= kSocketLimit; ++h)
        if (g_sockets[h - 1] && FD_ISSET(g_sockets[h - 1]->s, &host)) mask |= 1u << h;
    ppc::st32(b, guest_set, mask);
}

// int select(int nfds, fd_set* read, fd_set* write, fd_set* except, timeval* timeout /* u32 sec, u32 usec */)
void sock_select(ppc::Context& c, uint8_t* b) {
    const uint32_t sets[3] = {arg(c, 1), arg(c, 2), arg(c, 3)}, timeout = arg(c, 4);
    // no timeout: wait until ready (Cemu returns at once here; a plain BSD select waits)
    const int64_t wait_ms = timeout ? int64_t{ppc::ld32(b, timeout)} * 1000 + ppc::ld32(b, timeout + 4) / 1000 : -1;
    for (int64_t waited = 0;; waited += kWaitSliceMs) {
        fd_set host[3];
        int max_fd = -1;
        uint32_t any = 0;
        for (int i = 0; i < 3; ++i) any |= fdset_to_host(b, sets[i], host[i], max_fd);
        const int64_t slice = wait_ms < 0 ? kWaitSliceMs : std::min<int64_t>(kWaitSliceMs, std::max<int64_t>(0, wait_ms - waited));
        timeval tv{0, static_cast<long>(slice * 1000)};
        const int r = any ? ::select(max_fd + 1, sets[0] ? &host[0] : nullptr, sets[1] ? &host[1] : nullptr,
                                     sets[2] ? &host[2] : nullptr, &tv)
                          : (::select(0, nullptr, nullptr, nullptr, &tv), 0);
        if (r < 0) {
            for (uint32_t s : sets) if (s) ppc::st32(b, s, 0);
            return ret(c, static_cast<uint32_t>(fail_from_host(GETLASTERR)));
        }
        if (r > 0) {
            for (int i = 0; i < 3; ++i) fdset_to_guest(b, sets[i], host[i]);
            t_last_error = kErrSuccess;
            return ret(c, static_cast<uint32_t>(r));
        }
        if ((wait_ms >= 0 && waited + slice >= wait_ms) || rt().exit_requested.load()) {
            for (uint32_t s : sets) if (s) ppc::st32(b, s, 0);
            t_last_error = kErrSuccess;
            return ret(c, 0);
        }
    }
}

// int getsockname(int s, sockaddr* addr, int* len)
void sock_getsockname(ppc::Context& c, uint8_t* b) {
    VirtualSocket* vs = find(static_cast<int>(arg(c, 0)));
    if (!vs || !arg(c, 1)) {
        t_last_error = kErrInval;
        return ret(c, ~0u);
    }
    sockaddr h{};
    socklen_t len = sizeof h;
    if (::getsockname(vs->s, &h, &len) != 0) return ret(c, static_cast<uint32_t>(fail_from_host(GETLASTERR)));
    host_to_guest_addr(b, arg(c, 1), h);
    if (arg(c, 2)) ppc::st32(b, arg(c, 2), sizeof h);
    t_last_error = kErrSuccess;
    ret(c, 0);
}

// hostent* gethostbyname(const char* name): one IPv4 address, in a new guest block per call as in Cemu (the game
// resolves only its server names, a few times).
void sock_gethostbyname(ppc::Context& c, uint8_t* b) {
    const std::string name = guest_cstr(arg(c, 0));
    // MH3U finds its public address by asking nncs1/nncs2.app.nintendowifi.net; the online server answers that
    // instead, so the address is the one other players reach (mh3u-revival's Cemu fork, commit 3163f35:
    // https://github.com/Matt-Wood-23/Cemu/tree/mh3u-revival, src/Cafe/OS/libs/nsysnet/nsysnet.cpp)
    const bool nat_check = name.rfind("nncs", 0) == 0 && name.find("nintendo") != std::string::npos;
    const std::string lookup = nat_check && !g_nat_check_host.empty() ? g_nat_check_host : name;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* res = nullptr;
    if (getaddrinfo(lookup.c_str(), nullptr, &hints, &res) != 0 || !res) {
        if (g_log) std::printf("nsysnet: gethostbyname %s -> not found\n", name.c_str());
        return ret(c, 0);
    }
    const auto* in = reinterpret_cast<const sockaddr_in*>(res->ai_addr);
    // wu_hostent {h_name, h_aliases, h_addrtype, h_length, h_addr_list} + list[2] + one address + the name
    const uint32_t block = rt().mem->alloc_hle(20 + 8 + 4 + static_cast<uint32_t>(name.size()) + 1, 4);
    const uint32_t list = block + 20, addr = list + 8, text = addr + 4;
    std::memcpy(b + addr, &in->sin_addr, 4);  // network order, as the guest expects
    std::memcpy(b + text, name.c_str(), name.size() + 1);
    ppc::st32(b, block + 0, text);
    ppc::st32(b, block + 4, 0);
    ppc::st32(b, block + 8, kAfInet);
    ppc::st32(b, block + 12, 4);
    ppc::st32(b, block + 16, list);
    ppc::st32(b, list, addr);
    ppc::st32(b, list + 4, 0);
    if (g_log) std::printf("nsysnet: gethostbyname %s -> %s\n", name.c_str(), inet_ntoa(in->sin_addr));
    freeaddrinfo(res);
    ret(c, block);
}

// int inet_pton(int af, const char* src, void* dst): 1 ok, 0 not an address
void sock_inet_pton(ppc::Context& c, uint8_t* b) {
    in_addr a{};
    if (static_cast<int>(arg(c, 0)) != kAfInet || inet_pton(AF_INET, guest_cstr(arg(c, 1)).c_str(), &a) != 1) {
        t_last_error = kErrAfNoSupport;
        return ret(c, 0);
    }
    std::memcpy(b + arg(c, 2), &a, 4);
    ret(c, 1);
}

// char* inet_ntoa(in_addr): the struct arrives by reference (Cemu); the text is a static buffer, as on the console
void sock_inet_ntoa(ppc::Context& c, uint8_t* b) {
    static uint32_t text = rt().mem->alloc_hle(16, 4);
    const uint8_t* a = b + arg(c, 0);
    std::snprintf(reinterpret_cast<char*>(b + text), 16, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
    ret(c, text);
}

void reg(const char* name, ppc::GuestFunction fn) { ppc::register_import((std::string("nsysnet.") + name).c_str(), fn); }

}  // namespace

void register_nsysnet(Runtime&, bool log, const std::string& nat_check_host) {
    g_log = log;
    g_nat_check_host = nat_check_host;
    reg("socket_lib_init", [](ppc::Context& c, uint8_t*) {
#ifdef _WIN32
        if (!g_lib_ready) {
            WSADATA wsa;
            WSAStartup(MAKEWORD(2, 2), &wsa);
        }
#endif
        g_lib_ready = true;
        ret(c, 0);
    });
    reg("socket_lib_finish", [](ppc::Context& c, uint8_t*) { ret(c, 0); });  // sockets stay usable, as in Cemu's flow
    reg("socket", sock_socket);
    reg("socketclose", sock_close);
    reg("shutdown", sock_shutdown);
    reg("setsockopt", sock_setsockopt);
    reg("bind", sock_bind);
    reg("connect", sock_connect);
    reg("send", [](ppc::Context& c, uint8_t* b) { send_common(c, b, false); });
    reg("sendto", [](ppc::Context& c, uint8_t* b) { send_common(c, b, true); });
    reg("recv", [](ppc::Context& c, uint8_t* b) { recv_common(c, b, false); });
    reg("recvfrom", [](ppc::Context& c, uint8_t* b) { recv_common(c, b, true); });
    reg("select", sock_select);
    reg("getsockname", sock_getsockname);
    reg("socketlasterr", [](ppc::Context& c, uint8_t*) { ret(c, static_cast<uint32_t>(t_last_error)); });
    reg("gethostbyname", sock_gethostbyname);
    reg("inet_pton", sock_inet_pton);
    reg("inet_ntoa", sock_inet_ntoa);
}

}  // namespace mh3u::os
