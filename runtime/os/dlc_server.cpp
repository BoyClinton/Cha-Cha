// The DLC server, local: the game's DLC screen (main menu > DLC, aDLContents) downloads its index and quests over
// HTTP through cNetStorage (nlibcurl's easy interface; docs/research-log.md, "DLC download"), first a login
// (POST https://goshawk.capcom.co.jp/SSL/wiiu/mh3gu_us/login.cgi), then files under
// http://goshawk.capcom.co.jp/3ds/mh3g/. Capcom's server is gone; here every request is answered by a responder in
// Cha-Cha (boot code) with the file's plain bytes - the login and the encryption are done here - and nn_ac / nn_act report a connected console with a network account, so the game's own
// screens - list, download, progress - work. Only cNetStorage's easy interface is served; NEX (the multi interface,
// sockets) stays offline (offline.cpp).
//
// curl_easy_*: handles are small guest blocks; setopt records URL, POST data, the write and progress callbacks;
// perform asks the responder, then hands the body to the game's write callback (WRITEFUNCTION(ptr, 1, n, WRITEDATA))
// in pieces, calling PROGRESSFUNCTION(PROGRESSDATA, total, now, 0, 0) (doubles in f1-f4) between them, a short
// pause each, so the game's progress bar moves as for a real download. getinfo answers RESPONSE_CODE.
#include "hle.h"

#include "game_image.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mh3u::os {
namespace {

// curl options and infos the game sets (curl.h numbering: LONG 0, OBJECTPOINT 10000, FUNCTIONPOINT 20000, OFF_T
// 30000; Nintendo's NSSL options are 210 / 211)
constexpr uint32_t kOptWriteData = 10001, kOptUrl = 10002, kOptPostFields = 10015, kOptPostFieldSize = 60,
                   kOptNoProgress = 43, kOptWriteFunction = 20011, kOptProgressFunction = 20056,
                   kOptProgressData = 10057, kOptCopyPostFields = 10165;
constexpr uint32_t kInfoResponseCode = 0x200002, kInfoContentLength = 0x30000F;
constexpr uint32_t kCurleOk = 0, kCurleWriteError = 23, kCurleAborted = 42;
constexpr size_t kChunk = 16 * 1024;

DlcResponder g_responder;
bool g_log = false;

// The wire format (cNetStorage::vf15 / vf18, docs/research-log.md "DLC server protocol"): login.cgi answers
// [u16 BE key length][key][u16 BE length][base URL or nothing]; every file after it is
// Blowfish-ECB(key)(plaintext + SHA-1(plaintext), padded to 8) + [u32 BE plaintext length + 20]. MtCipher's Blowfish
// reads each 8-byte block as two little-endian words and has a quirk in its S-box schedule (below); its P / S start
// tables are the game's own (in guest memory, the standard pi digits), so they are not copied here. Checked against
// the game's own MtCipher and SHA-1 (called on our bytes, 2026-11-05).
constexpr char kKey[] = "Cha-Cha local DLC";
constexpr uint32_t kBlowfishP = 0x102D8160, kBlowfishS = 0x102D81A8;

struct Blowfish {
    uint32_t p[18], s[4][256];
    uint32_t f(uint32_t x) const { return ((s[0][x >> 24] + s[1][(x >> 16) & 0xFF]) ^ s[2][(x >> 8) & 0xFF]) + s[3][x & 0xFF]; }
    void encrypt(uint32_t& l, uint32_t& r) const {
        for (int i = 0; i < 16; ++i) {
            l ^= p[i];
            r ^= f(l);
            std::swap(l, r);
        }
        std::swap(l, r);
        r ^= p[16];
        l ^= p[17];
    }
    Blowfish(const uint8_t* base, const char* key, size_t n) {
        for (int i = 0; i < 18; ++i) p[i] = ppc::ld32(base, kBlowfishP + 4 * i);
        for (int i = 0; i < 4 * 256; ++i) s[i / 256][i % 256] = ppc::ld32(base, kBlowfishS + 4 * i);
        for (size_t i = 0, k = 0; i < 18; ++i) {
            uint32_t w = 0;
            for (int j = 0; j < 4; ++j, k = (k + 1) % n) w = w << 8 | static_cast<uint8_t>(key[k]);
            p[i] ^= w;
        }
        uint32_t l = 0, r = 0;
        for (int i = 0; i < 18; i += 2) encrypt(l, r), p[i] = l, p[i + 1] = r;
        // MtCipher's S loop steps one entry while writing two (FUN_02DBB9AC): every entry gets its own block, the
        // second word lands on the next entry until that one is written; box 3's last word falls past the end
        uint32_t* flat = &s[0][0];
        for (int i = 0; i < 4 * 256; ++i) {
            encrypt(l, r);
            flat[i] = l;
            if (i + 1 < 4 * 256) flat[i + 1] = r;
        }
    }
};

uint32_t le32(const uint8_t* b) { return b[0] | b[1] << 8 | b[2] << 16 | static_cast<uint32_t>(b[3]) << 24; }
void put_le32(uint8_t* b, uint32_t v) { for (int i = 0; i < 4; ++i) b[i] = static_cast<uint8_t>(v >> 8 * i); }

std::vector<uint8_t> wrap(const uint8_t* base, const std::vector<uint8_t>& plain) {
    std::vector<uint8_t> out = plain;
    const std::string hex = fs::sha1_hex(plain.data(), plain.size());
    for (size_t i = 0; i < 40; i += 2) out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    const uint32_t length = static_cast<uint32_t>(out.size());
    out.resize((out.size() + 7) & ~size_t(7));
    static const Blowfish bf(base, kKey, sizeof kKey - 1);
    for (size_t i = 0; i < out.size(); i += 8) {
        uint32_t l = le32(&out[i]), r = le32(&out[i + 4]);
        bf.encrypt(l, r);
        put_le32(&out[i], l);
        put_le32(&out[i + 4], r);
    }
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<uint8_t>(length >> 8 * i));
    return out;
}

