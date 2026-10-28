// Read-only file trees the guest file system is layered from: a host directory (an extracted dump) or a folder
// inside a .wua archive (runtime/fs/zarchive.h). Paths are relative to the tree root, '/'-separated, already
// normalised by OverlayFs (no "." / ".." / empty components); "" is the root.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mh3u::fs {

struct DirEntry {
    std::string name;
    bool is_dir = false;
    std::uint64_t size = 0;
    std::size_t layer = 0;  // index of the layer the entry came from (0 = highest priority)
};

struct NodeInfo {
    bool is_dir = false;
    std::uint64_t size = 0;
};

// An open read-only file. read() is positional and may be called from any thread.
class FileReader {
public:
    virtual ~FileReader() = default;
    virtual std::uint64_t size() const = 0;
    // Copies up to n bytes from offset into dst; returns the number of bytes copied (short at end of file).
    virtual std::size_t read(std::uint64_t offset, void* dst, std::size_t n) = 0;
};

class Tree {
public:
    virtual ~Tree() = default;
    virtual std::optional<NodeInfo> stat(const std::string& path) const = 0;
    virtual std::vector<DirEntry> list(const std::string& dir) const = 0;  // layer left 0
    virtual std::unique_ptr<FileReader> open(const std::string& path) const = 0;
    // Host directory behind the tree, if it is one (writable mounts, diagnostics).
    virtual const std::filesystem::path* host_root() const { return nullptr; }

    // Whole file, or nullopt if it does not exist.
    std::optional<std::vector<std::uint8_t>> read_all(const std::string& path) const;
};

std::shared_ptr<Tree> host_tree(std::filesystem::path root);

// Guest paths are UTF-8; build the host path without the deprecated u8path.
std::filesystem::path utf8_path(const std::string& s);

}  // namespace mh3u::fs
