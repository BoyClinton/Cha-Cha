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
#include <memory>
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
constexpr uint32_t kGuestSockaddrSize = 16;
constexpr uint32_t kFailed = ~0u;  // -1

// Host sockets are always non-blocking; a guest socket in blocking mode waits for them with select (wait_ready).
struct VirtualSocket {
    SOCKET s = INVALID_SOCKET;
    int type = 0;
    bool non_blocking = false;  // the guest's mode
    bool shutdown_recv = false;
    ~VirtualSocket() { closesocket(s); }
};

std::mutex g_lock;
// shared_ptr: a call on one guest thread keeps its socket alive while another thread closes the handle
std::shared_ptr<VirtualSocket> g_sockets[kSocketLimit];
bool g_lib_ready = false;
bool g_log = false;
std::string g_nat_check_host;  // where the game's NAT check (nncs*.app.nintendowifi.net) goes instead
thread_local int t_last_error = kErrSuccess;

std::shared_ptr<VirtualSocket> find(int handle) {
    std::lock_guard lk(g_lock);
    return handle >= 1 && handle <= kSocketLimit ? g_sockets[handle - 1] : nullptr;
}

bool make_nonblocking(SOCKET s) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(s, F_GETFL);
    return flags != -1 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

// a failed call: socketlasterr reports error, the call returns -1
void fail(ppc::Context& c, int error) {
    t_last_error = error;
    ret(c, kFailed);
}

// Cemu's _translateError: the host's error as the Cafe OS one
int cafe_error(int host_error, bool connecting = false) {
    switch (host_error) {
    case WSAEWOULDBLOCK: return connecting ? kErrInProgress : kErrWouldBlock;
    case WSAEINPROGRESS: return kErrInProgress;
    case WSAESHUTDOWN: return kErrShutdown;
    case WSAENOTCONN: return kErrNotConn;
    default: return kErrUnknown;
    }
}

void fail_from_host(ppc::Context& c, int host_error, bool connecting = false) { fail(c, cafe_error(host_error, connecting)); }

void succeed(ppc::Context& c, uint32_t value) {
    t_last_error = kErrSuccess;
    ret(c, value);
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

std::string ipv4_text(const void* network_order_addr) {
    const auto* d = static_cast<const uint8_t*>(network_order_addr);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%u.%u.%u.%u", d[0], d[1], d[2], d[3]);
    return buf;
}

std::string addr_text(const sockaddr& h) {
    const auto* d = reinterpret_cast<const uint8_t*>(h.sa_data);
    return ipv4_text(d + 2) + ":" + std::to_string((d[0] << 8) | d[1]);
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
        const std::shared_ptr<VirtualSocket> current = find(handle);
        if (rt().exit_requested.load() || !current || current->s != s) return false;
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
    if (!g_lib_ready) return fail(c, kErrNotInitialised);
    if (family != kAfInet || (type != kSockStream && type != kSockDgram) ||
        (protocol != kIpprotoTcp && protocol != kIpprotoUdp && protocol != kIpprotoIp))
        return fail(c, kErrAfNoSupport);
    const SOCKET s = ::socket(AF_INET, type == kSockStream ? SOCK_STREAM : SOCK_DGRAM, protocol);
    if (s == INVALID_SOCKET) return fail_from_host(c, GETLASTERR);
    if (!make_nonblocking(s)) {
        const int err = GETLASTERR;
        closesocket(s);
        return fail_from_host(c, err);
    }
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
        g_sockets[i] = std::make_shared<VirtualSocket>();
        g_sockets[i]->s = s;
        g_sockets[i]->type = type;
        if (g_log) std::printf("nsysnet: socket %d (%s)\n", i + 1, type == kSockStream ? "tcp" : "udp");
        return succeed(c, static_cast<uint32_t>(i + 1));
    }
    closesocket(s);
    fail(c, kErrInval);
}

// int socketclose(int s)
void sock_close(ppc::Context& c, uint8_t*) {
    const int handle = static_cast<int>(arg(c, 0));
    std::lock_guard lk(g_lock);
    if (handle >= 1 && handle <= kSocketLimit) g_sockets[handle - 1].reset();  // closed when the last user lets go
    ret(c, 0);
}

// int shutdown(int s, int how): 0 receive, 1 send, 2 both
void sock_shutdown(ppc::Context& c, uint8_t*) {
    const std::shared_ptr<VirtualSocket> vs = find(static_cast<int>(arg(c, 0)));
    const int how = static_cast<int>(arg(c, 1));
    if (!vs) return fail(c, kErrInval);
    if (::shutdown(vs->s, how) != 0) return fail_from_host(c, GETLASTERR);
    if (how == 0 || how == 2) vs->shutdown_recv = true;
    succeed(c, 0);
}