struct Easy {
    std::string url;
    uint32_t post = 0, post_size = 0;
    uint32_t write_fn = 0, write_data = 0, progress_fn = 0, progress_data = 0;
    bool no_progress = true;
    int status = 0;
    size_t length = 0;
    uint32_t buffer = 0;  // guest buffer the body is handed over in
};
std::mutex g_m;
std::map<uint32_t, Easy> g_easy;

void reg(const char* lib, const char* name, ppc::GuestFunction fn) {
    ppc::register_import((std::string(lib) + "." + name).c_str(), fn);
}

void easy_init(ppc::Context& c, uint8_t*) {
    const uint32_t h = rt().mem->alloc_hle(16, 16);
    std::lock_guard g(g_m);
    g_easy[h] = Easy{};
    ret(c, h);
}

void easy_setopt(ppc::Context& c, uint8_t*) {
    const uint32_t h = arg(c, 0), opt = arg(c, 1), v = arg(c, 2);
    std::lock_guard g(g_m);
    auto it = g_easy.find(h);
    if (it == g_easy.end()) return ret(c, 2);  // CURLE_FAILED_INIT
    Easy& e = it->second;
    switch (opt) {
        case kOptUrl: e.url = guest_cstr(v); break;
        case kOptPostFields:
        case kOptCopyPostFields: e.post = v; break;
        case kOptPostFieldSize: e.post_size = v; break;
        case kOptWriteFunction: e.write_fn = v; break;
        case kOptWriteData: e.write_data = v; break;
        case kOptProgressFunction: e.progress_fn = v; break;
        case kOptProgressData: e.progress_data = v; break;
        case kOptNoProgress: e.no_progress = v != 0; break;
        default:
            if (g_log) std::printf("dlc server: setopt %u = 0x%08X\n", opt, v);
            break;
    }
    ret(c, kCurleOk);
}

