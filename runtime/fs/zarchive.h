// Reader for ZArchive (.wua), the Wii U title archive Cemu creates from a dump: one file holding the base title,
// its update and DLC as folders ("0005000010118300_v0", "0005000e10118300_v32", ...), each with code/ content/ meta/.
// Written from the format (docs/formats/wua.md); file data is read and decompressed on demand, never extracted.
//
// Layout (all integers big-endian): a stream of 64 KiB blocks, each zstd-compressed (stored raw when compression
// does not help), followed by offset records (one u64 base offset + 16 u16 "compressed size - 1" per 16 blocks),
// a name table, the file tree (16-byte nodes; a directory's children are contiguous) and a 144-byte footer at the
// very end that locates every section.
#pragma once

#include "tree.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mh3u::fs {

class ZArchive : public std::enable_shared_from_this<ZArchive> {
public:
    static std::shared_ptr<ZArchive> open(const std::filesystem::path& path, std::string* error = nullptr);
    ~ZArchive();

    struct Node {
        std::string name;
        bool is_file = false;
        std::uint64_t offset = 0, size = 0;  // file: position in the uncompressed stream
        std::uint32_t first = 0, count = 0;  // directory: children [first, first + count)
    };
    static constexpr std::uint32_t kRoot = 0;

    const Node& node(std::uint32_t i) const { return nodes_[i]; }
    // Node for a '/'-separated path below `dir` (case-insensitive, as the Wii U file system), or nullopt.
    std::optional<std::uint32_t> find(std::string_view path, std::uint32_t dir = kRoot) const;

    // Reads uncompressed stream bytes [offset, offset + n) into dst; returns the bytes read.
    std::size_t read(std::uint64_t offset, void* dst, std::size_t n) const;

    // A folder of the archive (e.g. "0005000e10118300_v32") as a Tree, or nullptr if it does not exist.
    std::shared_ptr<Tree> subtree(std::string_view folder);

private:
    ZArchive() = default;
    bool block(std::uint64_t index, std::vector<std::uint8_t>& out) const;  // decompressed 64 KiB block

#ifdef _WIN32
    void* file_ = nullptr;  // HANDLE
    void* handle() const { return file_; }
#else
    int fd_ = -1;
    int handle() const { return fd_; }
#endif
    std::uint64_t data_offset_ = 0, data_size_ = 0;
    std::vector<std::uint8_t> records_;  // raw offset records
    std::vector<Node> nodes_;
};

}  // namespace mh3u::fs
