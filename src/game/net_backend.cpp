// Cha-Cha's network backend, replacing Snow / NEX behind the game's MtNet interfaces (docs/online.md, plan steps 2-3).
//
// The game still constructs its MtNet::Snow objects: their base classes hold the request queue and the listener helpers
// we reuse. Only entries of Snow's vtables change, pointing at host functions through ppc::register_indirect, so no game
// code is regenerated. Addresses: build v32.
//
// Copies find each other over UDP: each binds the first free port of a small range, a search asks every port of the
// range, and the host answers. Bound to 127.0.0.1 unless the player turns LAN on (net_configure): then it binds every interface, and a search
// also broadcasts and can ask one address directly. Incoming game packets go to the game's own parsers: LAN is for networks you trust.
//
// A session is asked for with net_request (Settings -> Gameplay -> Online): session_tick runs what the game's own menus would, one
// step a frame, then enters the online stage. The game's own door to the online menus is not found yet (docs/online.md).
//
// Status: wip.
#include "net_backend.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "functions.h"
#include "menu_mouse.h"
#include "text_trace.h"

namespace mh3u::game {
namespace {

constexpr uint32_t kFakeBase = 0x7FE00000;  // not mapped; only ever used as a call_indirect target
constexpr uint32_t kSnowContext = 0x100F3C48;
constexpr uint32_t kSnowSession = 0x100F4060;
constexpr uint32_t kSnowP2p = 0x100F3E5C;

// ---- Wire ---------------------------------------------------------------------------------------------------------
// One datagram: Header, then the type's payload. Both ends are this program on little-endian machines.
constexpr uint32_t kMagic = 0x314E4343;  // "CCN1"
constexpr uint16_t kFirstPort = 47800;
constexpr int kPortCount = 8;
constexpr size_t kMaxPacket = 0x4E2;                       // what P2p's receive buffer holds (0x02E76C04)
constexpr size_t kCreateOptionSize = 0x90, kSearchOptionSize = 0x204;  // Session slots 14, 15

enum MessageType : uint8_t { kSearchReq = 1, kSearchRep, kJoinReq, kJoinRep, kMemberAdd, kMemberLeave, kData, kQuestPosted, kQuestDepart };

struct Header {
    uint32_t magic;
    uint8_t type;
    uint8_t pad;
    uint16_t node;  // the sender
};

struct Peer {
    uint32_t ip;  // network order
    uint16_t port;
};

struct MemberWire {
    uint32_t index;
    uint16_t node, port;
    uint32_t ip;  // network order, as the host sees the member
    uint8_t is_host, pad[3];
};
constexpr int kMaxMembers = 4;

struct SearchRep {
    uint32_t capacity, members;
    uint8_t create_option[kCreateOptionSize], search_option[kSearchOptionSize];
};
struct JoinRep {
    uint32_t your_index, count;
    MemberWire members[kMaxMembers];
};

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kNoSocket = INVALID_SOCKET;
#else
using Socket = int;
constexpr Socket kNoSocket = -1;
#endif

std::atomic<int> g_requested{-1};
std::atomic<bool> g_want_lan{false};
std::atomic<uint32_t> g_want_direct_ip{0};
std::mutex g_wire;  // everything below up to "Guest helpers"
Socket g_socket = kNoSocket;
uint16_t g_node = 0, g_port = 0;
std::map<uint16_t, Peer> g_peers;  // node -> where it is
bool g_lan = false;           // bound to every interface, searches by broadcast too
uint32_t g_direct_ip = 0;     // a host to ask directly (network order), 0 = none
std::map<uint16_t, std::deque<std::vector<uint8_t>>> g_rx;
struct Found {
    uint16_t node;
    Peer peer;
    SearchRep info;
};
std::vector<Found> g_found;
struct Joiner {
    uint16_t node;
    Peer peer;
};
std::deque<Joiner> g_join_queue;
std::deque<MemberWire> g_member_adds;
std::deque<uint16_t> g_member_leaves;
std::deque<std::pair<uint8_t, uint32_t>> g_quest_events;  // message type, quest number
bool g_have_join_reply = false;
JoinRep g_join_reply{};
// the session this copy hosts or has joined
bool g_hosting = false;
uint32_t g_capacity = 0;
std::vector<MemberWire> g_members;
std::array<uint8_t, kCreateOptionSize> g_create_option{};
std::array<uint8_t, kSearchOptionSize> g_search_option{};

bool open_socket() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    g_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_socket == kNoSocket) return false;
#ifdef _WIN32
    u_long nonblocking = 1;
    ioctlsocket(g_socket, FIONBIO, &nonblocking);
#else
    fcntl(g_socket, F_SETFL, fcntl(g_socket, F_GETFL, 0) | O_NONBLOCK);
#endif
    if (g_lan) {
        const int on = 1;
        setsockopt(g_socket, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof on);
    }
    for (int i = 0; i < kPortCount; ++i) {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(g_lan ? INADDR_ANY : INADDR_LOOPBACK);
        a.sin_port = htons(static_cast<uint16_t>(kFirstPort + i));
        if (bind(g_socket, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) {
            g_port = static_cast<uint16_t>(kFirstPort + i);
            return true;
        }
    }
    return false;
}

void close_socket() {
#ifdef _WIN32
    closesocket(g_socket);
#else
    close(g_socket);
#endif
    g_socket = kNoSocket;
}

void send_to(const Peer& to, MessageType type, const void* payload, size_t size) {
    uint8_t buffer[sizeof(Header) + sizeof(SearchRep) + kMaxPacket];
    if (g_socket == kNoSocket || size > sizeof buffer - sizeof(Header)) return;
    const Header h{kMagic, type, 0, g_node};
    std::memcpy(buffer, &h, sizeof h);
    if (size) std::memcpy(buffer + sizeof h, payload, size);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = to.ip;
    a.sin_port = htons(to.port);
    sendto(g_socket, reinterpret_cast<const char*>(buffer), static_cast<int>(sizeof h + size), 0,
           reinterpret_cast<sockaddr*>(&a), sizeof a);
}

// Reads everything waiting into the queues above. Call with g_wire held.
void wire_poll() {
    if (g_socket == kNoSocket) return;
    for (;;) {
        uint8_t buffer[sizeof(Header) + sizeof(SearchRep) + kMaxPacket];
        sockaddr_in from{};
        socklen_t from_size = sizeof from;
        const auto got = recvfrom(g_socket, reinterpret_cast<char*>(buffer), static_cast<int>(sizeof buffer), 0,
                                  reinterpret_cast<sockaddr*>(&from), &from_size);
        if (got < static_cast<decltype(got)>(sizeof(Header))) return;
        Header h;
        std::memcpy(&h, buffer, sizeof h);
        if (h.magic != kMagic || h.node == g_node) continue;
        const Peer sender{from.sin_addr.s_addr, ntohs(from.sin_port)};
        const uint8_t* payload = buffer + sizeof h;
        const size_t size = static_cast<size_t>(got) - sizeof h;
        g_peers[h.node] = sender;
        switch (h.type) {
            case kSearchReq:
                if (g_hosting && g_members.size() < g_capacity) {
                    SearchRep rep{};
                    rep.capacity = g_capacity;
                    rep.members = static_cast<uint32_t>(g_members.size());
                    std::memcpy(rep.create_option, g_create_option.data(), kCreateOptionSize);
                    std::memcpy(rep.search_option, g_search_option.data(), kSearchOptionSize);
                    send_to(sender, kSearchRep, &rep, sizeof rep);
                }
                break;
            case kSearchRep:
                if (size == sizeof(SearchRep)) {
                    bool known = false;
                    for (const Found& f : g_found) known = known || f.node == h.node;
                    if (!known) {
                        Found f{h.node, sender, {}};
                        std::memcpy(&f.info, payload, sizeof f.info);
                        g_found.push_back(f);
                    }
                }
                break;
            case kJoinReq:
                if (g_hosting) g_join_queue.push_back({h.node, sender});
                break;
            case kJoinRep:
                if (size == sizeof(JoinRep)) {
                    std::memcpy(&g_join_reply, payload, sizeof g_join_reply);
                    for (uint32_t i = 0; i < g_join_reply.count && i < kMaxMembers; ++i)
                        if (g_join_reply.members[i].ip == htonl(INADDR_LOOPBACK) || g_join_reply.members[i].ip == 0)
                            g_join_reply.members[i].ip = sender.ip;
                    g_have_join_reply = true;
                }
                break;
            case kMemberAdd:
                if (size == sizeof(MemberWire)) {
                    MemberWire m;
                    std::memcpy(&m, payload, sizeof m);
                    if (m.ip == htonl(INADDR_LOOPBACK)) m.ip = sender.ip;  // on the host's machine: that is the sender's address from here
                    g_member_adds.push_back(m);
                }
                break;
            case kMemberLeave:
                g_member_leaves.push_back(h.node);
                break;
            case kData:
                g_rx[h.node].emplace_back(payload, payload + size);
                break;
            case kQuestPosted:
            case kQuestDepart:
                if (size == sizeof(uint32_t)) {
                    uint32_t number;
                    std::memcpy(&number, payload, sizeof number);
                    g_quest_events.emplace_back(h.type, number);
                }
                break;
            default:
                break;
        }
    }
}

void reset_session_state() {
    g_hosting = false;
    g_capacity = 0;
    g_members.clear();
    g_found.clear();
    g_join_queue.clear();
    g_member_adds.clear();
    g_member_leaves.clear();
    g_have_join_reply = false;
    g_rx.clear();
}

// ---- Guest helpers ------------------------------------------------------------------------------------------------

uint32_t g_next = kFakeBase;

void replace_slot(uint8_t* base, uint32_t vtable, int slot, ppc::GuestFunction fn) {
    const uint32_t fake = g_next;
    g_next += 4;
    ppc::register_indirect(fake, fn);
    ppc::st32(base, vtable + slot * 8 + 4, fake);
}

// Context slot 34: the sign-in request. Same ending as Snow's own (0x02E75DC0): +0xD8 "signed in", listener told state 2.
PPC_FUNC(context_sign_in) {
    const uint32_t self = static_cast<uint32_t>(ctx.r[3]), request = static_cast<uint32_t>(ctx.r[4]);
    ppc::st8(base, self + 0xD8, 1);
    ctx.r[3] = self;
    ctx.r[4] = request;
    ctx.r[5] = 2;
    sub_02E4C9AC(ctx, base);
    ctx.r[3] = 1;
}

// Context slot 35: the sign-out request. Same ending as Snow's own (0x02E76130).
PPC_FUNC(context_sign_out) {
    const uint32_t self = static_cast<uint32_t>(ctx.r[3]), request = static_cast<uint32_t>(ctx.r[4]);
    ppc::st8(base, self + 0xD8, 0);
    ctx.r[3] = self;
    ctx.r[4] = request;
    sub_02E4CAE0(ctx, base);
    ctx.r[3] = 1;
}

// Context slot 12: this machine's id. Same form as Snow's own (0x02E75BF8): kind 4, the 16-bit node number.
PPC_FUNC(context_unique_id) {
    const uint32_t out = static_cast<uint32_t>(ctx.r[4]);
    const uint64_t r1 = ctx.r[1];
    const uint32_t frame = static_cast<uint32_t>(r1) - 0x20;
    ctx.r[1] = frame;
    ppc::st32(base, frame, static_cast<uint32_t>(r1));
    ppc::st16(base, frame + 8, g_node);
    if (out) {
        ctx.r[3] = out;
        ctx.r[4] = 4;
        ctx.r[5] = frame + 8;
        ctx.r[6] = 2;
        sub_02E4A160(ctx, base);
    }
    ctx.r[1] = r1;
    ctx.r[3] = out;
}

// ---- Session ------------------------------------------------------------------------------------------------------
// Fields of MtNet::Snow::Session used here (read from 0x02E784C4 and 0x02E7AA64):
//   +0x77D / +0x77E "joined the Snow room": stay 0, so Snow's own event polling (0x02E7872C) and leave never run
//   +0x77F "session up"; +0x790 members, 4 of 0x1B8 bytes (+0 index, +0xD2 "announced", +0xD4 the listener's member data)
//   +0xE70 this machine's member, +0xE74 the host's member
constexpr uint32_t kSessionUp = 0x77F, kSelfMember = 0xE70, kHostMember = 0xE74;

void lock_session(ppc::Context& ctx, uint8_t* base, uint32_t self) {
    if (!ppc::ld8(base, self + 0x30)) return;
    ctx.r[3] = self + 4;
    ppc::resolve_import("coreinit.OSLockMutex")(ctx, base);
    ppc::st32(base, self + 0x34, ppc::ld32(base, self + 0x34) + 1);
}

void unlock_session(ppc::Context& ctx, uint8_t* base, uint32_t self) {
    if (!ppc::ld8(base, self + 0x30)) return;
    ppc::st32(base, self + 0x34, ppc::ld32(base, self + 0x34) - 1);
    ctx.r[3] = self + 4;
    ppc::resolve_import("coreinit.OSUnlockMutex")(ctx, base);
}

uint32_t request_arg(ppc::Context& ctx, uint8_t* base, uint32_t request, uint32_t index) {
    ctx.r[3] = request;
    ctx.r[4] = index;
    sub_02E5FB50(ctx, base);
    return static_cast<uint32_t>(ctx.r[3]);
}

void call_virtual(ppc::Context& ctx, uint8_t* base, uint32_t self, int slot, uint32_t r4) {
    ctx.r[3] = self;
    ctx.r[4] = r4;
    ppc::call_indirect(ctx, base, ppc::ld32(base, ppc::ld32(base, self) + slot * 8 + 4));
}

// Adds a member the way Snow's event handler does (0x02E7896C -> 0x02E784C4). Returns the add routine's result, 0 = added.
uint32_t add_member(ppc::Context& ctx, uint8_t* base, uint32_t self, uint32_t index, uint16_t node, bool is_self, bool is_host,
                    bool must_connect) {
    const uint64_t r1 = ctx.r[1];
    const uint32_t frame = static_cast<uint32_t>(r1) - 0x120;
    ctx.r[1] = frame;
    ppc::st32(base, frame, static_cast<uint32_t>(r1));
    const uint32_t info = frame + 0x10, node_word = frame + 8, unique_id = info + 0xC;
    ctx.r[3] = unique_id;
    sub_02E49FFC(ctx, base);  // MtNetUniqueId constructor
    ppc::st16(base, node_word, node);
    ctx.r[3] = unique_id;  // kind 4: a 16-bit node number
    ctx.r[4] = 4;
    ctx.r[5] = node_word;
    ctx.r[6] = 2;
    sub_02E4A160(ctx, base);
    std::memset(base + info + 0x9C, 0, 6);
    std::memset(base + info + 0xA2, 0, 0x20);  // name
    ppc::st32(base, info, index);
    sub_02E620B8(ctx, base);  // the value Snow stores here: a time
    ppc::st32(base, info + 8, static_cast<uint32_t>(ctx.r[3]));
    ppc::st8(base, info + 4, is_self);
    ppc::st8(base, info + 5, is_host);
    ppc::st16(base, info + 0xC2, node);
    ppc::st32(base, info + 0xC8, 0);
    ppc::st32(base, info + 0xCC, index + 1);
    ctx.r[3] = self;
    ctx.r[4] = info;
    ctx.r[5] = is_host;
    ctx.r[6] = must_connect;
    sub_02E784C4(ctx, base);
    const uint32_t result = static_cast<uint32_t>(ctx.r[3]);
    ctx.r[3] = unique_id;
    ctx.r[4] = 2;
    sub_02E4A108(ctx, base);  // destructor
    ctx.r[1] = r1;
    return result;
}

// Session slot 29, request 0x301 "create": done at once. Args 0 and 1 are the create and search options (slots 14, 15).
PPC_FUNC(session_create) {
    const uint32_t self = static_cast<uint32_t>(ctx.r[3]), request = static_cast<uint32_t>(ctx.r[4]);
    const uint32_t create_option = request_arg(ctx, base, request, 0), search_option = request_arg(ctx, base, request, 1),
                   capacity = request_arg(ctx, base, request, 2);
    lock_session(ctx, base, self);
    for (uint32_t field : {0x35Cu, 0x364u, 0x368u, 0x36Cu}) ppc::st32(base, self + field, 0);
    ppc::st32(base, self + 0x360, capacity);
    ppc::st32(base, self + 0x370, capacity);
    call_virtual(ctx, base, self, 14, create_option);
    call_virtual(ctx, base, self, 15, search_option);
    ctx.r[3] = self;
    sub_02E7A4E4(ctx, base);  // clears the members, +0xE70 and +0xE74
    for (uint32_t field : {0x77Cu, 0x77Du, 0x77Eu, kSessionUp}) ppc::st8(base, self + field, 0);
    std::memset(base + self + 0x784, 0, 8);
    const uint32_t added = add_member(ctx, base, self, 0, g_node, true, true, true);
    {
        std::lock_guard g(g_wire);
        reset_session_state();
        g_hosting = true;
        // the game's capacity argument can be 0 when nothing asked for a size; the backend's limit then applies
        g_capacity = capacity ? capacity : kMaxMembers;
        g_members.push_back({0, g_node, g_port, htonl(INADDR_LOOPBACK), 1, {}});
        if (create_option) std::memcpy(g_create_option.data(), base + create_option, kCreateOptionSize);
        if (search_option) std::memcpy(g_search_option.data(), base + search_option, kSearchOptionSize);
    }
    std::printf("net: create (capacity %u): member added, result %u\n", capacity, added);
    if (added != 0) {
        unlock_session(ctx, base, self);
        ctx.r[3] = 0xFFFFFFFF;
        return;
    }
    // the rest as Snow's own create ends (0x02E7ADA0)
    ppc::st8(base, self + kSessionUp, 1);
    const uint32_t me = ppc::ld32(base, self + kSelfMember);
    ppc::st8(base, me + 0xD2, 1);
    ctx.r[3] = self;
    ctx.r[4] = request;
    ctx.r[5] = ppc::ld32(base, me);
    ctx.r[6] = me + 0xD4;
    ctx.r[7] = self + 0xC8;
    sub_02E62C8C(ctx, base);  // listeners: created
    ctx.r[3] = self;
    ctx.r[4] = ppc::ld32(base, me);
    ctx.r[5] = ppc::ld32(base, self + kHostMember) + 0xD4;
    sub_02E6269C(ctx, base);  // listeners: this member is the host
    ctx.r[3] = self;
    ctx.r[4] = ppc::ld8(base, self + 0x77C);
    sub_02E62760(ctx, base);
    unlock_session(ctx, base, self);
    ctx.r[3] = 1;
}

// Fires the request's failure the way Snow does: request slot 10 (error code), then a finished phase.
void fail_request(ppc::Context& ctx, uint8_t* base, uint32_t request, uint32_t code) {
    ctx.r[3] = request;
    ctx.r[4] = code;
    ctx.r[5] = 0;
    ctx.r[6] = 0x80000000;
    ppc::call_indirect(ctx, base, ppc::ld32(base, ppc::ld32(base, request) + 0x54));
}

constexpr uint32_t kResultCount = 0xF18, kResults = 0xF1C, kResultSize = 0x744;
constexpr int kSearchFrames = 30;      // calls to wait for answers
constexpr int kJoinRetryFrames = 30;   // calls between join requests
constexpr int kJoinTimeoutFrames = 600;
constexpr uint32_t kErrorTimeout = 0x80050034;  // Snow's own, on a create that timed out (0x02E7AD5C)

// Session slot 30, request 0x302 "search". Phases: 0 ask every port, 1 wait, then the results as Snow delivers them
// (0x02E7B1B8). Result entries: create option at +0, search option at +0x90, then Snow's copies of the session's numbers
// (0x02E7B418); +0x2B8 the session id, here the host's node.
PPC_FUNC(session_search) {
    const uint32_t self = static_cast<uint32_t>(ctx.r[3]), request = static_cast<uint32_t>(ctx.r[4]);
    static int waited = 0;
    ctx.r[3] = request;
    sub_02E5FBC0(ctx, base);
    const uint32_t phase = static_cast<uint32_t>(ctx.r[3]);
    if (phase == 0) {
        waited = 0;
        lock_session(ctx, base, self);
        ctx.r[3] = self + kResultCount;
        sub_02E61C5C(ctx, base);  // empties the result list
        unlock_session(ctx, base, self);
        {
            std::lock_guard g(g_wire);
            g_found.clear();
            for (int i = 0; i < kPortCount; ++i) {
                const uint16_t port = static_cast<uint16_t>(kFirstPort + i);
                if (port != g_port) send_to({htonl(INADDR_LOOPBACK), port}, kSearchReq, nullptr, 0);
                if (g_lan) send_to({htonl(INADDR_BROADCAST), port}, kSearchReq, nullptr, 0);
                if (g_direct_ip) send_to({g_direct_ip, port}, kSearchReq, nullptr, 0);
            }
        }
        ctx.r[3] = request;
        ctx.r[4] = 1;
        sub_02E5FB98(ctx, base);
        ctx.r[3] = 0;
        return;
    }
    if (++waited < kSearchFrames) {
        ctx.r[3] = 0;
        return;
    }
    std::vector<Found> found;
    {
        std::lock_guard g(g_wire);
        wire_poll();
        found = g_found;
    }
    lock_session(ctx, base, self);
    uint32_t count = 0;
    for (const Found& f : found) {
        if (count >= 100) break;
        const uint32_t entry = self + kResults + count * kResultSize;
        std::memcpy(base + entry, f.info.create_option, kCreateOptionSize);
        std::memcpy(base + entry + kCreateOptionSize, f.info.search_option, kSearchOptionSize);
        ppc::st32(base, entry + 0x298, f.info.capacity);
        ppc::st32(base, entry + 0x2A0, f.info.members);
        ppc::st32(base, entry + 0x2A8, f.info.capacity);
        ppc::st32(base, entry + 0x2B8, f.node);
        ++count;
    }
    ppc::st32(base, self + kResultCount, count);
    unlock_session(ctx, base, self);
    std::printf("net: search found %u session(s)\n", count);
    ctx.r[3] = self;
    ctx.r[4] = self + kResultCount;
    sub_02E62814(ctx, base);  // listeners: the list
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t entry = self + kResults + i * kResultSize, flags = ppc::ld32(base, entry + 8);
        if (flags & 2) {
            ctx.r[3] = self;
            ctx.r[4] = i;
            ctx.r[5] = entry;
            sub_02E6298C(ctx, base);
        }
        if (flags & 1) {
            ctx.r[3] = self;
            ctx.r[4] = i;
            ctx.r[5] = entry + kCreateOptionSize;
            sub_02E628C8(ctx, base);
        }
    }
    ctx.r[3] = self;
    ctx.r[4] = request;
    sub_02E62DB8(ctx, base);  // listeners: search done
    ctx.r[3] = 1;
}

