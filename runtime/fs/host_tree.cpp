// Tree over a host directory: positional reads (ReadFile with an OVERLAPPED offset on Windows, pread elsewhere), so
// one open file can be read from several guest threads without a shared file pointer.
#include "tree.h"

#include <algorithm>
#include <cstring>

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

namespace stdfs = std::filesystem;

#ifdef _WIN32
class HostFile final : public FileReader {
public:
    HostFile(HANDLE h, std::uint64_t size) : h_(h), size_(size) {}
    ~HostFile() override { CloseHandle(h_); }
    std::uint64_t size() const override { return size_; }
    std::size_t read(std::uint64_t offset, void* dst, std::size_t n) override {
        std::size_t done = 0;
        auto* out = static_cast<std::uint8_t*>(dst);
        while (done < n && offset + done < size_) {
            OVERLAPPED ov{};
            const std::uint64_t at = offset + done;
            ov.Offset = static_cast<DWORD>(at);
            ov.OffsetHigh = static_cast<DWORD>(at >> 32);
            const DWORD want = static_cast<DWORD>(std::min<std::size_t>(n - done, 1u << 30));
            DWORD got = 0;
            if (!ReadFile(h_, out + done, want, &got, &ov) || got == 0) break;
            done += got;
        }
        return done;
    }

private:
    HANDLE h_;
    std::uint64_t size_;
};
#else
class HostFile final : public FileReader {
public:
    HostFile(int fd, std::uint64_t size) : fd_(fd), size_(size) {}
    ~HostFile() override { ::close(fd_); }
    std::uint64_t size() const override { return size_; }
    // through a bounce buffer: dst is often guest memory, which the write tracking may keep read-only, and the kernel
    // does not fault on its own writes (pread into such a page fails with EFAULT instead)
    std::size_t read(std::uint64_t offset, void* dst, std::size_t n) override {
        thread_local std::vector<std::uint8_t> bounce(std::size_t{1} << 20);
        std::size_t done = 0;
        auto* out = static_cast<std::uint8_t*>(dst);
        while (done < n && offset + done < size_) {
            const std::size_t want = std::min(n - done, bounce.size());
            const ssize_t got = ::pread(fd_, bounce.data(), want, static_cast<off_t>(offset + done));
            if (got <= 0) break;
            std::memcpy(out + done, bounce.data(), static_cast<std::size_t>(got));
            done += static_cast<std::size_t>(got);
        }
        return done;
    }

private:
    int fd_;
    std::uint64_t size_;
};
#endif

class HostTree final : public Tree {
public:
    explicit HostTree(stdfs::path root) : root_(std::move(root)) {}

    std::optional<NodeInfo> stat(const std::string& path) const override {
        std::error_code ec;
        const stdfs::path p = at(path);
        const auto st = stdfs::status(p, ec);
        if (ec || !stdfs::exists(st)) return std::nullopt;
        NodeInfo n;
        n.is_dir = stdfs::is_directory(st);
        n.size = n.is_dir ? 0 : stdfs::file_size(p, ec);
        return n;
    }

    std::vector<DirEntry> list(const std::string& dir) const override {
        std::vector<DirEntry> out;
        std::error_code ec;
        for (const auto& e : stdfs::directory_iterator(at(dir), ec)) {
            const auto name = e.path().filename().u8string();
            DirEntry d;
            d.name.assign(name.begin(), name.end());
            d.is_dir = e.is_directory(ec);
            d.size = d.is_dir ? 0 : e.file_size(ec);
            out.push_back(std::move(d));
        }
        return out;
    }

    std::unique_ptr<FileReader> open(const std::string& path) const override {
#ifndef _WIN32
        const int fd = ::open(at(path).c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return nullptr;
        struct stat st{};
        if (fstat(fd, &st) != 0 || S_ISDIR(st.st_mode)) {
            ::close(fd);
            return nullptr;
        }
        return std::make_unique<HostFile>(fd, static_cast<std::uint64_t>(st.st_size));
#else
        HANDLE h = CreateFileW(at(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return nullptr;
        LARGE_INTEGER size{};
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(h, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            !GetFileSizeEx(h, &size)) {
            CloseHandle(h);
            return nullptr;
        }
        return std::make_unique<HostFile>(h, static_cast<std::uint64_t>(size.QuadPart));
#endif
    }

    const stdfs::path* host_root() const override { return &root_; }

private:
    stdfs::path at(const std::string& path) const { return path.empty() ? root_ : root_ / utf8_path(path); }
    stdfs::path root_;
};

}  // namespace

std::optional<std::vector<std::uint8_t>> Tree::read_all(const std::string& path) const {
    auto f = open(path);
    if (!f) return std::nullopt;
    std::vector<std::uint8_t> data(static_cast<std::size_t>(f->size()));
    if (f->read(0, data.data(), data.size()) != data.size()) return std::nullopt;
    return data;
}

std::shared_ptr<Tree> host_tree(stdfs::path root) { return std::make_shared<HostTree>(std::move(root)); }

stdfs::path utf8_path(const std::string& s) { return stdfs::path(std::u8string(s.begin(), s.end())); }

}  // namespace mh3u::fs
