// h264.rpl HLE: the Wii U H.264 decoder, backed by the Windows Media Foundation H.264 decoder (no extra
// dependency) on Windows and by FFmpeg's libavcodec elsewhere. Used by the game's movie player (nativeCafe/movie/*.mp4: 1920x1080 High@4.1); the game demuxes the
// MP4 itself and passes one access unit per H264DECSetBitstream / H264DECExecute.
//
// Interface (behaviour as in Cemu's H264Dec.cpp, MPL-2.0, read for reference):
// - H264DECExecute queues the access unit with the frame buffer to decode into and returns 0x80 | 100; each decoded
//   picture is delivered through the output callback set with H264DECSetParam_FPTR_OUTPUT as
//   {frameCount = 1, H264DECFrameOutput** results, userMemory}. H264DECEnd drains the remaining pictures.
// - H264DECFrameOutput (0x78 bytes): result 100 +0, timestamp (double) +8, width +0x10, height +0x14, bytesPerRow
//   +0x18, cropEnable +0x1C, crop top/bottom/left/right +0x20..+0x2C, imagePtr +0x44.
// - Pictures are NV12 in the game's buffer: Y rows at pitch = width rounded up to 256, interleaved UV at
//   pitch * height. Height is the coded height (1088 for 1080p) with the crop reported.
// If the decoder is unavailable, pictures are delivered black so movies still play out in time.
#include "hle.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <codecapi.h>
#include <strmif.h>
#else
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}
#endif

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace mh3u::os {
namespace {

constexpr uint32_t kSuccess = 0, kInvalidParam = 0x1010000;
constexpr uint32_t kExecuteOk = 0x80 | 100;
constexpr uint32_t kFrameOutputSize = 0x78;

#ifdef _WIN32
template <class T>
void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

bool com_ready() {
    thread_local bool done = false, ok = false;
    if (!done) {
        done = true;
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ok = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
        static std::once_flag mf;
        std::call_once(mf, [] { MFStartup(MF_VERSION, MFSTARTUP_LITE); });
    }
    return ok;
}
#endif

struct Pending {
    double timestamp;
    uint32_t framebuffer;
};

struct Decoder {
    uint32_t output_fn = 0, user_memory = 0;
    bool per_frame = false;
    uint32_t bitstream = 0, length = 0;
    double timestamp = 0;
    uint32_t guest_block = 0;  // callback struct (0x10) + result pointer + frame output record
#ifdef _WIN32
    IMFTransform* mft = nullptr;
    bool output_type = false;
    uint32_t width = 0, height = 0, stride = 0;
    MFVideoArea aperture{};
    bool has_aperture = false;
#else
    AVCodecContext* mft = nullptr;      // low delay, no reordering, B pictures skipped: I and P pictures at once
    AVCodecContext* reorder = nullptr;  // one picture of reordering: each (non-reference) B picture at once
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
#endif
    bool failed = false;
    int64_t next_time = 0;
    std::map<int64_t, Pending> pending;  // decoder time stamp (a sequence number) -> the Execute call it came from
    std::vector<uint8_t> annexb;
    uint32_t frames_out = 0;
};

std::mutex g_m;
std::map<uint32_t, std::unique_ptr<Decoder>> g_decoders;  // by the game's work-memory address

Decoder* find(uint32_t mem) {
    auto it = g_decoders.find(mem);
    return it == g_decoders.end() ? nullptr : it->second.get();
}

#ifdef _WIN32
bool set_output_type(Decoder& d);

bool create_mft(Decoder& d) {
    if (!com_ready()) return false;
    if (FAILED(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d.mft)))) return false;
    // Low-latency mode: each picture is output as soon as it is decoded (decode order, no reorder delay). That is
    // what the Wii U decoder does with OUTPUT_PER_FRAME: the game feeds the next access unit only after this one's
    // picture arrives, and orders pictures for display by their timestamps itself.
    ICodecAPI* codec = nullptr;
    if (SUCCEEDED(d.mft->QueryInterface(IID_PPV_ARGS(&codec)))) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_UI4;
        v.ulVal = 1;
        codec->SetValue(&CODECAPI_AVLowLatencyMode, &v);
        codec->SetValue(&CODECAPI_AVDecNumWorkerThreads, &v);  // decode within the calls, not on worker threads
        release(codec);
    }
    IMFAttributes* attr = nullptr;
    if (SUCCEEDED(d.mft->GetAttributes(&attr))) {
        attr->SetUINT32(MF_LOW_LATENCY, TRUE);
        release(attr);
    }
    IMFMediaType* in = nullptr;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    const HRESULT hr = d.mft->SetInputType(0, in, 0);
    release(in);
    if (FAILED(hr)) {
        release(d.mft);
        return false;
    }
    set_output_type(d);  // NV12 now (the decoder reports the real size with a stream change later)
    d.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    d.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

