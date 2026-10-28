#include "zarchive.h"

#include <zstd.h>

#include <algorithm>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace mh3u::fs {
namespace {

constexpr std::uint32_t kMagic = 0x169F52D6, kVersion1 = 0x61BF3A01;
constexpr std::size_t kBlock = 64 * 1024, kBlocksPerRecord = 16, kRecordSize = 8 + 2 * kBlocksPerRecord;
constexpr std::size_t kFooterSize = 6 * 16 + 32 + 8 + 4 + 4;
constexpr std::uint32_t kNoName = 0x7FFFFFFF;

std::uint64_t be(const std::uint8_t* p, int n) {
    std::uint64_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 8) | p[i];
    return v;
}

#ifdef _WIN32
using Handle = HANDLE;
#else
using Handle = int;
#endif

bool read_at(Handle h, std::uint64_t offset, void* dst, std::size_t n) {
    auto* out = static_cast<std::uint8_t*>(dst);
    while (n) {
#ifndef _WIN32
        const ssize_t got = ::pread(h, out, std::min<std::size_t>(n, 1u << 30), static_cast<off_t>(offset));
        if (got <= 0) return false;
#else
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(offset);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(n, 1u << 30));
        DWORD got = 0;
        if (!ReadFile(h, out, want, &got, &ov) || got == 0) return false;
#endif
        out += got;
        offset += got;
        n -= got;
    }
    return true;
}

bool iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const char x = a[i] >= 'A' && a[i] <= 'Z' ? a[i] + 32 : a[i], y = b[i] >= 'A' && b[i] <= 'Z' ? b[i] + 32 : b[i];
        if (x != y) return false;
    }
    return true;
}

// One decompressed block per thread: guest threads mostly read files front to back, so consecutive reads hit it.
struct BlockCache {
    const ZArchive* owner = nullptr;
    std::uint64_t index = ~0ull;
    std::vector<std::uint8_t> data;
};
thread_local BlockCache t_cache;

class ArchiveFile final : public FileReader {
public:
    ArchiveFile(std::shared_ptr<const ZArchive> a, std::uint64_t offset, std::uint64_t size)
        : a_(std::move(a)), offset_(offset), size_(size) {}
    std::uint64_t size() const override { return size_; }
    std::size_t read(std::uint64_t offset, void* dst, std::size_t n) override {
        if (offset >= size_) return 0;
        n = static_cast<std::size_t>(std::min<std::uint64_t>(n, size_ - offset));
        return a_->read(offset_ + offset, dst, n);
    }

private:
    std::shared_ptr<const ZArchive> a_;
    std::uint64_t offset_, size_;
};

class ArchiveTree final : public Tree {
public:
    ArchiveTree(std::shared_ptr<const ZArchive> a, std::uint32_t root) : a_(std::move(a)), root_(root) {}

    std::optional<NodeInfo> stat(const std::string& path) const override {
        const auto i = a_->find(path, root_);
        if (!i) return std::nullopt;
        const auto& n = a_->node(*i);
        return NodeInfo{!n.is_file, n.is_file ? n.size : 0};
    }
    std::vector<DirEntry> list(const std::string& dir) const override {
        std::vector<DirEntry> out;
        const auto i = a_->find(dir, root_);
        if (!i || a_->node(*i).is_file) return out;
        const auto& d = a_->node(*i);
        for (std::uint32_t c = d.first; c < d.first + d.count; ++c) {
            const auto& n = a_->node(c);
            out.push_back({n.name, !n.is_file, n.is_file ? n.size : 0, 0});
        }
        return out;
    }
    std::unique_ptr<FileReader> open(const std::string& path) const override {
        const auto i = a_->find(path, root_);
        if (!i || !a_->node(*i).is_file) return nullptr;
        return std::make_unique<ArchiveFile>(a_, a_->node(*i).offset, a_->node(*i).size);
    }

private:
    std::shared_ptr<const ZArchive> a_;
    std::uint32_t root_;
};

}  // namespace