// Session slot 31, request 0x303 "join". Phase 0 sets up and asks the host, 1 waits for its answer; 4 on are Snow's own
// (0x02E7B8B8: wait until every member is connected, then listeners), which never touch the server.
PPC_FUNC(session_join) {
    const uint32_t self = static_cast<uint32_t>(ctx.r[3]), request = static_cast<uint32_t>(ctx.r[4]);
    static int waited = 0;
    static uint16_t host = 0;
    ctx.r[3] = request;
    sub_02E5FBC0(ctx, base);
    const uint32_t phase = static_cast<uint32_t>(ctx.r[3]);
    if (phase >= 4) {
        ctx.r[3] = self;
        ctx.r[4] = request;
        sub_02E7B2A8(ctx, base);
        return;
    }
    if (phase == 0) {
        const uint32_t entry = request_arg(ctx, base, request, 0);
        waited = 0;
        host = static_cast<uint16_t>(ppc::ld32(base, entry + 0x2B8));
        lock_session(ctx, base, self);
        for (uint32_t field : {0x35Cu, 0x364u, 0x36Cu}) ppc::st32(base, self + field, 0);
        ppc::st32(base, self + 0x360, ppc::ld32(base, entry + 0x298));
        ppc::st32(base, self + 0x370, ppc::ld32(base, entry + 0x2A8));
        ppc::st32(base, self + 0x368, ppc::ld32(base, entry + 0x2A0));
        ppc::st32(base, self + 0x380, host);
        call_virtual(ctx, base, self, 14, entry);
        call_virtual(ctx, base, self, 15, entry + kCreateOptionSize);
        ctx.r[3] = self;
        sub_02E7A4E4(ctx, base);
        for (uint32_t field : {0x77Cu, 0x77Du, 0x77Eu, kSessionUp, 0x780u, 0x781u}) ppc::st8(base, self + field, 0);
        std::memset(base + self + 0x784, 0, 8);
        unlock_session(ctx, base, self);
        {
            std::lock_guard g(g_wire);
            const auto peer = g_peers.find(host);
            reset_session_state();
            if (peer != g_peers.end()) send_to(peer->second, kJoinReq, nullptr, 0);
        }
        std::printf("net: join: asking host node %04X\n", host);
        ctx.r[3] = request;
        ctx.r[4] = 1;
        sub_02E5FB98(ctx, base);
        ctx.r[3] = 0;
        return;
    }
    JoinRep reply{};
    bool answered = false;
    {
        std::lock_guard g(g_wire);
        wire_poll();
        if (g_have_join_reply) {
            reply = g_join_reply;
            g_have_join_reply = false;
            answered = true;
            g_capacity = kMaxMembers;
            g_members.assign(reply.members, reply.members + reply.count);
            for (const MemberWire& m : g_members)
                if (m.node != g_node) g_peers[m.node] = {m.ip, m.port};
        } else if (++waited % kJoinRetryFrames == 0) {
            const auto peer = g_peers.find(host);
            if (peer != g_peers.end()) send_to(peer->second, kJoinReq, nullptr, 0);
        }
    }
    if (!answered) {
        if (waited < kJoinTimeoutFrames) {
            ctx.r[3] = 0;
            return;
        }
        std::printf("net: join: no answer from host node %04X\n", host);
        fail_request(ctx, base, request, kErrorTimeout);
        ctx.r[3] = request;
        ctx.r[4] = 7;  // Snow's finished-with-error phase of this request
        sub_02E5FB98(ctx, base);
        ctx.r[3] = self;
        ctx.r[4] = request;
        sub_02E7B2A8(ctx, base);
        return;
    }
    // this machine first, as Snow's own (0x02E7B7FC): +0x781 tells phase 4 that this member is in
    uint32_t added = add_member(ctx, base, self, reply.your_index, g_node, true, false, true);
    ppc::st8(base, self + 0x781, 1);
    for (uint32_t i = 0; i < reply.count; ++i) {
        const MemberWire& m = reply.members[i];
        if (m.node != g_node) added |= add_member(ctx, base, self, m.index, m.node, false, m.is_host != 0, true);
    }
    std::printf("net: join: in as member %u of %u, result %u\n", reply.your_index, reply.count, added);
    ctx.r[3] = request;
    ctx.r[4] = 4;
    sub_02E5FB98(ctx, base);
    ctx.r[3] = 0;
}