// Picks NV12 output and reads the picture geometry (after MF_E_TRANSFORM_STREAM_CHANGE).
bool set_output_type(Decoder& d) {
    for (DWORD i = 0;; ++i) {
        IMFMediaType* t = nullptr;
        if (FAILED(d.mft->GetOutputAvailableType(0, i, &t))) return false;
        GUID sub{};
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        if (sub == MFVideoFormat_NV12 && SUCCEEDED(d.mft->SetOutputType(0, t, 0))) {
            UINT32 w = 0, h = 0;
            MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &w, &h);
            d.width = w;
            d.height = h;
            UINT32 stride = 0;
            d.stride = SUCCEEDED(t->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ? stride : w;
            d.has_aperture = SUCCEEDED(t->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&d.aperture),
                                                   sizeof d.aperture, nullptr));
            release(t);
            d.output_type = true;
            return true;
        }
        release(t);
    }
}

#endif

// Calls the game's output function for one picture (on the calling guest thread).
void deliver(ppc::Context& ctx, uint8_t* base, Decoder& d, const Pending& p, uint32_t width, uint32_t height,
             const uint32_t crop[4]) {
    if (!d.output_fn) return;
    if (!d.guest_block) d.guest_block = rt().mem->alloc_hle(0x10 + kFrameOutputSize, 16);
    const uint32_t cb = d.guest_block, rec = d.guest_block + 0x10;
    std::memset(base + rec, 0, kFrameOutputSize);
    ppc::st32(base, rec, 100);
    uint64_t ts;
    std::memcpy(&ts, &p.timestamp, 8);
    ppc::st64(base, rec + 0x08, ts);
    ppc::st32(base, rec + 0x10, width);
    ppc::st32(base, rec + 0x14, height);
    ppc::st32(base, rec + 0x18, (width + 0xFF) & ~0xFFu);
    const bool crop_on = crop[0] || crop[1] || crop[2] || crop[3];
    ppc::st32(base, rec + 0x1C, crop_on ? 1 : 0);
    for (int i = 0; i < 4; ++i) ppc::st32(base, rec + 0x20 + 4 * i, crop[i]);
    ppc::st32(base, rec + 0x44, p.framebuffer);
    ppc::st32(base, cb, 1);
    ppc::st32(base, cb + 4, cb + 0xC);
    ppc::st32(base, cb + 8, d.user_memory);
    ppc::st32(base, cb + 0xC, rec);
    ppc::Context c = ctx;
    c.r[3] = cb;
    ppc::call_indirect(c, base, d.output_fn);
    ++d.frames_out;
}

// Black NV12 picture (fallback when there is no decoder).
void deliver_black(ppc::Context& ctx, uint8_t* base, Decoder& d, const Pending& p) {
    const uint32_t w = 1920, h = 1088, pitch = (w + 0xFF) & ~0xFFu;
    if (p.framebuffer) {
        std::memset(base + p.framebuffer, 16, size_t{pitch} * h);
        std::memset(base + p.framebuffer + size_t{pitch} * h, 128, size_t{pitch} * h / 2);
    }
    const uint32_t crop[4] = {0, 8, 0, 0};
    deliver(ctx, base, d, p, w, h, crop);
}

