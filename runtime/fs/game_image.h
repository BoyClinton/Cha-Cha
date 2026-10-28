// The user's copy of the game, wherever it lives. Nothing is extracted or copied: the runtime reads files from it on
// demand. Wii U dumps come in several shapes (docs/formats/dumps.md); the decrypted ones are read here:
//   <file>.wua                          Cemu's archive: base, update and DLC as folders "<titleid>_v<version>"
//   a title folder (code/ content/ meta/)  loadiine / Dumpling / cdecrypt output, one title per folder
//   a folder of title folders           extracted/ (scripts/extract.ps1), Dumpling's Games/ + Updates/, ...
//   a Cemu mlc01 folder                 usr/title/00050000/10118300 (base) + usr/title/0005000e/10118300 (update)
//   code/<name>.rpx                     the executable inside a title folder (taken as that title folder)
// Each title is identified by code/app.xml (title_id, title_version), not by its folder name, and the base game
// and update may come from different sources (e.g. a .wud base later, a Dumpling update folder).
// Encrypted formats (.wud/.wux discs, NUS/WUP .app + title.tmd) need the user's keys and are not read yet.
//
// The executable must be the US release with update v32: every address in the recompiled code is specific to it.
#pragma once

#include "overlay_fs.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mh3u::fs {

inline constexpr std::uint64_t kBaseTitleId = 0x0005000010118300ull;    // Monster Hunter 3 Ultimate (US)
inline constexpr std::uint64_t kUpdateTitleId = 0x0005000E10118300ull;  // its update
inline constexpr std::uint32_t kTargetUpdateVersion = 32;
// SHA-1 of the RPX this port is recompiled from (config/target.txt: US, update v32).
inline constexpr const char* kTargetRpxSha1 = "d97aabfc5a45362f88dfacb09f23c6dcae6faac2";
inline constexpr const char* kRpxName = "MH3G_Cafe_US.rpx";

struct Title {
    std::uint64_t id = 0;
    std::uint32_t version = 0;
    std::string where;            // for messages: "mh3u.wua: 0005000e10118300_v32", a folder path, ...
    std::shared_ptr<Tree> tree;   // root holds code/ content/ meta/
};

// Every title found in one source (a file or folder the user chose). Fails only if the source cannot be read.
std::optional<std::vector<Title>> scan_source(const std::filesystem::path& path, std::string* error);

struct GameImage {
    std::vector<std::filesystem::path> sources;
    Title base, update;            // update.tree is null when the update is already merged into the base folder
    std::vector<std::uint8_t> rpx; // the executable as stored (compressed RPX), from the update over the base
    std::string rpx_sha1;          // always kTargetRpxSha1 once open_game succeeded

    OverlayFs make_fs(const std::filesystem::path& save_dir) const { return make_title_fs(base.tree, update.tree, save_dir); }
};

// Picks MH3U's base title and its newest update from all sources and reads the RPX. Fails with a message meant
// for the user (wrong game, other region, update missing, wrong update version).
std::optional<GameImage> open_game(const std::vector<std::filesystem::path>& sources, std::string* error);

// Lower-case hex SHA-1 (Windows CNG).
std::string sha1_hex(const std::uint8_t* data, std::size_t size);

}  // namespace mh3u::fs