std::shared_ptr<ZArchive> ZArchive::open(const std::filesystem::path& path, std::string* error) {
    auto fail = [&](const char* why) -> std::shared_ptr<ZArchive> {
        if (error) *error = why;
        return nullptr;
    };
#ifdef _WIN32
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return fail("cannot open the file");
    std::shared_ptr<ZArchive> a(new ZArchive);
    a->file_ = h;
    LARGE_INTEGER fsize{};
    GetFileSizeEx(h, &fsize);
    const auto file_size = static_cast<std::uint64_t>(fsize.QuadPart);
#else
    const int h = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (h < 0) return fail("cannot open the file");
    std::shared_ptr<ZArchive> a(new ZArchive);
    a->fd_ = h;
    struct stat st{};
    fstat(h, &st);
    const auto file_size = static_cast<std::uint64_t>(st.st_size);
#endif
    std::uint8_t f[kFooterSize];
    if (file_size < kFooterSize || !read_at(h, file_size - kFooterSize, f, kFooterSize)) return fail("file too small");
    if (be(f + kFooterSize - 4, 4) != kMagic) return fail("not a .wua (ZArchive) file");
    if (be(f + kFooterSize - 8, 4) != kVersion1) return fail("unsupported ZArchive version");
    if (be(f + 6 * 16 + 32, 8) != file_size) return fail("truncated .wua (size in the footer differs)");
    struct Range {
        std::uint64_t offset, size;
    } sec[6];
    for (int i = 0; i < 6; ++i) {
        sec[i] = {be(f + i * 16, 8), be(f + i * 16 + 8, 8)};
        if (sec[i].offset + sec[i].size > file_size) return fail("section out of range");
    }
    const Range &data = sec[0], &records = sec[1], &names = sec[2], &tree = sec[3];
    a->data_offset_ = data.offset;
    a->data_size_ = data.size;
    a->records_.resize(static_cast<std::size_t>(records.size));
    std::vector<std::uint8_t> name_tab(static_cast<std::size_t>(names.size)), nodes(static_cast<std::size_t>(tree.size));
    if (!read_at(h, records.offset, a->records_.data(), a->records_.size()) ||
        !read_at(h, names.offset, name_tab.data(), name_tab.size()) || !read_at(h, tree.offset, nodes.data(), nodes.size()))
        return fail("cannot read the archive index");
    if (nodes.size() % 16 || nodes.empty()) return fail("bad file tree");

    a->nodes_.resize(nodes.size() / 16);
    for (std::size_t i = 0; i < a->nodes_.size(); ++i) {
        const std::uint8_t* e = nodes.data() + i * 16;
        Node& n = a->nodes_[i];
        const auto type_name = static_cast<std::uint32_t>(be(e, 4));
        n.is_file = (type_name & 0x80000000u) != 0;
        if (const std::uint32_t off = type_name & 0x7FFFFFFFu; off != kNoName) {
            if (off >= name_tab.size()) return fail("bad name offset");
            std::size_t len = name_tab[off] & 0x7F, p = off + 1;
            if (name_tab[off] & 0x80) {
                if (p >= name_tab.size()) return fail("bad name");
                len |= std::size_t{name_tab[p++]} << 7;
            }
            if (p + len > name_tab.size()) return fail("bad name");
            n.name.assign(reinterpret_cast<const char*>(name_tab.data() + p), len);
        }
        if (n.is_file) {
            n.offset = be(e + 4, 4) | (be(e + 14, 2) << 32);
            n.size = be(e + 8, 4) | (be(e + 12, 2) << 32);
            if (n.offset + n.size > (records.size / kRecordSize) * kBlocksPerRecord * kBlock)
                return fail("file outside the data stream");
        } else {
            n.first = static_cast<std::uint32_t>(be(e + 4, 4));
            n.count = static_cast<std::uint32_t>(be(e + 8, 4));
            if (std::uint64_t{n.first} + n.count > a->nodes_.size()) return fail("bad directory");
        }
    }
    if (a->nodes_[kRoot].is_file) return fail("root is not a directory");
    return a;
}

ZArchive::~ZArchive() {
#ifdef _WIN32
    if (file_) CloseHandle(static_cast<HANDLE>(file_));
#else
    if (fd_ >= 0) ::close(fd_);
#endif
}

std::optional<std::uint32_t> ZArchive::find(std::string_view path, std::uint32_t dir) const {
    std::uint32_t cur = dir;
    while (!path.empty()) {
        const std::size_t slash = path.find('/');
        const std::string_view part = path.substr(0, slash);
        path = slash == std::string_view::npos ? std::string_view{} : path.substr(slash + 1);
        if (part.empty()) continue;
        const Node& d = nodes_[cur];
        if (d.is_file) return std::nullopt;
        // children are sorted (case-insensitively) by the writer; a linear scan keeps this independent of that
        std::optional<std::uint32_t> hit;
        for (std::uint32_t c = d.first; c < d.first + d.count; ++c)
            if (iequal(nodes_[c].name, part)) {
                hit = c;
                break;
            }
        if (!hit) return std::nullopt;
        cur = *hit;
    }
    return cur;
}

bool ZArchive::block(std::uint64_t index, std::vector<std::uint8_t>& out) const {
    const std::uint64_t rec = index / kBlocksPerRecord;
    if ((rec + 1) * kRecordSize > records_.size()) return false;
    const std::uint8_t* r = records_.data() + rec * kRecordSize;
    std::uint64_t offset = be(r, 8);
    const std::size_t slot = static_cast<std::size_t>(index % kBlocksPerRecord);
    for (std::size_t i = 0; i < slot; ++i) offset += be(r + 8 + 2 * i, 2) + 1;
    const std::size_t csize = static_cast<std::size_t>(be(r + 8 + 2 * slot, 2)) + 1;
    out.resize(kBlock);
    if (csize == kBlock)  // stored uncompressed
        return read_at(handle(), data_offset_ + offset, out.data(), kBlock);
    thread_local std::vector<std::uint8_t> packed;
    thread_local ZSTD_DCtx* dctx = ZSTD_createDCtx();
    packed.resize(csize);
    if (!read_at(handle(), data_offset_ + offset, packed.data(), csize)) return false;
    const std::size_t n = ZSTD_decompressDCtx(dctx, out.data(), kBlock, packed.data(), csize);
    if (ZSTD_isError(n)) return false;
    std::fill(out.begin() + static_cast<std::ptrdiff_t>(n), out.end(), std::uint8_t{0});
    return true;
}

std::size_t ZArchive::read(std::uint64_t offset, void* dst, std::size_t n) const {
    auto* out = static_cast<std::uint8_t*>(dst);
    std::size_t done = 0;
    BlockCache& c = t_cache;
    while (done < n) {
        const std::uint64_t at = offset + done, index = at / kBlock;
        if (c.owner != this || c.index != index) {
            c.owner = this;
            c.index = ~0ull;
            if (!block(index, c.data)) break;
            c.index = index;
        }
        const std::size_t in = static_cast<std::size_t>(at % kBlock), take = std::min(kBlock - in, n - done);
        std::memcpy(out + done, c.data.data() + in, take);
        done += take;
    }
    return done;
}

std::shared_ptr<Tree> ZArchive::subtree(std::string_view folder) {
    const auto i = find(folder);
    if (!i || nodes_[*i].is_file) return nullptr;
    return std::make_shared<ArchiveTree>(shared_from_this(), *i);
}

}  // namespace mh3u::fs