#ifdef _WIN32
// Pulls every picture the decoder has ready and delivers it.
void drain(ppc::Context& ctx, uint8_t* base, Decoder& d) {
    for (;;) {
        MFT_OUTPUT_STREAM_INFO info{};
        d.mft->GetOutputStreamInfo(0, &info);
        IMFSample* sample = nullptr;
        IMFMediaBuffer* buf = nullptr;
        const bool provides = info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES);
        if (!provides) {
            MFCreateSample(&sample);
            MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize, 1920 * 1088 * 3 / 2), &buf);
            sample->AddBuffer(buf);
        }
        MFT_OUTPUT_DATA_BUFFER out{};
        out.pSample = sample;
        DWORD status = 0;
        const HRESULT hr = d.mft->ProcessOutput(0, 1, &out, &status);
        if (out.pEvents) out.pEvents->Release();
        if (rt().trace_imports && d.next_time <= 4)
            std::fprintf(stderr, "  h264: ProcessOutput hr %08lX (output type %d, %ux%u)\n", static_cast<unsigned long>(hr),
                         d.output_type, d.width, d.height);
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE || (hr == MF_E_TRANSFORM_TYPE_NOT_SET && !d.output_type)) {
            release(buf);
            release(sample);
            if (!set_output_type(d)) {
                d.failed = true;
                return;
            }
            continue;
        }
        if (FAILED(hr)) {  // MF_E_TRANSFORM_NEED_MORE_INPUT or an error
            release(buf);
            release(sample);
            return;
        }
        IMFSample* got = out.pSample;
        LONGLONG time = 0;
        got->GetSampleTime(&time);
        IMFMediaBuffer* cbuf = nullptr;
        got->ConvertToContiguousBuffer(&cbuf);
        BYTE* data = nullptr;
        DWORD len = 0;
        // Pictures come out in decode order, one per access unit, but the decoder re-stamps sample times in display
        // order, so a picture belongs to the oldest pending access unit, not to the one with its sample time.
        (void)time;
        auto it = d.pending.begin();
        if (cbuf && it != d.pending.end() && SUCCEEDED(cbuf->Lock(&data, nullptr, &len))) {
            const Pending p = it->second;
            const uint32_t w = d.width, h = d.height, pitch = (w + 0xFF) & ~0xFFu;
            const uint32_t src_stride = d.stride ? d.stride : w;
            uint8_t* dst = base + p.framebuffer;
            if (p.framebuffer && size_t{src_stride} * h * 3 / 2 <= len) {
                for (uint32_t y = 0; y < h; ++y) std::memcpy(dst + size_t{y} * pitch, data + size_t{y} * src_stride, w);
                const uint8_t* uv = data + size_t{src_stride} * h;
                for (uint32_t y = 0; y < h / 2; ++y)
                    std::memcpy(dst + size_t{pitch} * h + size_t{y} * pitch, uv + size_t{y} * src_stride, w);
            }
            cbuf->Unlock();
            uint32_t crop[4] = {};
            if (d.has_aperture) {
                crop[0] = static_cast<uint32_t>(d.aperture.OffsetY.value);
                crop[1] = h - crop[0] - static_cast<uint32_t>(d.aperture.Area.cy);
                crop[2] = static_cast<uint32_t>(d.aperture.OffsetX.value);
                crop[3] = w - crop[2] - static_cast<uint32_t>(d.aperture.Area.cx);
            }
            d.pending.erase(it);
            deliver(ctx, base, d, p, w, h, crop);
        }
        release(cbuf);
        got->Release();
        if (got != sample) release(sample);
        else sample = nullptr;
        release(buf);
    }
}
#else
// libavcodec, single-threaded. The Wii U decoder with OUTPUT_PER_FRAME returns each picture from the call that fed its
// access unit (decode order). libavcodec cannot do that on its own: without reordering it drops a picture whose POC is
// below the last one output (the movies' B pictures), and with reordering it holds back I and P pictures. Two decoders
// are fed every access unit instead: one low-delay with no reordering that skips non-reference pictures (I and P come
// out at once), one with one picture of reordering (each non-reference B comes out at once, its I / P a unit late).
// Pictures are matched to their access unit by pts; a picture the other decoder already delivered is dropped. No
// cropping: the game gets the coded picture (1088 lines for 1080p) with the crop reported, as on the console.
AVCodecContext* open_codec(bool low_delay) {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext* c = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (!c) return nullptr;
    c->thread_count = 1;
    c->apply_cropping = 0;
    if (low_delay) {
        c->flags |= AV_CODEC_FLAG_LOW_DELAY;
        c->skip_frame = AVDISCARD_NONREF;
    }
    if (avcodec_open2(c, codec, nullptr) < 0) avcodec_free_context(&c);
    return c;
}

void release_decoder(Decoder& d) {
    avcodec_free_context(&d.mft);
    avcodec_free_context(&d.reorder);
    av_frame_free(&d.frame);
    av_packet_free(&d.packet);
}

bool create_mft(Decoder& d) {
    d.mft = open_codec(true);
    d.reorder = open_codec(false);
    d.frame = av_frame_alloc();
    d.packet = av_packet_alloc();
    if (!d.mft || !d.reorder || !d.frame || !d.packet) {
        release_decoder(d);
        return false;
    }
    return true;
}