// A member has gone: as Snow's event handler does it (0x02E788F4).
void remove_member(ppc::Context& ctx, uint8_t* base, uint32_t self, uint16_t node) {
    constexpr uint32_t kMemberArray = 0x790, kMemberBytes = 0x1B8;
    uint32_t gone = 0;
    for (uint32_t i = 0; i < kMaxMembers; ++i) {
        const uint32_t member = self + kMemberArray + i * kMemberBytes;
        if (ppc::ld8(base, member + 0xD4) && ppc::ld16(base, member + 0xC2) == node) gone = member;
    }
    if (!gone) return;
    const int32_t was = static_cast<int32_t>(ppc::ld32(base, gone + 0x1AC));
    if (was >= 0) {
        ctx.r[3] = self;
        ctx.r[4] = static_cast<uint32_t>(was);
        sub_02E62BF8(ctx, base);
    }
    ppc::st32(base, gone + 0x1A8, 0);
    ppc::st32(base, gone + 0x1AC, 0xFFFFFFFE);
    for (uint32_t i = 0; i < kMaxMembers; ++i) {
        const uint32_t other = self + kMemberArray + i * kMemberBytes;
        const int32_t connected = static_cast<int32_t>(ppc::ld32(base, other + 0x1AC));
        if (other == ppc::ld32(base, self + kSelfMember) || connected < 0) continue;
        ctx.r[3] = self;
        ctx.r[4] = static_cast<uint32_t>(connected);
        ctx.r[5] = gone;
        sub_02E78164(ctx, base);
    }
    ctx.r[3] = self;
    ctx.r[4] = gone;
    sub_02E78368(ctx, base);
}