// int setsockopt(int s, int level, int optname, const void* optval, int optlen)
void sock_setsockopt(ppc::Context& c, uint8_t* b) {
    const std::shared_ptr<VirtualSocket> vs = find(static_cast<int>(arg(c, 0)));
    const int level = static_cast<int>(arg(c, 1)), name = static_cast<int>(arg(c, 2));
    const uint32_t optval = arg(c, 3), optlen = arg(c, 4);
    if (!vs) return fail(c, kErrInval);
    const int value = optval && optlen == 4 ? static_cast<int>(ppc::ld32(b, optval)) : 0;
    if (level == kSolSocket && (name == kSoNbio || name == kSoBio)) {
        vs->non_blocking = name == kSoNbio;  // the value is ignored (Cemu)
        return succeed(c, 0);
    }
    if (level == kSolSocket && name == kSoNonBlock) {
        vs->non_blocking = value != 0;
        return succeed(c, 0);
    }
    // options passed on to the host socket (Cafe level/name -> host level/name)
    struct HostOption {
        int cafe_level, cafe_name, host_level, host_name;
    };
    static constexpr HostOption kHostOptions[] = {
        {kSolSocket, kSoReuseAddr, SOL_SOCKET, SO_REUSEADDR}, {kSolSocket, kSoBroadcast, SOL_SOCKET, SO_BROADCAST},
        {kSolSocket, kSoRcvBuf, SOL_SOCKET, SO_RCVBUF},       {kSolSocket, kSoSndBuf, SOL_SOCKET, SO_SNDBUF},
        {kIpprotoTcp, kTcpNoDelay, IPPROTO_TCP, TCP_NODELAY},
    };
    for (const HostOption& o : kHostOptions) {
        if (o.cafe_level != level || o.cafe_name != name) continue;
        if (setsockopt(vs->s, o.host_level, o.host_name, reinterpret_cast<const char*>(&value), sizeof value) != 0)
            return fail_from_host(c, GETLASTERR);
        return succeed(c, 0);
    }
    // accepted without effect, as Cemu does (keep-alive, window scale, multicast, TTL...)
    if (g_log) std::printf("nsysnet: setsockopt level %d 0x%x has no effect\n", level, name);
    succeed(c, 0);
}

// int bind(int s, const sockaddr* addr, int len)
void sock_bind(ppc::Context& c, uint8_t* b) {
    const std::shared_ptr<VirtualSocket> vs = find(static_cast<int>(arg(c, 0)));
    if (!vs || !arg(c, 1)) return fail(c, kErrInval);
    const sockaddr h = guest_to_host_addr(b, arg(c, 1));
    const int r = ::bind(vs->s, &h, sizeof h);
    if (g_log) std::printf("nsysnet: bind %d %s -> %d\n", static_cast<int>(arg(c, 0)), addr_text(h).c_str(), r);
    if (r != 0) return fail_from_host(c, GETLASTERR);
    succeed(c, 0);
}

// int connect(int s, const sockaddr* addr, int len)
void sock_connect(ppc::Context& c, uint8_t* b) {
    const int handle = static_cast<int>(arg(c, 0));
    const std::shared_ptr<VirtualSocket> vs = find(handle);
    if (!vs || !arg(c, 1)) return fail(c, kErrInval);
    const sockaddr h = guest_to_host_addr(b, arg(c, 1));
    if (g_log) std::printf("nsysnet: connect %d %s\n", handle, addr_text(h).c_str());
    if (::connect(vs->s, &h, sizeof h) == 0) return succeed(c, 0);
    const int err = GETLASTERR;
    const bool in_progress = err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
    if (vs->non_blocking || !in_progress) return fail_from_host(c, err, true);
    // a blocking guest connect: wait until the host connect finishes, then take its outcome
    if (!wait_ready(handle, vs->s, true)) return fail(c, kErrShutdown);
    int connect_error = 0;
    socklen_t len = sizeof connect_error;
    if (getsockopt(vs->s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&connect_error), &len) != 0)
        return fail_from_host(c, GETLASTERR);
    if (connect_error != 0) return fail_from_host(c, connect_error, true);
    succeed(c, 0);
}

// int send(int s, const void* msg, int len, int flags) / int sendto(..., const sockaddr* to, int tolen)
void send_common(ppc::Context& c, uint8_t* b, bool to) {
    const int handle = static_cast<int>(arg(c, 0));
    const std::shared_ptr<VirtualSocket> vs = find(handle);
    if (!vs) return fail(c, kErrInval);
    const auto* msg = reinterpret_cast<const char*>(b + arg(c, 1));
    const int len = static_cast<int>(arg(c, 2));
    const bool dont_wait = vs->non_blocking || (arg(c, 3) & kMsgDontWait);
    sockaddr peer{};
    if (to) peer = guest_to_host_addr(b, arg(c, 4));
    if (!dont_wait && !wait_ready(handle, vs->s, true)) return fail(c, kErrShutdown);
    const int r = to ? ::sendto(vs->s, msg, len, 0, &peer, sizeof peer) : ::send(vs->s, msg, len, 0);
    if (r < 0) return fail_from_host(c, GETLASTERR);
    log_bytes(to ? "sendto" : "send", handle, reinterpret_cast<const uint8_t*>(msg), r, to ? &peer : nullptr);
    succeed(c, static_cast<uint32_t>(r));
}