// Pulls every picture a decoder has ready and delivers those still pending (NV12 into the game's buffer).
void drain(ppc::Context& ctx, uint8_t* base, Decoder& d, AVCodecContext* codec) {
    while (avcodec_receive_frame(codec, d.frame) == 0) {
        const int64_t pts = d.frame->pts != AV_NOPTS_VALUE ? d.frame->pts : d.frame->best_effort_timestamp;
        std::unique_lock g(g_m);
        auto it = d.pending.find(pts);
        if (it == d.pending.end()) {  // delivered by the other decoder
            av_frame_unref(d.frame);
            continue;
        }
        const Pending p = it->second;
        d.pending.erase(it);
        g.unlock();
        const AVFrame* f = d.frame;
        const auto w = static_cast<uint32_t>(f->width), h = static_cast<uint32_t>(f->height), pitch = (w + 0xFF) & ~0xFFu;
        const bool yuv420 = f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVJ420P;
        if (p.framebuffer && yuv420) {
            uint8_t* dst = base + p.framebuffer;
            for (uint32_t y = 0; y < h; ++y)
                std::memcpy(dst + size_t{y} * pitch, f->data[0] + size_t{y} * static_cast<size_t>(f->linesize[0]), w);
            uint8_t* uv = dst + size_t{pitch} * h;
            for (uint32_t y = 0; y < h / 2; ++y) {
                const uint8_t* cb = f->data[1] + size_t{y} * static_cast<size_t>(f->linesize[1]);
                const uint8_t* cr = f->data[2] + size_t{y} * static_cast<size_t>(f->linesize[2]);
                uint8_t* row = uv + size_t{y} * pitch;
                for (uint32_t x = 0; x < w / 2; ++x) {
                    row[2 * x] = cb[x];
                    row[2 * x + 1] = cr[x];
                }
            }
        }
        const uint32_t crop[4] = {static_cast<uint32_t>(f->crop_top), static_cast<uint32_t>(f->crop_bottom),
                                  static_cast<uint32_t>(f->crop_left), static_cast<uint32_t>(f->crop_right)};
        av_frame_unref(d.frame);
        deliver(ctx, base, d, p, w, h, crop);
    }
}
#endif

// ---- SPS rewrite: max_num_reorder_frames = 0 ---------------------------------------------------------------------
// With OUTPUT_PER_FRAME the Wii U decoder returns each picture right after decoding it (decode order); the game puts
// them in display order by timestamp itself. The MF decoder instead delays output for B-frame reordering, sized by the
// SPS VUI. Rewriting the VUI bitstream restriction to "no reordering" makes it output immediately (the movies' B
// frames are non-reference, so decode order is still correct pixel-wise).
class BitReader {
public:
    explicit BitReader(const std::vector<uint8_t>& d) : d_(d) {}
    uint32_t bit() {
        if (pos_ >= d_.size() * 8) {
            ok_ = false;
            return 0;
        }
        const uint32_t b = (d_[pos_ >> 3] >> (7 - (pos_ & 7))) & 1;
        ++pos_;
        return b;
    }
    uint32_t bits(int n) {
        uint32_t v = 0;
        while (n--) v = v << 1 | bit();
        return v;
    }
    uint32_t ue() {
        int zeros = 0;
        while (ok_ && bit() == 0 && zeros < 32) ++zeros;
        return (zeros ? ((1u << zeros) - 1) : 0) + bits(zeros);
    }
    int32_t se() {
        const uint32_t k = ue();
        return k & 1 ? static_cast<int32_t>((k + 1) / 2) : -static_cast<int32_t>(k / 2);
    }
    size_t pos() const { return pos_; }
    bool ok() const { return ok_; }

private:
    const std::vector<uint8_t>& d_;
    size_t pos_ = 0;
    bool ok_ = true;
};

class BitWriter {
public:
    void bit(uint32_t b) {
        if ((n_ & 7) == 0) d_.push_back(0);
        if (b) d_.back() |= static_cast<uint8_t>(0x80 >> (n_ & 7));
        ++n_;
    }
    void bits(uint32_t v, int n) {
        while (n--) bit((v >> n) & 1);
    }
    void ue(uint32_t v) {
        const uint64_t x = uint64_t{v} + 1;
        int len = 0;
        while ((x >> len) > 1) ++len;
        bits(0, len);
        for (int i = len; i >= 0; --i) bit(static_cast<uint32_t>((x >> i) & 1));
    }
    std::vector<uint8_t> d_;
    size_t n_ = 0;
};