// Session slot 11, every frame: Snow's own, then what arrived over the wire.
PPC_FUNC(session_update) {
    const uint32_t self = static_cast<uint32_t>(ctx.r[3]);
    sub_02E78B30(ctx, base);
    std::deque<Joiner> joiners;
    std::deque<MemberWire> adds;
    std::deque<uint16_t> leaves;
    bool host_left = false;
    {
        std::lock_guard g(g_wire);
        wire_poll();
        joiners.swap(g_join_queue);
        adds.swap(g_member_adds);
        leaves.swap(g_member_leaves);
        for (const uint16_t node : leaves) {
            for (size_t i = 0; i < g_members.size(); ++i)
                if (g_members[i].node == node) {
                    host_left = host_left || g_members[i].is_host;
                    g_members.erase(g_members.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
        }
    }
    for (const uint16_t node : leaves) {
        std::printf("net: node %04X left\n", node);
        remove_member(ctx, base, self, node);
    }
    if (host_left) {  // no host migration: the session is over for this copy too
        std::printf("net: the host left\n");
        g_requested = static_cast<int>(NetRequest::Leave);
    }
    for (const Joiner& j : joiners) {
        JoinRep reply{};
        MemberWire added{};
        {
            std::lock_guard g(g_wire);
            bool again = false;
            uint32_t used = 0;
            for (const MemberWire& m : g_members) {
                again = again || m.node == j.node;
                used |= 1u << m.index;
            }
            uint32_t index = 0;
            while (used & (1u << index)) ++index;
            if (again) {  // the answer was lost: same member, same answer
                for (const MemberWire& m : g_members)
                    if (m.node == j.node) index = m.index;
            } else if (g_members.size() >= g_capacity || index >= kMaxMembers) {
                continue;
            } else {
                added = {index, j.node, j.peer.port, j.peer.ip, 0, {}};
                g_members.push_back(added);
            }
            reply.your_index = index;
            reply.count = static_cast<uint32_t>(g_members.size());
            for (uint32_t i = 0; i < reply.count; ++i) reply.members[i] = g_members[i];
            send_to(j.peer, kJoinRep, &reply, sizeof reply);
            if (!again)
                for (const MemberWire& m : g_members)
                    if (m.node != g_node && m.node != j.node) send_to({m.ip, m.port}, kMemberAdd, &added, sizeof added);
            if (again) continue;
        }
        const uint32_t result = add_member(ctx, base, self, added.index, added.node, false, false, false);
        std::printf("net: node %04X joined as member %u, result %u\n", added.node, added.index, result);
    }
    for (const MemberWire& m : adds) {
        {
            std::lock_guard g(g_wire);
            g_peers[m.node] = {m.ip, m.port};
            g_members.push_back(m);
        }
        add_member(ctx, base, self, m.index, m.node, false, false, false);
    }
    ctx.r[3] = self;
}

// Session slot 32, request 0x304 "leave": tell the others once, then Snow's own (with +0x77D at 0 it skips the server).
PPC_FUNC(session_leave) {
    const uint32_t self = static_cast<uint32_t>(ctx.r[3]), request = static_cast<uint32_t>(ctx.r[4]);
    ctx.r[3] = request;
    sub_02E5FBC0(ctx, base);
    if (ctx.r[3] == 0) {
        std::lock_guard g(g_wire);
        for (const MemberWire& m : g_members)
            if (m.node != g_node) send_to({m.ip, m.port}, kMemberLeave, nullptr, 0);
        reset_session_state();
    }
    ctx.r[3] = self;
    ctx.r[4] = request;
    sub_02E7BBC8(ctx, base);
}

// ---- P2p ----------------------------------------------------------------------------------------------------------
// Fields of MtNet::Snow::P2p: +0xC8 receive buffer; +0x5AC 4 peers of 12 bytes: +0 "in use", +1 "connected" (slot 12 reads
// them). The peer's node lives here; datagrams go straight to its port.
constexpr uint32_t kReceiveBuffer = 0xC8, kPeers = 0x5AC, kPeerSize = 0xC;
constexpr int kMaxPeers = 4;
std::array<uint16_t, kMaxPeers> g_peer_nodes{};  // guarded by g_wire; 0 = free

// P2p slot 15, request 0x201 "open". The u16 at arg 0 + 0xE is the peer's member index + 1, the value add_member stores at
// info +0xC8 (seen in two-copy runs). Nothing to set up on UDP: done at once.
PPC_FUNC(p2p_open) {
    const uint32_t self = static_cast<uint32_t>(ctx.r[3]), request = static_cast<uint32_t>(ctx.r[4]);
    const uint32_t arg = request_arg(ctx, base, request, 0);
    uint32_t peer = kMaxPeers;
    const uint32_t member = arg ? ppc::ld16(base, arg + 0xE) : 0;
    uint16_t node = 0;
    {
        std::lock_guard g(g_wire);
        for (const MemberWire& m : g_members)
            if (m.index + 1 == member) node = m.node;
        for (uint32_t i = 0; i < kMaxPeers && peer == kMaxPeers; ++i)
            if (g_peer_nodes[i] == 0) peer = i;
        if (peer != kMaxPeers && node != 0) g_peer_nodes[peer] = node;
    }
    std::printf("net: p2p open: member %u, node %04X, as peer %u\n", member - 1, node, peer);
    if (peer == kMaxPeers || node == 0) {
        fail_request(ctx, base, request, 0x80030002);
        ctx.r[3] = request;
        ctx.r[4] = 2;
        sub_02E5FB98(ctx, base);
        ctx.r[3] = self;
        ctx.r[4] = request;
        sub_02E773C4(ctx, base);
        return;
    }
    ppc::st8(base, self + kPeers + peer * kPeerSize, 1);
    ppc::st8(base, self + kPeers + peer * kPeerSize + 1, 1);
    ctx.r[3] = self;
    ctx.r[4] = request;
    ctx.r[5] = peer;
    sub_02E5C9F4(ctx, base);  // listeners: open
    ctx.r[3] = 1;
}

// counters for the log, every kStatsFrames P2p updates
constexpr uint32_t kStatsFrames = 1200;
std::map<uint32_t, uint64_t> g_sent_sizes;  // (size << 16 | first two bytes) -> packets, to see what kinds of packet flow
uint64_t g_sent_packets = 0, g_sent_bytes = 0, g_received_packets = 0, g_received_bytes = 0;  // guarded by g_wire

// P2p slot 11, every frame. Delivery as Snow's (0x02E76C04): the packet in +0xC8, then 0x02E5C920(this, peer, buffer, size).
PPC_FUNC(p2p_update) {
    const uint32_t self = static_cast<uint32_t>(ctx.r[3]);
    sub_02E5C5A0(ctx, base);
    for (uint32_t peer = 0; peer < kMaxPeers; ++peer) {
        for (;;) {
            std::vector<uint8_t> packet;
            {
                std::lock_guard g(g_wire);
                wire_poll();
                const uint16_t node = g_peer_nodes[peer];
                const auto queue = g_rx.find(node);
                if (node == 0 || queue == g_rx.end() || queue->second.empty()) break;
                packet = std::move(queue->second.front());
                queue->second.pop_front();
                ++g_received_packets;
                g_received_bytes += packet.size();
            }
            const size_t size = packet.size() < kMaxPacket ? packet.size() : kMaxPacket;
            std::memcpy(base + self + kReceiveBuffer, packet.data(), size);
            ctx.r[3] = self;
            ctx.r[4] = peer;
            ctx.r[5] = self + kReceiveBuffer;
            ctx.r[6] = size;
            sub_02E5C920(ctx, base);
        }
    }
    ctx.r[3] = self;
    sub_02E5C784(ctx, base);
    static uint32_t updates = 0;
    if (++updates % kStatsFrames == 0) {
        std::lock_guard g(g_wire);
        std::printf("net: totals: sent %llu packets %llu bytes, received %llu packets %llu bytes\n",
                    static_cast<unsigned long long>(g_sent_packets), static_cast<unsigned long long>(g_sent_bytes),
                    static_cast<unsigned long long>(g_received_packets), static_cast<unsigned long long>(g_received_bytes));
        std::printf("net:   rx queues:");
        for (const auto& [node, queue] : g_rx) std::printf(" %04X=%zu", node, queue.size());
        std::printf("; peers:");
        for (const uint16_t node : g_peer_nodes) std::printf(" %04X", node);
        std::printf("\n");
        for (const auto& [kind, count] : g_sent_sizes)
            std::printf("net:   size %u head %04X: %llu\n", kind >> 16, kind & 0xFFFF, static_cast<unsigned long long>(count));
        g_sent_sizes.clear();
    }
    ctx.r[3] = self;
}

// P2p slot 13: send(peer or -1 for all, data, size) -> size.
PPC_FUNC(p2p_send) {
    const uint32_t peer = static_cast<uint32_t>(ctx.r[4]), data = static_cast<uint32_t>(ctx.r[5]),
                   size = static_cast<uint32_t>(ctx.r[6]);
    if (size == 0 || size > kMaxPacket) {
        ctx.r[3] = 0;
        return;
    }
    {
        std::lock_guard g(g_wire);
        for (uint32_t i = 0; i < kMaxPeers; ++i) {
            if (g_peer_nodes[i] == 0 || (peer != 0xFFFFFFFF && peer != i)) continue;
            const auto peer = g_peers.find(g_peer_nodes[i]);
            if (peer != g_peers.end()) {
                send_to(peer->second, kData, base + data, size);
                ++g_sent_packets;
                g_sent_bytes += size;
                ++g_sent_sizes[(size << 16) | ppc::ld16(base, data)];
            }
        }
    }
    ctx.r[3] = size;
}

// P2p slot 14: close(peer). Ours, then Snow's own, whose sender and receiver objects never existed (it checks for that).
PPC_FUNC(p2p_close) {
    const uint32_t self = static_cast<uint32_t>(ctx.r[3]), peer = static_cast<uint32_t>(ctx.r[4]);
    if (peer < kMaxPeers) {
        std::lock_guard g(g_wire);
        g_peer_nodes[peer] = 0;
    }
    ctx.r[3] = self;
    ctx.r[4] = peer;
    sub_02E766E4(ctx, base);
}

// ---- nn_fp --------------------------------------------------------------------------------------------------------
// No friends service: everything succeeds with empty answers, as a console signed in with an empty friend list.
constexpr uint32_t kMyPresenceSize = 0x30;  // what 0x0272A280 reads of it

// Result XAsync(callback(Result, void*), void* user): finishes at once, successfully.
PPC_FUNC(fp_async_ok) {
    const uint32_t callback = static_cast<uint32_t>(ctx.r[3]), user = static_cast<uint32_t>(ctx.r[4]);
    if (callback) {
        ctx.r[3] = 0;
        ctx.r[4] = user;
        ppc::call_indirect(ctx, base, callback);
    }
    ctx.r[3] = 0;
}

void register_fp(const std::vector<std::string>& fp_imports) {
    for (const std::string& n : fp_imports)
        ppc::register_import(("nn_fp." + n).c_str(), [](ppc::Context& c, uint8_t*) { c.r[3] = 0; });
    ppc::register_import("nn_fp.IsInitialized__Q2_2nn2fpFv", [](ppc::Context& c, uint8_t*) { c.r[3] = 1; });
    ppc::register_import("nn_fp.HasLoggedIn__Q2_2nn2fpFv", [](ppc::Context& c, uint8_t*) { c.r[3] = 1; });
    ppc::register_import("nn_fp.LoginAsync__Q2_2nn2fpFPFQ2_2nn6ResultPv_vPv", &fp_async_ok);
    ppc::register_import("nn_fp.GetMyPresence__Q2_2nn2fpFPQ3_2nn2fp10MyPresence", [](ppc::Context& c, uint8_t* b) {
        if (c.r[3]) std::memset(b + static_cast<uint32_t>(c.r[3]), 0, kMyPresenceSize);
        c.r[3] = 0;
    });
}

// ---- The Host / Join sequence ---------------------------------------------------------------------------------------
// sLobbyNetwork's Internet machine (0x02266E88): state +0x8AC (17 signed in, 3 and 6 session hub, 10 in a session), requests +0x8B0
// (2 sign in, 4 session stage, 8 create, 0x10 search, 0x20 join with result +0x8C4, 0x40 leave).
constexpr uint32_t kLobbyNetwork = 0x103A2F90, kManager = 0x103A33E8, kLobby = 0x103147A0, kGame = 0x101FDB9C, kStage = 0x10315C80;
constexpr uint32_t kPlayers = 0x10314B80, kAreaSystem = 0x103D2EF4;
constexpr uint32_t kHarborSquare = 0x1030CAE8, kOnlineHall = 0x1030C918, kQuestArea = 0x1030CEA8;  // area DTIs: aHarborSquare, aHarborRoomGuild, aQuestArea
constexpr uint8_t kHarborStage = 0x1B;
constexpr uint32_t kStateHub = 17, kStateSessionHub = 3, kStateSearched = 6, kStateInSession = 10, kStateFailed = 13;
constexpr uint32_t kRequestSignIn = 2, kRequestSession = 4, kRequestCreate = 8, kRequestSearch = 0x10, kRequestJoin = 0x20, kRequestLeave = 0x40;
constexpr int kSettleFrames = 120, kStepTimeoutFrames = 3600, kSearchTries = 10, kSearchPauseFrames = 120, kHallDelayFrames = 300;

// Hub movement (docs/online.md "Hub movement delay"): the sender (0x02798F68 -> 0x022627CC) sends the hunter's position when a countdown
// reloaded from sLobbyNetwork +0x594 runs out, and the receiver 0x02262DA0 glides the other hunter there over the same number of ticks.
// Only the constructor 0x022601B8 sets it, to 20 (about 0.7 s behind). Every copy uses kHubInterval while online; MH3U_NET_HUB_INTERVAL overrides.
constexpr uint32_t kHubIntervalOffset = 0x594;
constexpr int16_t kHubIntervalGame = 20, kHubInterval = 4;

int16_t hub_interval() {
    static const int16_t value = [] {
        const char* text = std::getenv("MH3U_NET_HUB_INTERVAL");
        const int parsed = text ? std::atoi(text) : 0;
        return static_cast<int16_t>(parsed >= 1 && parsed <= 600 ? parsed : kHubInterval);
    }();
    return value;
}

enum class Step { Idle, ToHarbor, WaitHarbor, SignIn, WaitSignIn, OpenSession, WaitSession, Create, Search, WaitSearch, Join, WaitInSession, ToHall, WaitHall, InHall, Leave, WaitLeave, BackToHarbor };

std::atomic<Step> g_step{Step::Idle};
std::mutex g_status_lock;
std::string g_status = "Not connected";
bool g_want_host = false;
int g_step_frames = 0, g_search_tries = 0;
ppc::GuestFunction g_swap = nullptr;

void (*g_toast)(const std::string&) = nullptr;
constexpr uint32_t kFacilityText = 0x67;

// tell: also on screen, for what the player has to know
void set_status(const std::string& text, bool tell = false) {
    std::lock_guard g(g_status_lock);
    if (g_status != text) {
        std::printf("net: %s\n", text.c_str());
        if (tell && g_toast) g_toast(text);
    }
    g_status = text;
}

void go(Step step) {
    g_step = step;
    g_step_frames = 0;
}

void fail(const std::string& text) {
    set_status(text, true);
    go(Step::Idle);
}

void request_bits(uint8_t* base, uint32_t net, uint32_t bits) { ppc::st32(base, net + 0x8B0, ppc::ld32(base, net + 0x8B0) | bits); }

// The game's own area change, (*0x103D2EF4)->slot 11(DTI), as aGalleryMenu::vf7 does it (0x02018EB0).
void change_area(ppc::Context& ctx, uint8_t* base, uint32_t dti) {
    const ppc::Context saved = ctx;
    const uint32_t system = ppc::ld32(base, kAreaSystem);
    ctx.r[3] = system;
    ctx.r[4] = dti;
    ppc::call_indirect(ctx, base, ppc::ld32(base, ppc::ld32(base, system) + 0x5C));
    const uint64_t r1 = ctx.r[1];
    ctx = saved;
    ctx.r[1] = r1;
}

// The sub-area the Harbor areas load: sLobby +0x5450, +1 stage, +2 and +0x15 sub-area (filled the same way by 0x0214CE3C).
void set_sub_area(uint8_t* base, uint8_t sub) {
    const uint32_t info = ppc::ld32(base, kLobby) + 0x5450;
    ppc::st8(base, info + 2, sub);
    ppc::st8(base, info + 0x15, sub);
}

bool on_stage(uint8_t* base, uint8_t stage, uint8_t sub) {
    const uint32_t s = ppc::ld32(base, kStage);
    return s && ppc::ld8(base, s + 0x115) == stage && ppc::ld8(base, s + 0x116) == sub;
}

// In town and on foot: my hunter's parts have loaded (the 11 load states at unit +0x2400, 0x02847AA4) and the area's manager is idle.
bool hunter_ready(uint8_t* base) {
    const uint32_t game = ppc::ld32(base, kGame), players = ppc::ld32(base, kPlayers);
    if (!game || !players) return false;
    const int8_t slot = static_cast<int8_t>(ppc::ld8(base, game + 0x4B));
    if (slot < 0 || slot >= 8) return false;
    const uint32_t unit = ppc::ld32(base, players + 0x40 + static_cast<uint32_t>(slot) * 4);
    if (!unit) return false;
    for (uint32_t i = 0; i < 11; ++i)
        if (ppc::ld8(base, unit + 0x2400 + i) != 1) return false;
    return true;
}

// An event cutscene (sEvDemo 0x103138C8, its update 0x0210F7D0) runs in states +0x34 2, 3, 4. Starting, it puts the frame pacer (sMain 0x103CEA44) in fixed step at 30 FPS, and only its
// own end in state 4 takes that off. An area change wipes the state, so a cutscene cut off by one leaves the game at 30 FPS until it restarts.
constexpr uint32_t kEvDemo = 0x103138C8, kMain = 0x103CEA44, kRender = 0x102CE7FC;
constexpr int kCutsceneWaitFrames = 1800;  // a minute at the cutscene's 30 FPS

bool cutscene_playing(uint8_t* base) {
    const uint32_t demo = ppc::ld32(base, kEvDemo);
    if (!demo) return false;
    const uint8_t state = ppc::ld8(base, demo + 0x34);
    return state >= 2 && state <= 4;
}

// What the end of state 4 does (0x0210FBAC), when that end was never reached.
void end_stuck_cutscene(ppc::Context& ctx, uint8_t* base) {
    const uint32_t main = ppc::ld32(base, kMain);
    if (!main || !ppc::ld8(base, main + 0x76) || cutscene_playing(base)) return;
    const ppc::Context saved = ctx;
    ctx.r[3] = main;
    ctx.r[4] = 0;
    sub_0294C968(ctx, base);
    ppc::stf32(base, main + 0x4C, 60.0f);
    ctx.f[1].ps0 = 60.0;
    ctx.r[3] = ppc::ld32(base, kRender);
    sub_02BA5E54(ctx, base);
    ctx.r[3] = ppc::ld32(base, kRender);
    ctx.r[4] = 0;
    sub_02BA5EE8(ctx, base);
    ppc::stf32(base, main + 0x88, 1.0f);
    std::printf("net: ended a cutscene's frame pacing\n");
    const uint64_t r1 = ctx.r[1];
    ctx = saved;
    ctx.r[1] = r1;
}

bool manager_idle(uint8_t* base) {
    const uint32_t manager = ppc::ld32(base, kManager);
    return manager && ppc::ld8(base, manager + 0x190) == 0 && (ppc::ld32(base, manager + 0x18C) & 1) == 0 && ppc::ld32(base, manager + 0x194) == 0;
}

// A request from the settings page. In a quest there is nothing to do yet: phases 2 and up of sGame +0x196 are quest time.
void take_request(uint8_t* base) {
    const int request = g_requested.exchange(-1);
    if (request < 0) return;
    const bool leave = request == static_cast<int>(NetRequest::Leave);
    if (leave) {
        if (g_step != Step::Idle) go(Step::Leave);
        return;
    }
    const uint32_t game = ppc::ld32(base, kGame);
    if (g_step != Step::Idle) return;
    if (game && ppc::ld8(base, game + 0x196) >= 2) {
        set_status("Finish the quest first", true);
        return;
    }
    if (const bool lan = g_want_lan.load(); g_socket != kNoSocket && lan != g_lan) {
        std::lock_guard g(g_wire);
        close_socket();
    }
    if (g_socket == kNoSocket) {
        std::lock_guard g(g_wire);
        g_lan = g_want_lan;
        g_direct_ip = g_want_direct_ip;
        if (!open_socket()) {
            fail("No free network port");
            return;
        }
        g_node = static_cast<uint16_t>((static_cast<uint32_t>(std::time(nullptr)) * 31u) ^ (g_port * 257u));
        if (g_node == 0) g_node = 1;
        std::printf("net: node %04X, UDP port %u (%s)\n", g_node, g_port, g_lan ? "LAN" : "127.0.0.1 only");
    }
    g_want_host = request == static_cast<int>(NetRequest::Host);
    g_search_tries = 0;
    go(Step::ToHarbor);
}

// ---- Quests together --------------------------------------------------------------------------------------------------
// The host picks a quest at the game's own counter; the others follow by themselves. The game's party messages (0x0224E3E8, on the object
// at sLobby +0x4E20) do not reach a guest yet, so this carries the same facts over our wire and fills each copy's party:
//   quest select 0x021E355C(sLobby, my pad block, number, 1); party entry (0x9C4 each, from 0x02250C58) +0xB = who is in;
//   sGame +0x4A = the host's member; the launch step 0x0214E8A8. sLobby +0x54AE: 1 host's mode bits, 8 = departing (0x022049DC).
uint32_t g_posted_quest = 0;
bool g_departed = false;
constexpr int kQuestSends = 3;  // datagrams can be lost; receivers ignore repeats

void broadcast_quest(MessageType type, uint32_t number) {
    std::lock_guard g(g_wire);
    for (const MemberWire& m : g_members)
        if (m.node != g_node)
            for (int i = 0; i < kQuestSends; ++i) send_to({m.ip, m.port}, type, &number, sizeof number);
}

uint32_t my_member(ppc::Context& ctx, uint8_t* base) {
    ctx.r[3] = ppc::ld32(base, kManager);
    sub_0226EFEC(ctx, base);
    return static_cast<uint32_t>(ctx.r[3]);
}

uint8_t member_mask() {
    std::lock_guard g(g_wire);
    uint8_t mask = 0;
    for (const MemberWire& m : g_members) mask = static_cast<uint8_t>(mask | (1u << m.index));
    return mask;
}

uint8_t host_member() {
    std::lock_guard g(g_wire);
    for (const MemberWire& m : g_members)
        if (m.is_host) return static_cast<uint8_t>(m.index);
    return 0;
}

void fill_party(ppc::Context& ctx, uint8_t* base) {
    const uint32_t party = ppc::ld32(base, ppc::ld32(base, kLobby) + 0x4E20);
    if (!party) return;
    ctx.r[3] = party;
    sub_02250C58(ctx, base);
    const uint32_t entries = static_cast<uint32_t>(ctx.r[3]);
    const uint32_t me = my_member(ctx, base);
    ppc::st8(base, entries + me * 0x9C4 + 0xB, member_mask());
    ppc::st8(base, ppc::ld32(base, kGame) + 0x4A, host_member());
}

// Guest: what the host's counter did.
void follow_post(ppc::Context& ctx, uint8_t* base, uint32_t number) {
    const uint32_t game = ppc::ld32(base, kGame), players = ppc::ld32(base, kPlayers);
    const uint32_t unit = ppc::ld32(base, players + 0x40 + ppc::ld8(base, game + 0x4B) * 4);
    const ppc::Context saved = ctx;
    ctx.r[3] = ppc::ld32(base, kLobby);
    ctx.r[4] = ppc::ld32(base, unit + 0xE30);
    ctx.r[5] = number;
    ctx.r[6] = 1;
    sub_021E355C(ctx, base);
    fill_party(ctx, base);
    std::printf("net: following quest %u\n", number);
    const uint64_t r1 = ctx.r[1];
    ctx = saved;
    ctx.r[1] = r1;
}

// Guest: the launch step 0x0214E8A8, with the party table first (0x0214E454).
void follow_depart(ppc::Context& ctx, uint8_t* base, uint32_t number) {
    const uint32_t game_quest = ppc::ld32(base, kGame) + 0x34, lobby = ppc::ld32(base, kLobby);
    const ppc::Context saved = ctx;
    fill_party(ctx, base);
    ctx.r[3] = ppc::ld32(base, 0x103A2DB0);
    sub_0225B238(ctx, base);
    ppc::st16(base, lobby + 0x522C, static_cast<uint16_t>(number));
    ppc::st16(base, lobby + 0x54AC, static_cast<uint16_t>(number));
    ppc::st8(base, lobby + 0x5450, 0);
    ctx.r[3] = ppc::ld32(base, 0x10314890);
    sub_021512E8(ctx, base);
    ctx.r[3] = ppc::ld32(base, 0x10315F50);
    sub_02197EBC(ctx, base);
    ctx.r[3] = ppc::ld32(base, 0x1031537C);
    sub_02166A1C(ctx, base);
    ppc::st32(base, game_quest + 0x24, 1);
    ppc::st32(base, game_quest + 0x30, number);
    ppc::st8(base, lobby + 0x154, 0);
    ppc::st8(base, lobby + 0x5234, 0);
    const uint64_t r1 = ctx.r[1];
    ctx = saved;
    ctx.r[1] = r1;
    change_area(ctx, base, kQuestArea);
    const ppc::Context after = ctx;
    ctx.r[3] = ppc::ld32(base, 0x1031537C);
    sub_021788E4(ctx, base);
    ctx.r[3] = ppc::ld32(base, kLobbyNetwork);
    ctx.r[4] = 1;
    sub_02265070(ctx, base);
    const uint64_t r1b = ctx.r[1];
    ctx = after;
    ctx.r[1] = r1b;
    std::printf("net: departing on quest %u\n", number);
}

void quest_tick(ppc::Context& ctx, uint8_t* base) {
    const uint32_t lobby = ppc::ld32(base, kLobby);
    if (!lobby) return;
    // Back from a quest: ready for the next one. The game can leave the last quest's number and depart bit set, so only a new press counts.
    static bool away = false, depart_was_set = false;
    const bool in_hall = on_stage(base, kHarborStage, 1);
    if (!in_hall) {
        away = true;
        return;
    }
    if (away) {
        away = false;
        g_departed = false;
        g_posted_quest = g_want_host ? ppc::ld16(base, lobby + 0x522C) : 0;
        depart_was_set = (ppc::ld8(base, lobby + 0x54AE) & 8) != 0;
        std::printf("net: back in the hall\n");
    }
    if (g_want_host) {
        const uint32_t number = ppc::ld16(base, lobby + 0x522C), mode = ppc::ld8(base, lobby + 0x54AE);
        const bool depart_set = (mode & 8) != 0, depart_pressed = depart_set && !depart_was_set;
        depart_was_set = depart_set;
        if (number == 0 || !(mode & 1)) {
            if (g_posted_quest) {
                g_posted_quest = 0;
                g_departed = false;
            }
            return;
        }
        if (number != g_posted_quest) {
            g_posted_quest = number;
            g_departed = false;
            const ppc::Context saved = ctx;
            fill_party(ctx, base);
            const uint64_t r1 = ctx.r[1];
            ctx = saved;
            ctx.r[1] = r1;
            std::printf("net: posted quest %u to the others\n", number);
            broadcast_quest(kQuestPosted, number);
        }
        if (depart_pressed && !g_departed) {
            g_departed = true;
            // the depart call (0x0225111C) recomputes the party, so the others go back in, and the table the launch uses is built again (0x0225B238)
            const ppc::Context saved = ctx;
            fill_party(ctx, base);
            ctx.r[3] = ppc::ld32(base, 0x103A2DB0);
            sub_0225B238(ctx, base);
            const uint64_t r1 = ctx.r[1];
            ctx = saved;
            ctx.r[1] = r1;
            std::printf("net: departing on quest %u\n", number);
            broadcast_quest(kQuestDepart, number);
        }
        return;
    }
    std::deque<std::pair<uint8_t, uint32_t>> events;
    {
        std::lock_guard g(g_wire);
        events.swap(g_quest_events);
    }
    for (const auto& [type, number] : events) {
        if (type == kQuestPosted && number != g_posted_quest) {
            g_posted_quest = number;
            g_departed = false;
            follow_post(ctx, base, number);
        } else if (type == kQuestDepart && !g_departed) {
            if (number != g_posted_quest) {
                g_posted_quest = number;
                follow_post(ctx, base, number);
            }
            g_departed = true;
            follow_depart(ctx, base, number);
        }
    }
}

void session_tick(ppc::Context& ctx, uint8_t* base) {
    take_request(base);
    const Step step = g_step;
    const uint32_t net = ppc::ld32(base, kLobbyNetwork);
    if (!net) return;
    const int16_t interval = step == Step::Idle ? kHubIntervalGame : hub_interval();  // the game's own value back once offline
    if (static_cast<int16_t>(ppc::ld16(base, net + kHubIntervalOffset)) != interval) ppc::st16(base, net + kHubIntervalOffset, static_cast<uint16_t>(interval));
    if (step == Step::Idle) return;
    const uint32_t state = ppc::ld32(base, net + 0x8AC);
    ++g_step_frames;
    const bool timed_out = g_step_frames > kStepTimeoutFrames;
    switch (step) {
        case Step::ToHarbor:
            if (ppc::ld8(base, ppc::ld32(base, kLobby) + 0x5450) != 0) break;  // Neko's menu still closing
            if (on_stage(base, kHarborStage, 0) || on_stage(base, kHarborStage, 1)) {
                go(Step::SignIn);
                break;
            }
            set_status("Going to Port Tanzia", true);
            set_sub_area(base, 0);
            change_area(ctx, base, kHarborSquare);
            go(Step::WaitHarbor);
            break;
        case Step::WaitHarbor:
            if (on_stage(base, kHarborStage, 0) && g_step_frames > kSettleFrames) go(Step::SignIn);
            else if (timed_out) fail("Could not reach Port Tanzia");
            break;
        case Step::SignIn:
            if (state == kStateHub || state == kStateSessionHub || state == kStateSearched) {
                go(Step::OpenSession);
            } else if (hunter_ready(base) && manager_idle(base) && g_step_frames > kSettleFrames) {
                set_status("Signing in");
                request_bits(base, net, kRequestSignIn);
                go(Step::WaitSignIn);
            } else if (timed_out) {
                fail("The game is busy; try again in town");
            }
            break;
        case Step::WaitSignIn:
            if (state == kStateHub) go(Step::OpenSession);
            else if (state == kStateFailed || timed_out) fail("Sign-in failed");
            break;
        case Step::OpenSession:
            if (state == kStateSessionHub || state == kStateSearched) {
                go(g_want_host ? Step::Create : Step::Search);
            } else {
                request_bits(base, net, kRequestSession);
                go(Step::WaitSession);
            }
            break;
        case Step::WaitSession:
            if (state == kStateSessionHub) go(g_want_host ? Step::Create : Step::Search);
            else if (timed_out) fail("Could not open a session");
            break;
        case Step::Create:
            set_status("Creating a session", true);
            request_bits(base, net, kRequestCreate);
            go(Step::WaitInSession);
            break;
        case Step::Search:
            set_status("Looking for a session", true);
            request_bits(base, net, kRequestSearch);
            go(Step::WaitSearch);
            break;
        case Step::WaitSearch: {
            if (state != kStateSearched || g_step_frames < kSearchPauseFrames) {
                if (timed_out) fail("Search did not finish");
                break;
            }
            bool found;
            {
                std::lock_guard g(g_wire);
                found = !g_found.empty();
            }
            if (found) go(Step::Join);
            else if (++g_search_tries < kSearchTries) go(Step::Search);
            else fail("No session found");
            break;
        }
        case Step::Join:
            set_status("Joining", true);
            ppc::st32(base, net + 0x8C4, 0);
            request_bits(base, net, kRequestJoin);
            go(Step::WaitInSession);
            break;
        case Step::WaitInSession:
            if (state == kStateInSession) go(Step::ToHall);
            else if (timed_out) fail("Could not get into the session");
            break;
        case Step::ToHall:
            if (g_step_frames < kHallDelayFrames) break;
            if (cutscene_playing(base) && g_step_frames < kHallDelayFrames + kCutsceneWaitFrames) break;  // let it end: leaving now would strand its 30 FPS
            set_status("Entering the hall");
            set_sub_area(base, 1);
            change_area(ctx, base, kOnlineHall);
            go(Step::WaitHall);
            break;
        case Step::WaitHall:
            end_stuck_cutscene(ctx, base);
            if (on_stage(base, kHarborStage, 1) && hunter_ready(base)) {
                set_status(g_want_host ? "Hosting a hunt: others can join from Neko" : "Joined the hunt", true);
                go(Step::InHall);
            } else if (timed_out) {
                fail("Could not enter the hall");
            }
            break;
        case Step::InHall:
            if (state != kStateInSession) {  // the session ended under us
                go(Step::BackToHarbor);
            } else {
                quest_tick(ctx, base);
                std::lock_guard g(g_wire);
                set_status((g_want_host ? "Hosting, " : "In a session, ") + std::to_string(g_members.size()) + " hunter(s)");
            }
            break;
        case Step::Leave:
            set_status("Leaving", true);
            request_bits(base, net, kRequestLeave);
            go(Step::WaitLeave);
            break;
        case Step::WaitLeave:
            if (state == kStateHub || timed_out) go(Step::BackToHarbor);
            break;
        case Step::BackToHarbor:
            set_sub_area(base, 0);
            change_area(ctx, base, kHarborSquare);
            set_status("Not connected");
            go(Step::Idle);
            break;
        case Step::Idle:
            break;
    }
}

PPC_FUNC(swap_with_session) {
    session_tick(ctx, base);
    g_swap(ctx, base);
}

}  // namespace

// Neko's destination list (uIDLobbyOnline, docs/online.md): its Multi rows are for local play with the 3DS game, which cannot work here, so they and
// Network Mode are the online ones. Rows: 0 Moga Village, 1, 2 Solo, 3, 4 Multi, 5 Network Mode.
constexpr uint32_t kRowHost = 3, kRowJoin = 4, kRowLeave = 5;

FerryChoice ferry_choice(uint32_t row) {
    const bool idle = g_step == Step::Idle;
    if (idle) {
        if (row == kRowHost || row == kRowJoin) {
            g_requested = static_cast<int>(row == kRowHost ? NetRequest::Host : NetRequest::Join);
            return FerryChoice::Close;
        }
        return row == kRowLeave ? FerryChoice::Refuse : FerryChoice::Game;
    }
    if (row == kRowLeave) {
        g_requested = static_cast<int>(NetRequest::Leave);
        return FerryChoice::Close;
    }
    return FerryChoice::Refuse;
}

bool ferry_row_usable(uint32_t row) { return (g_step == Step::Idle) == (row != kRowLeave); }

void net_request(NetRequest request) { g_requested = static_cast<int>(request); }

void net_set_toast(void (*toast)(const std::string&)) { g_toast = toast; }

void net_configure(bool lan, const std::string& host_address) {
    g_want_lan = lan;
    in_addr parsed{};
    g_want_direct_ip = !host_address.empty() && inet_pton(AF_INET, host_address.c_str(), &parsed) == 1 ? parsed.s_addr : 0;
}

std::string net_status() {
    std::lock_guard g(g_status_lock);
    return g_status;
}

bool net_can_request(NetRequest request) {
    const bool idle = g_step == Step::Idle;
    return request == NetRequest::Leave ? !idle : idle;
}

void register_net_backend_tick() {
    g_swap = ppc::resolve_import("gx2.GX2SwapScanBuffers");
    if (g_swap) ppc::register_import("gx2.GX2SwapScanBuffers", &swap_with_session);
}

void register_net_backend(uint8_t* base, const std::vector<std::string>& fp_imports) {
    register_fp(fp_imports);
    set_ferry_choice(&ferry_choice, &ferry_row_usable);
    set_text(kFacilityText, 0x239, "Online");
    // the callers tell apart the rows that share a text (0x026AD480)
    set_text(kFacilityText, 0x23C, "Host a Hunt", 0x026AD568);
    set_text(kFacilityText, 0x23D, "Join a Hunt", 0x026AD594);
    set_text(kFacilityText, 0x24C, "Leave Online", 0x026AD5C0);
    replace_slot(base, kSnowContext, 12, &context_unique_id);
    replace_slot(base, kSnowContext, 34, &context_sign_in);
    replace_slot(base, kSnowContext, 35, &context_sign_out);
    replace_slot(base, kSnowSession, 11, &session_update);
    replace_slot(base, kSnowSession, 29, &session_create);
    replace_slot(base, kSnowSession, 30, &session_search);
    replace_slot(base, kSnowSession, 31, &session_join);
    replace_slot(base, kSnowSession, 32, &session_leave);
    replace_slot(base, kSnowP2p, 11, &p2p_update);
    replace_slot(base, kSnowP2p, 13, &p2p_send);
    replace_slot(base, kSnowP2p, 14, &p2p_close);
    replace_slot(base, kSnowP2p, 15, &p2p_open);
}

}  // namespace mh3u::game
