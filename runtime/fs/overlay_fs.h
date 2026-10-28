// Guest file system for the Cafe OS FS layer: maps guest paths (/vol/content/..., /vol/code/...,
// /vol/save/...) onto read-only trees (an extracted dump folder or a folder inside the user's .wua) and, for saves,
// a writable host directory.
//
// A read-only mount is a stack of layers searched in order; the first layer containing a path wins.
// That is how the Wii U applies a title update: update content shadows base content file by file, and
// files only in the update are added (see docs/research-log.md - the v32 update replaces 20 files and
// adds 15). A writable mount (saves) has a single host directory.
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

class OverlayFs {
public:
    // layers[0] has the highest priority. guest_prefix like "/vol/content".
    void mount(std::string guest_prefix, std::vector<std::shared_ptr<Tree>> layers);
    void mount_writable(std::string guest_prefix, std::filesystem::path host_dir);

    // File or directory at a guest path, or nullopt if no layer has it, the path is outside every mount, or ".."
    // climbs above "/". ".." is resolved on the guest path before mount lookup, so access never leaves a mount.
    std::optional<NodeInfo> stat(std::string_view guest_path) const;
    std::unique_ptr<FileReader> open(std::string_view guest_path) const;
    // Index of the layer a path comes from (0 = highest priority), for tests and diagnostics.
    std::optional<std::size_t> layer_of(std::string_view guest_path) const;

    // Host path to create/write a file under a writable mount (parent directories are created).
    std::optional<std::filesystem::path> resolve_for_write(std::string_view guest_path) const;

    // Merged directory listing across layers, sorted by name; a name present in several layers is
    // listed once, from the highest-priority layer.
    std::vector<DirEntry> list(std::string_view guest_dir) const;

private:
    struct Mount {
        std::string prefix;  // normalised, no trailing '/'
        std::vector<std::shared_ptr<Tree>> layers;
        std::filesystem::path host_dir;  // writable mounts only
        bool writable = false;
    };

    // Longest-prefix mount for a guest path, plus the normalised '/'-joined path relative to it.
    std::optional<std::pair<const Mount*, std::string>> find(std::string_view guest_path) const;

    std::vector<Mount> mounts_;
};

// Standard layout for MH3U: /vol/content and /vol/code layered update-over-base (update may be null when the dump
// has the update already merged), /vol/save writable at save_dir.
OverlayFs make_title_fs(std::shared_ptr<Tree> base_title, std::shared_ptr<Tree> update_title,
                        const std::filesystem::path& save_dir);

}  // namespace mh3u::fs