std::vector<uint8_t> unescape(const uint8_t* p, size_t n) {
    std::vector<uint8_t> r;
    for (size_t i = 0; i < n; ++i) {
        if (i >= 2 && p[i] == 3 && p[i - 1] == 0 && p[i - 2] == 0 && r.size() >= 2 && r[r.size() - 1] == 0 &&
            r[r.size() - 2] == 0)
            continue;  // emulation prevention byte
        r.push_back(p[i]);
    }
    return r;
}
std::vector<uint8_t> escape(const std::vector<uint8_t>& r) {
    std::vector<uint8_t> e;
    int zeros = 0;
    for (uint8_t b : r) {
        if (zeros >= 2 && b <= 3) {
            e.push_back(3);
            zeros = 0;
        }
        e.push_back(b);
        zeros = b == 0 ? zeros + 1 : 0;
    }
    return e;
}

void skip_hrd(BitReader& r) {
    const uint32_t cpb = r.ue();
    r.bits(8);
    for (uint32_t i = 0; i <= cpb && r.ok(); ++i) {
        r.ue();
        r.ue();
        r.bit();
    }
    r.bits(20);
}

// RBSP (without the NAL header byte) of an SPS -> the same SPS with bitstream_restriction saying `reorder` frames of
// reordering (0 for the decoders that must output in decode order). Returns empty if the SPS could not be parsed.
std::vector<uint8_t> sps_with_reorder(const std::vector<uint8_t>& rbsp, uint32_t reorder) {
    BitReader r(rbsp);
    const uint32_t profile = r.bits(8);
    r.bits(16);  // constraint flags, level
    r.ue();      // sps id
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 ||
        profile == 86 || profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134) {
        const uint32_t chroma = r.ue();
        if (chroma == 3) r.bit();
        r.ue();
        r.ue();
        r.bit();
        if (r.bit()) {  // seq_scaling_matrix_present
            for (int i = 0; i < (chroma == 3 ? 12 : 8); ++i) {
                if (!r.bit()) continue;
                int32_t last = 8, next = 8;
                for (int j = 0; j < (i < 6 ? 16 : 64) && r.ok(); ++j) {
                    if (next) next = (last + r.se() + 256) % 256;
                    last = next ? next : last;
                }
            }
        }
    }
    r.ue();  // log2_max_frame_num
    const uint32_t poc = r.ue();
    if (poc == 0) r.ue();
    else if (poc == 1) {
        r.bit();
        r.se();
        r.se();
        const uint32_t n = r.ue();
        for (uint32_t i = 0; i < n && r.ok(); ++i) r.se();
    }
    const uint32_t refs = r.ue();
    r.bit();
    r.ue();
    r.ue();
    if (!r.bit()) r.bit();  // frame_mbs_only, mb_adaptive_frame_field
    r.bit();
    if (r.bit()) {  // frame cropping
        r.ue();
        r.ue();
        r.ue();
        r.ue();
    }
    const size_t vui_flag = r.pos();
    size_t restriction_flag = 0;
    const bool vui = r.bit();
    if (vui) {
        if (r.bit() && r.bits(8) == 255) r.bits(32);  // aspect ratio
        if (r.bit()) r.bit();                         // overscan
        if (r.bit()) {                                // video signal type
            r.bits(4);
            if (r.bit()) r.bits(24);
        }
        if (r.bit()) {  // chroma location
            r.ue();
            r.ue();
        }
        if (r.bit()) {  // timing
            r.bits(32);
            r.bits(32);
            r.bit();
        }
        const bool nal = r.bit();
        if (nal) skip_hrd(r);
        const bool vcl = r.bit();
        if (vcl) skip_hrd(r);
        if (nal || vcl) r.bit();
        r.bit();  // pic_struct_present
        restriction_flag = r.pos();
    }
    if (!r.ok()) return {};
    // copy everything before the restriction flag (or before vui_parameters_present when there is no VUI)
    BitWriter w;
    BitReader c(rbsp);
    const size_t keep = vui ? restriction_flag : vui_flag;
    for (size_t i = 0; i < keep; ++i) w.bit(c.bit());
    if (!vui) {
        w.bit(1);       // vui_parameters_present
        w.bits(0, 9);   // aspect, overscan, video signal, chroma loc, timing, nal hrd, vcl hrd, pic_struct flags
    }
    w.bit(1);  // bitstream_restriction_flag
    w.bit(1);  // motion_vectors_over_pic_boundaries
    w.ue(2);   // max_bytes_per_pic_denom
    w.ue(1);   // max_bits_per_mb_denom
    w.ue(16);  // log2_max_mv_length_horizontal
    w.ue(16);  // log2_max_mv_length_vertical
    w.ue(reorder);  // max_num_reorder_frames
    w.ue(std::max({refs, reorder, 1u}));  // max_dec_frame_buffering
    w.bit(1);  // rbsp_stop_one_bit
    while (w.n_ & 7) w.bit(0);
    return w.d_;
}