// a guest function (fn) with integer args and up to four doubles; returns r3
uint32_t call(ppc::Context& c, uint8_t* b, uint32_t fn, std::initializer_list<uint32_t> args, std::initializer_list<double> fargs = {}) {
    const ppc::Context saved = c;
    int i = 3;
    for (uint32_t a : args) c.r[i++] = a;
    i = 1;
    for (double f : fargs) c.f[i++].ps0 = f;
    ppc::call_indirect(c, b, fn);
    const uint32_t r = c.r[3];
    c = saved;
    return r;
}

void easy_perform(ppc::Context& c, uint8_t* b) {
    const uint32_t h = arg(c, 0);
    Easy e;
    {
        std::lock_guard g(g_m);
        auto it = g_easy.find(h);
        if (it == g_easy.end()) return ret(c, 2);
        if (!it->second.buffer) it->second.buffer = rt().mem->alloc_hle(kChunk, 64);
        e = it->second;
    }
    std::vector<uint8_t> post;
    if (e.post) {
        const uint32_t n = e.post_size ? e.post_size : static_cast<uint32_t>(std::strlen(reinterpret_cast<const char*>(b + e.post)));
        post.assign(b + e.post, b + e.post + n);
    }
    DlcResponse r;
    if (e.url.find("/login.cgi") != std::string::npos) {  // the session key; no other base URL
        r.status = 200;
        const uint16_t n = sizeof kKey - 1;
        r.body = {static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(n)};
        r.body.insert(r.body.end(), kKey, kKey + n);
        r.body.insert(r.body.end(), {0, 0});
    } else {
        if (g_responder) r = g_responder(e.url, post);
        if (r.status == 200) r.body = wrap(b, r.body);
    }
    if (g_log) std::printf("dlc server: %s %s -> %d, %zu bytes\n", e.post ? "POST" : "GET", e.url.c_str(), r.status, r.body.size());
    {
        std::lock_guard g(g_m);
        g_easy[h].status = r.status;
        g_easy[h].length = r.body.size();
    }
    const double total = static_cast<double>(r.body.size());
    const auto progress = [&](double now) {
        return !e.no_progress && e.progress_fn && call(c, b, e.progress_fn, {e.progress_data}, {total, now, 0.0, 0.0}) != 0;
    };
    if (progress(0)) return ret(c, kCurleAborted);
    // in pieces, each with a short pause (or waiting for its share of a job), so the game's progress shows
    const size_t pieces = static_cast<size_t>(std::max(1, r.pieces));
    const size_t piece = std::max<size_t>(1, std::min(kChunk, (r.body.size() + pieces - 1) / pieces));
    for (size_t off = 0; off < r.body.size(); off += piece) {
        const size_t n = std::min(piece, r.body.size() - off);
        if (r.progress)
            while (r.progress() < static_cast<double>(off + n) / static_cast<double>(r.body.size())) {
                if (r.cancelled && r.cancelled()) {
                    if (g_log) std::printf("dlc server: %s called off\n", e.url.c_str());
                    return ret(c, kCurleAborted);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        std::memcpy(b + e.buffer, r.body.data() + off, n);
        if (e.write_fn) {
            const uint32_t took = call(c, b, e.write_fn, {e.buffer, 1, static_cast<uint32_t>(n), e.write_data});
            if (took != n) {
                if (g_log) std::printf("dlc server: write callback took %u of %zu\n", took, n);
                return ret(c, kCurleWriteError);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(r.delay_ms));
        if (progress(static_cast<double>(off + n))) return ret(c, kCurleAborted);
    }
    ret(c, kCurleOk);
}

void easy_getinfo(ppc::Context& c, uint8_t* b) {
    const uint32_t h = arg(c, 0), info = arg(c, 1), out = arg(c, 2);
    std::lock_guard g(g_m);
    auto it = g_easy.find(h);
    if (it == g_easy.end() || !out) return ret(c, 2);
    if (g_log) std::printf("dlc server: getinfo 0x%X\n", info);
    if (info == kInfoResponseCode) ppc::st32(b, out, static_cast<uint32_t>(it->second.status));
    else if (info == kInfoContentLength) {
        const double d = static_cast<double>(it->second.length);
        uint64_t u;
        std::memcpy(&u, &d, 8);
        ppc::st32(b, out, static_cast<uint32_t>(u >> 32));
        ppc::st32(b, out + 4, static_cast<uint32_t>(u));
    }
    ret(c, kCurleOk);
}

void easy_reset(ppc::Context& c, uint8_t*) {
    std::lock_guard g(g_m);
    if (auto it = g_easy.find(arg(c, 0)); it != g_easy.end()) {
        const uint32_t buffer = it->second.buffer;
        it->second = Easy{};
        it->second.buffer = buffer;
    }
    ret(c, 0);
}

void easy_cleanup(ppc::Context& c, uint8_t*) {
    std::lock_guard g(g_m);
    g_easy.erase(arg(c, 0));  // its guest blocks stay (HLE allocations are not freed)
    ret(c, 0);
}

}  // namespace

void register_dlc_server(Runtime&, DlcResponder responder, bool log) {
    g_responder = std::move(responder);
    g_log = log;
    reg("nlibcurl", "curl_easy_init", easy_init);
    reg("nlibcurl", "curl_easy_setopt", easy_setopt);
    reg("nlibcurl", "curl_easy_perform", easy_perform);
    reg("nlibcurl", "curl_easy_getinfo", easy_getinfo);
    reg("nlibcurl", "curl_easy_reset", easy_reset);
    reg("nlibcurl", "curl_easy_cleanup", easy_cleanup);
    reg("nlibcurl", "curl_slist_append", [](ppc::Context& c, uint8_t*) { ret(c, arg(c, 0) ? arg(c, 0) : rt().mem->alloc_hle(16, 16)); });
    // NSSL: contexts for the login's HTTPS (nothing is encrypted: the responder answers)
    reg("nsysnet", "NSSLCreateContext", [](ppc::Context& c, uint8_t*) {
        static uint32_t next = 1;
        ret(c, next++);
    });
    for (const char* n : {"NSSLDestroyContext", "NSSLAddServerPKI", "NSSLAddServerPKIGroups"})
        reg("nsysnet", n, [](ppc::Context& c, uint8_t*) { ret(c, 0); });
    // nn_ac: connected
    reg("nn_ac", "Connect__Q2_2nn2acFv", [](ppc::Context& c, uint8_t*) { ret(c, 0); });
    reg("nn_ac", "IsApplicationConnected__Q2_2nn2acFPb", [](ppc::Context& c, uint8_t* b) {
        if (arg(c, 0)) b[arg(c, 0)] = 1;
        ret(c, 0);
    });
    reg("nn_ac", "GetAssignedAddress__Q2_2nn2acFPUl", [](ppc::Context& c, uint8_t* b) {
        if (arg(c, 0)) ppc::st32(b, arg(c, 0), 0x7F000001);
        ret(c, 0);
    });
    reg("nn_ac", "GetLastErrorCode__Q2_2nn2acFPUi", [](ppc::Context& c, uint8_t* b) {
        if (arg(c, 0)) ppc::st32(b, arg(c, 0), 0);
        ret(c, 0);
    });
    // nn_act: a network account, and a service token for the login
    reg("nn_act", "HasNfsAccount__Q2_2nn3actFv", [](ppc::Context& c, uint8_t*) { ret(c, 1); });
    reg("nn_act", "GetAccountId__Q2_2nn3actFPc", [](ppc::Context& c, uint8_t* b) {
        if (arg(c, 0)) std::memcpy(b + arg(c, 0), "ChaCha", 7);
        ret(c, 0);
    });
    reg("nn_act", "AcquireIndependentServiceToken__Q2_2nn3actFPcPCc", [](ppc::Context& c, uint8_t* b) {
        if (g_log) std::printf("dlc server: independent service token for client %s\n", guest_cstr(arg(c, 1)).c_str());
        if (arg(c, 0)) std::memcpy(b + arg(c, 0), "chacha-local-dlc", 17);
        ret(c, 0);
    });
}

}  // namespace mh3u::os
