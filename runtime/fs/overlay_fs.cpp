#include "overlay_fs.h"

#include <algorithm>
#include <map>

namespace mh3u::fs {
namespace {

namespace stdfs = std::filesystem;

// '/'-separated, no empty or "." components, no trailing '/'. Backslashes are accepted too, since MT
// Framework builds resource paths with '\'. Returns nullopt if ".." would climb above the root.
std::optional<std::vector<std::string>> split_guest(std::string_view path) {
    std::vector<std::string> parts;
    std::string cur;
    auto flush = [&]() -> bool {
        if (cur.empty() || cur == ".") {
        } else if (cur == "..") {
            if (parts.empty()) return false;
            parts.pop_back();
        } else {
            parts.push_back(cur);
        }
        cur.clear();
        return true;
    };
    for (char c : path) {
        if (c == '/' || c == '\\') {
            if (!flush()) return std::nullopt;
        } else {
            cur += c;
        }
    }
    if (!flush()) return std::nullopt;
    return parts;
}

std::string prefix_of(std::string_view guest_prefix) {
    auto parts = split_guest(guest_prefix);
    if (!parts) return std::string(guest_prefix);
    std::string out;
    for (const auto& p : *parts) out += "/" + p;
    return out;
}

}  // namespace

void OverlayFs::mount(std::string guest_prefix, std::vector<std::shared_ptr<Tree>> layers) {
    std::erase(layers, nullptr);
    mounts_.push_back({prefix_of(guest_prefix), std::move(layers), {}, false});
}

void OverlayFs::mount_writable(std::string guest_prefix, std::filesystem::path host_dir) {
    mounts_.push_back({prefix_of(guest_prefix), {host_tree(host_dir)}, host_dir, true});
}

std::optional<std::pair<const OverlayFs::Mount*, std::string>> OverlayFs::find(std::string_view guest_path) const {
    auto parts = split_guest(guest_path);
    if (!parts) return std::nullopt;
    // ".." is resolved before matching, so "/vol/content/../code" is matched as "/vol/code"; the relative
    // part can therefore never leave its mount.
    std::string full;
    for (const auto& p : *parts) full += "/" + p;
    const Mount* best = nullptr;
    for (const auto& m : mounts_) {
        bool match = full == m.prefix || (full.size() > m.prefix.size() && full.compare(0, m.prefix.size(), m.prefix) == 0 &&
                                          full[m.prefix.size()] == '/');
        if (match && (!best || m.prefix.size() > best->prefix.size())) best = &m;
    }
    if (!best) return std::nullopt;
    std::string rel = full.size() > best->prefix.size() ? full.substr(best->prefix.size() + 1) : std::string{};
    return std::make_pair(best, std::move(rel));
}

std::optional<NodeInfo> OverlayFs::stat(std::string_view guest_path) const {
    auto hit = find(guest_path);
    if (!hit) return std::nullopt;
    for (const auto& layer : hit->first->layers)
        if (auto n = layer->stat(hit->second)) return n;
    return std::nullopt;
}

std::optional<std::size_t> OverlayFs::layer_of(std::string_view guest_path) const {
    auto hit = find(guest_path);
    if (!hit) return std::nullopt;
    for (std::size_t i = 0; i < hit->first->layers.size(); ++i)
        if (hit->first->layers[i]->stat(hit->second)) return i;
    return std::nullopt;
}

std::unique_ptr<FileReader> OverlayFs::open(std::string_view guest_path) const {
    auto hit = find(guest_path);
    if (!hit) return nullptr;
    for (const auto& layer : hit->first->layers) {
        auto n = layer->stat(hit->second);
        if (!n) continue;
        return n->is_dir ? nullptr : layer->open(hit->second);  // a directory shadows lower layers' files
    }
    return nullptr;
}

std::optional<std::filesystem::path> OverlayFs::resolve_for_write(std::string_view guest_path) const {
    auto hit = find(guest_path);
    if (!hit || !hit->first->writable || hit->second.empty()) return std::nullopt;
    auto host = hit->first->host_dir / utf8_path(hit->second);
    std::error_code ec;
    stdfs::create_directories(host.parent_path(), ec);
    if (ec) return std::nullopt;
    return host;
}

std::vector<DirEntry> OverlayFs::list(std::string_view guest_dir) const {
    std::map<std::string, DirEntry> merged;
    auto hit = find(guest_dir);
    if (!hit) return {};
    const auto& layers = hit->first->layers;
    for (std::size_t li = 0; li < layers.size(); ++li) {
        auto n = layers[li]->stat(hit->second);
        if (!n || !n->is_dir) continue;
        for (DirEntry& d : layers[li]->list(hit->second)) {
            if (merged.count(d.name)) continue;  // a higher-priority layer already supplied it
            d.layer = li;
            std::string key = d.name;
            merged.emplace(std::move(key), std::move(d));
        }
    }
    std::vector<DirEntry> out;
    out.reserve(merged.size());
    for (auto& [_, d] : merged) out.push_back(std::move(d));
    return out;
}

OverlayFs make_title_fs(std::shared_ptr<Tree> base_title, std::shared_ptr<Tree> update_title,
                        const std::filesystem::path& save_dir) {
    // a title's subfolders as trees of their own: "content" inside a title tree
    struct Sub final : Tree {
        std::shared_ptr<Tree> t;
        std::string dir;
        std::string at(const std::string& p) const { return p.empty() ? dir : dir + "/" + p; }
        std::optional<NodeInfo> stat(const std::string& p) const override { return t->stat(at(p)); }
        std::vector<DirEntry> list(const std::string& p) const override { return t->list(at(p)); }
        std::unique_ptr<FileReader> open(const std::string& p) const override { return t->open(at(p)); }
    };
    auto sub = [](const std::shared_ptr<Tree>& t, const char* dir) -> std::shared_ptr<Tree> {
        if (!t) return nullptr;
        auto s = std::make_shared<Sub>();
        s->t = t;
        s->dir = dir;
        return s;
    };
    OverlayFs fs;
    fs.mount("/vol/content", {sub(update_title, "content"), sub(base_title, "content")});
    fs.mount("/vol/code", {sub(update_title, "code"), sub(base_title, "code")});
    fs.mount_writable("/vol/save", save_dir);
    return fs;
}

}  // namespace mh3u::fs