// Replaces every SPS NAL in an Annex B buffer with a version allowing `reorder` frames of reordering.
void rewrite_sps(std::vector<uint8_t>& au, uint32_t reorder = 0) {
    std::vector<size_t> starts;  // offsets of NAL headers (after start codes)
    for (size_t i = 0; i + 3 <= au.size(); ++i)
        if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) starts.push_back(i + 3);
    for (size_t k = starts.size(); k-- > 0;) {
        const size_t s = starts[k];
        if (s >= au.size() || (au[s] & 0x1F) != 7) continue;
        size_t e = k + 1 < starts.size() ? starts[k + 1] - 3 : au.size();
        while (e > s && au[e - 1] == 0) --e;  // a 4-byte start code's leading zero
        const std::vector<uint8_t> rbsp = unescape(au.data() + s + 1, e - s - 1);
        const std::vector<uint8_t> fixed = sps_with_reorder(rbsp, reorder);
        if (fixed.empty()) continue;
        std::vector<uint8_t> nal{au[s]};
        const std::vector<uint8_t> esc = escape(fixed);
        nal.insert(nal.end(), esc.begin(), esc.end());
        au.erase(au.begin() + static_cast<std::ptrdiff_t>(s), au.begin() + static_cast<std::ptrdiff_t>(e));
        au.insert(au.begin() + static_cast<std::ptrdiff_t>(s), nal.begin(), nal.end());
    }
}

// Access units may come as Annex B (start codes) or length-prefixed (MP4 sample format): normalise to Annex B.
// An access unit delimiter is appended: the MF decoder only completes a picture once it sees the next access unit
// begin, and the game wants each picture back from the Execute that fed it.
const std::vector<uint8_t>& to_annexb(Decoder& d, const uint8_t* p, uint32_t n) {
    d.annexb.clear();
    const bool start_code = n >= 4 && p[0] == 0 && p[1] == 0 && (p[2] == 1 || (p[2] == 0 && p[3] == 1));
    if (start_code) {
        d.annexb.assign(p, p + n);
    } else {
        for (uint32_t o = 0; o + 4 <= n;) {
            const uint32_t len = uint32_t{p[o]} << 24 | uint32_t{p[o + 1]} << 16 | uint32_t{p[o + 2]} << 8 | p[o + 3];
            o += 4;
            if (len > n - o) break;
            d.annexb.insert(d.annexb.end(), {0, 0, 0, 1});
            d.annexb.insert(d.annexb.end(), p + o, p + o + len);
            o += len;
        }
    }
    if (d.per_frame) rewrite_sps(d.annexb);
    d.annexb.insert(d.annexb.end(), {0, 0, 0, 1, 0x09, 0xF0});  // access unit delimiter (any slice types)
    return d.annexb;
}