// int recv(int s, void* buf, int len, int flags) / int recvfrom(..., sockaddr* from, int* fromlen)
void recv_common(ppc::Context& c, uint8_t* b, bool from) {
    const int handle = static_cast<int>(arg(c, 0));
    const std::shared_ptr<VirtualSocket> vs = find(handle);
    if (!vs) return fail(c, kErrInval);
    if (vs->shutdown_recv) return fail(c, kErrShutdown);
    auto* buf = reinterpret_cast<char*>(b + arg(c, 1));
    const int len = static_cast<int>(arg(c, 2)), flags = static_cast<int>(arg(c, 3));
    const bool dont_wait = vs->non_blocking || (flags & kMsgDontWait);
    if (!dont_wait && !wait_ready(handle, vs->s, false)) return fail(c, kErrShutdown);
    sockaddr peer{};
    socklen_t peer_len = sizeof peer;
    const int host_flags = (flags & kMsgPeek) ? MSG_PEEK : 0;
    const int r = from ? ::recvfrom(vs->s, buf, len, host_flags, &peer, &peer_len) : ::recv(vs->s, buf, len, host_flags);
    if (r < 0) return fail_from_host(c, GETLASTERR);
    if (from && arg(c, 4) && arg(c, 5)) {
        host_to_guest_addr(b, arg(c, 4), peer);
        ppc::st32(b, arg(c, 5), kGuestSockaddrSize);
    }
    log_bytes(from ? "recvfrom" : "recv", handle, reinterpret_cast<const uint8_t*>(buf), r, from ? &peer : nullptr);
    succeed(c, static_cast<uint32_t>(r));
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
void clear_guest_sets(uint8_t* b, const uint32_t (&guest_sets)[3]) {
    for (uint32_t s : guest_sets)
        if (s) ppc::st32(b, s, 0);
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
            const int err = GETLASTERR;
            clear_guest_sets(b, sets);
            return fail_from_host(c, err);
        }
        if (r > 0) {
            for (int i = 0; i < 3; ++i) fdset_to_guest(b, sets[i], host[i]);
            return succeed(c, static_cast<uint32_t>(r));
        }
        if ((wait_ms >= 0 && waited + slice >= wait_ms) || rt().exit_requested.load()) {
            clear_guest_sets(b, sets);
            return succeed(c, 0);
        }
    }
}

// int getsockname(int s, sockaddr* addr, int* len)
void sock_getsockname(ppc::Context& c, uint8_t* b) {
    const std::shared_ptr<VirtualSocket> vs = find(static_cast<int>(arg(c, 0)));
    if (!vs || !arg(c, 1)) return fail(c, kErrInval);
    sockaddr h{};
    socklen_t len = sizeof h;
    if (::getsockname(vs->s, &h, &len) != 0) return fail_from_host(c, GETLASTERR);
    host_to_guest_addr(b, arg(c, 1), h);
    if (arg(c, 2)) ppc::st32(b, arg(c, 2), kGuestSockaddrSize);
    succeed(c, 0);
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
    if (g_log) std::printf("nsysnet: gethostbyname %s -> %s\n", name.c_str(), ipv4_text(&in->sin_addr).c_str());
    freeaddrinfo(res);
    ret(c, block);
}

// int inet_pton(int af, const char* src, void* dst): 1 ok, 0 not an address
void sock_inet_pton(ppc::Context& c, uint8_t* b) {
    in_addr a{};
    if (static_cast<int>(arg(c, 0)) != kAfInet || inet_pton(AF_INET, guest_cstr(arg(c, 1)).c_str(), &a) != 1) {
        t_last_error = kErrAfNoSupport;
        return ret(c, 0);  // 0: not an address (not -1)
    }
    std::memcpy(b + arg(c, 2), &a, 4);
    ret(c, 1);
}

// char* inet_ntoa(in_addr): the struct arrives by reference (Cemu); the text is a static buffer, as on the console
void sock_inet_ntoa(ppc::Context& c, uint8_t* b) {
    constexpr uint32_t kTextSize = 16;
    static const uint32_t text = rt().mem->alloc_hle(kTextSize, 4);
    const std::string dotted = ipv4_text(b + arg(c, 0));
    std::memcpy(b + text, dotted.c_str(), dotted.size() + 1);
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
            if (const int err = WSAStartup(MAKEWORD(2, 2), &wsa); err != 0) {
                std::printf("nsysnet: WSAStartup failed (%d)\n", err);
                return ret(c, kFailed);
            }
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