// int32 H264DECMemoryRequirement(profile, level, maxWidth, maxHeight, u32* outSize)
HLE_FUNC(h264, H264DECMemoryRequirement) {
    if (!arg(ctx, 4) || arg(ctx, 2) < 32 || arg(ctx, 3) < 32 || arg(ctx, 2) > 2800 || arg(ctx, 3) > 1408) {
        ret(ctx, kInvalidParam);
        return;
    }
    // The work memory is never touched (Media Foundation decodes host-side), but the game allocates it from the same
    // heap as its 28 frame buffers (3.3 MB each at 1080p): the largest level's 0xB400000 exhausted that heap. Report
    // Cemu's level-3.2 size up to level 4.1 (the game's movies), the large one only above.
    const uint32_t level = arg(ctx, 1);
    ppc::st32(base, arg(ctx, 4), level >= 0x33 ? (0x2D000u << 10) : 0x1400000u);
    ret(ctx, kSuccess);
}
// int32 H264DECCheckMemSegmentation(void* memory, u32 size): any range is fine for us
HLE_FUNC(h264, H264DECCheckMemSegmentation) { ret(ctx, kSuccess); }
// int32 H264DECInitParam(s32 memSize, void* memory)
HLE_FUNC(h264, H264DECInitParam) {
    std::lock_guard g(g_m);
    g_decoders[arg(ctx, 1)] = std::make_unique<Decoder>();
    ret(ctx, kSuccess);
}
HLE_FUNC(h264, H264DECSetParam_FPTR_OUTPUT) {
    std::lock_guard g(g_m);
    if (Decoder* d = find(arg(ctx, 0))) d->output_fn = arg(ctx, 1);
    ret(ctx, kSuccess);
}
HLE_FUNC(h264, H264DECSetParam_OUTPUT_PER_FRAME) {
    std::lock_guard g(g_m);
    if (Decoder* d = find(arg(ctx, 0))) d->per_frame = arg(ctx, 1) != 0;
    ret(ctx, kSuccess);
}
// (memory, void** value): the user pointer handed back in every output callback
HLE_FUNC(h264, H264DECSetParam_USER_MEMORY) {
    std::lock_guard g(g_m);
    if (Decoder* d = find(arg(ctx, 0))) d->user_memory = arg(ctx, 1) ? ppc::ld32(base, arg(ctx, 1)) : 0;
    if (rt().trace_imports && arg(ctx, 1)) std::fprintf(stderr, "  H264DEC user memory %08X\n", ppc::ld32(base, arg(ctx, 1)));
    ret(ctx, kSuccess);
}
HLE_FUNC(h264, H264DECOpen) {
    std::lock_guard g(g_m);
    Decoder* d = find(arg(ctx, 0));
    if (d && !d->mft && !create_mft(*d)) {
        std::fprintf(stderr, "h264: no H.264 decoder: movies play black\n");
        d->failed = true;
    }
    ret(ctx, kSuccess);
}
// Begin: start of a stream (also after a seek): drop anything in flight
HLE_FUNC(h264, H264DECBegin) {
    std::lock_guard g(g_m);
    if (Decoder* d = find(arg(ctx, 0)); d && d->mft) {
#ifdef _WIN32
        d->mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
#else
        avcodec_flush_buffers(d->mft);
        avcodec_flush_buffers(d->reorder);
#endif
        d->pending.clear();
    }
    ret(ctx, kSuccess);
}
// int32 H264DECSetBitstream(memory, const u8* buffer, u32 length, double timestamp)
HLE_FUNC(h264, H264DECSetBitstream) {
    std::lock_guard g(g_m);
    if (Decoder* d = find(arg(ctx, 0))) {
        d->bitstream = arg(ctx, 1);
        d->length = arg(ctx, 2);
        d->timestamp = ctx.f[1].ps0;
    }
    ret(ctx, kSuccess);
}
// int32 H264DECExecute(memory, void* frameBuffer)
HLE_FUNC(h264, H264DECExecute) {
    std::unique_lock g(g_m);
    Decoder* d = find(arg(ctx, 0));
    if (!d) {
        ret(ctx, 0);
        return;
    }
    const Pending p{d->timestamp, arg(ctx, 1)};
    if (d->failed || !d->mft) {
        g.unlock();
        deliver_black(ctx, base, *d, p);
        ret(ctx, kExecuteOk);
        return;
    }
    const std::vector<uint8_t>& au = to_annexb(*d, base + d->bitstream, d->length);
    if (rt().trace_imports && d->next_time < 2) {
        std::fprintf(stderr, "  h264: AU %u bytes:", d->length);
        for (uint32_t i = 0; i < std::min<uint32_t>(d->length, 48); ++i) std::fprintf(stderr, " %02X", base[d->bitstream + i]);
        std::fprintf(stderr, "\n");
    }
#ifndef _WIN32
    const int64_t time = d->next_time++;
    d->pending[time] = p;
    std::vector<uint8_t> au_reorder = au;
    if (d->per_frame) rewrite_sps(au_reorder, 1);
    bool accepted = false;
    for (AVCodecContext* c : {d->mft, d->reorder}) {
        const std::vector<uint8_t>& data = c == d->mft ? au : au_reorder;
        d->packet->data = const_cast<uint8_t*>(data.data());
        d->packet->size = static_cast<int>(data.size());
        d->packet->pts = d->packet->dts = time;
        accepted = avcodec_send_packet(c, d->packet) >= 0 || accepted;
        d->packet->data = nullptr;
        d->packet->size = 0;
    }
    if (!accepted) d->pending.erase(time);
    g.unlock();
    // the output callback runs without our lock (it may call back into h264)
    drain(ctx, base, *d, d->mft);
    drain(ctx, base, *d, d->reorder);
    g.lock();
    const bool missing = d->per_frame && d->pending.count(time) != 0;
    if (missing) d->pending.erase(time);
    g.unlock();
    if (missing) {
        // neither decoder gave this unit's picture (a damaged unit): the game waits for it, so it gets a black one
        if (rt().trace_imports)
            std::fprintf(stderr, "  h264: no picture for unit %lld, black\n", static_cast<long long>(time));
        deliver_black(ctx, base, *d, p);
    }
    if (rt().trace_imports && d->next_time <= 8)
        std::fprintf(stderr, "  H264DECExecute #%lld: %u bytes, %u pictures out\n", static_cast<long long>(d->next_time),
                     d->length, d->frames_out);
    ret(ctx, kExecuteOk);
#else
    IMFSample* sample = nullptr;
    IMFMediaBuffer* buf = nullptr;
    MFCreateSample(&sample);
    MFCreateMemoryBuffer(static_cast<DWORD>(au.size()), &buf);
    BYTE* data = nullptr;
    buf->Lock(&data, nullptr, nullptr);
    std::memcpy(data, au.data(), au.size());
    buf->Unlock();
    buf->SetCurrentLength(static_cast<DWORD>(au.size()));
    sample->AddBuffer(buf);
    const LONGLONG time = d->next_time++ * 10000;  // sequence number as the MF timestamp (100 ns units)
    sample->SetSampleTime(time);
    d->pending[time] = p;
    HRESULT hr = d->mft->ProcessInput(0, sample, 0);
    if (hr == MF_E_NOTACCEPTING) {  // pull pictures first, then retry
        g.unlock();
        drain(ctx, base, *d);
        g.lock();
        hr = d->mft->ProcessInput(0, sample, 0);
    }
    release(buf);
    release(sample);
    if (FAILED(hr)) d->pending.erase(time);
    g.unlock();
    // the output callback runs without our lock (it may call back into h264). With OUTPUT_PER_FRAME the game waits
    // for this access unit's picture: give the decoder a moment to finish it.
    const uint32_t before = d->frames_out;
    drain(ctx, base, *d);
    for (int i = 0; d->per_frame && SUCCEEDED(hr) && d->frames_out == before && !d->pending.empty() && i < 4; ++i) {
        Sleep(1);
        drain(ctx, base, *d);
    }
    if (d->per_frame && SUCCEEDED(hr) && d->frames_out == before && !d->pending.empty()) {
        // still held (a B picture whose POC precedes one already output): drain pushes it out; the decoder keeps its
        // reference pictures and takes new input afterwards
        d->mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        drain(ctx, base, *d);
    }
    if (rt().trace_imports && d->next_time <= 8)
        std::fprintf(stderr, "  H264DECExecute #%lld: %u bytes, input %s, %u pictures out, %zu pending\n",
                     static_cast<long long>(d->next_time), d->length, SUCCEEDED(hr) ? "ok" : "rejected", d->frames_out,
                     d->pending.size());
    ret(ctx, kExecuteOk);
#endif
}
// End of stream: deliver every remaining picture
HLE_FUNC(h264, H264DECEnd) {
    Decoder* d;
    {
        std::lock_guard g(g_m);
        d = find(arg(ctx, 0));
    }
    if (d && d->mft) {
#ifdef _WIN32
        d->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        d->mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        drain(ctx, base, *d);
#else
        for (AVCodecContext* c : {d->mft, d->reorder}) {
            avcodec_send_packet(c, nullptr);  // drain mode
            drain(ctx, base, *d, c);
            avcodec_flush_buffers(c);  // and ready for another stream
        }
#endif
        d->pending.clear();
    }
    ret(ctx, kSuccess);
}
HLE_FUNC(h264, H264DECClose) {
    std::lock_guard g(g_m);
    auto it = g_decoders.find(arg(ctx, 0));
    if (it != g_decoders.end()) {
#ifdef _WIN32
        release(it->second->mft);
#else
        release_decoder(*it->second);
#endif
        g_decoders.erase(it);
    }
    ret(ctx, kSuccess);
}

}  // namespace

void register_h264(Runtime&) {
    HLE_REGISTER(h264, H264DECMemoryRequirement);
    HLE_REGISTER(h264, H264DECCheckMemSegmentation);
    HLE_REGISTER(h264, H264DECInitParam);
    HLE_REGISTER(h264, H264DECSetParam_FPTR_OUTPUT);
    HLE_REGISTER(h264, H264DECSetParam_OUTPUT_PER_FRAME);
    HLE_REGISTER(h264, H264DECSetParam_USER_MEMORY);
    HLE_REGISTER(h264, H264DECOpen);
    HLE_REGISTER(h264, H264DECBegin);
    HLE_REGISTER(h264, H264DECSetBitstream);
    HLE_REGISTER(h264, H264DECExecute);
    HLE_REGISTER(h264, H264DECEnd);
    HLE_REGISTER(h264, H264DECClose);
}

}  // namespace mh3u::os
