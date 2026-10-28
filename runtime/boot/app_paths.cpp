#include "app_paths.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>

namespace mh3u::boot {
namespace {

namespace stdfs = std::filesystem;

stdfs::path from_utf8(const std::string& s) { return stdfs::path(std::u8string(s.begin(), s.end())); }
std::string to_utf8(const stdfs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

// A source checkout: config/target.txt next to runtime/ (the exe lives in build/... below it).
bool is_repo(const stdfs::path& p) {
    std::error_code ec;
    return stdfs::exists(p / "config" / "target.txt", ec) && stdfs::exists(p / "runtime", ec);
}

std::vector<std::pair<std::string, std::string>> read_ini(const stdfs::path& file) {
    std::vector<std::pair<std::string, std::string>> out;
    std::ifstream f(file);
    for (std::string line; std::getline(f, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        out.emplace_back(line.substr(0, eq), line.substr(eq + 1));
    }
    return out;
}

}  // namespace

AppPaths find_paths(const stdfs::path& exe, const stdfs::path& data_override) {
    AppPaths p;
    std::error_code ec;
    p.exe_dir = stdfs::absolute(exe, ec).parent_path();
#ifndef _WIN32
    // in an AppImage the binary runs from a temporary mount: "next to the exe" means next to the .AppImage file
    if (const char* appimage = std::getenv("APPIMAGE"); appimage && *appimage) p.exe_dir = stdfs::path(appimage).parent_path();
#endif
    for (stdfs::path d : {stdfs::current_path(ec), p.exe_dir})
        for (; !d.empty(); d = d.parent_path()) {
            if (is_repo(d)) {
                p.repo = d;
                break;
            }
            if (d == d.parent_path()) break;
        }
    if (!data_override.empty()) {
        p.data = stdfs::absolute(data_override, ec);
    } else if (stdfs::exists(p.exe_dir / "portable.txt", ec)) {
        p.data = p.exe_dir / "data";
    } else if (!p.repo.empty()) {
        p.data = p.repo;
    } else {
#ifndef _WIN32
        // $XDG_DATA_HOME/Cha-Cha, by default ~/.local/share/Cha-Cha
        const char* xdg = std::getenv("XDG_DATA_HOME");
        const char* home = std::getenv("HOME");
        p.data = xdg && *xdg ? stdfs::path(xdg) / "Cha-Cha" : home ? stdfs::path(home) / ".local" / "share" / "Cha-Cha" : p.exe_dir / "data";
#else
        wchar_t* appdata = nullptr;
        size_t len = 0;
        if (_wdupenv_s(&appdata, &len, L"APPDATA") == 0 && appdata) {
            p.data = stdfs::path(appdata) / "Cha-Cha";
            // the folder had the project's earlier name: take it over (settings, saves, caches)
            const stdfs::path old = stdfs::path(appdata) / "MH3U-PC";
            if (!stdfs::exists(p.data, ec) && stdfs::exists(old, ec)) stdfs::rename(old, p.data, ec);
        } else {
            p.data = p.exe_dir / "data";
        }
        std::free(appdata);
#endif
    }
    stdfs::create_directories(p.data, ec);
    return p;
}

std::vector<stdfs::path> load_game_sources(const AppPaths& p) {
    std::vector<stdfs::path> out;
    for (const auto& [k, v] : read_ini(p.config()))
        if (k == "game" && !v.empty()) out.push_back(from_utf8(v));
    return out;
}

bool save_game_sources(const AppPaths& p, const std::vector<stdfs::path>& sources) {
    std::vector<std::pair<std::string, std::string>> keep;
    for (auto& kv : read_ini(p.config()))
        if (kv.first != "game") keep.push_back(std::move(kv));
    std::ofstream f(p.config(), std::ios::trunc);
    f << "# Cha-Cha settings. game = your own dump of Monster Hunter 3 Ultimate (US) + update v32:\n"
         "# a .wua file or game folders (code, content, meta). Several game lines are combined.\n";
    for (const auto& s : sources) {
        std::error_code ec;
        f << "game=" << to_utf8(stdfs::absolute(s, ec)) << '\n';
    }
    for (const auto& [k, v] : keep) f << k << '=' << v << '\n';
    return static_cast<bool>(f);
}

Settings load_settings(const AppPaths& p) {
    Settings s;
    for (const auto& [k, v] : read_ini(p.config())) {
        const bool on = v == "1" || v == "true" || v == "on";
        if (k == "skip_boot_notices") s.skip_boot_notices = on;
        else if (k == "render_scale") s.render_scale = std::clamp(static_cast<float>(std::atof(v.c_str())), 0.25f, 4.0f);
        else if (k == "display_mode") s.display_mode = v == "borderless" || v == "fullscreen" ? v : "windowed";
        else if (k == "window_size") s.window_size = v;
        else if (k == "bloom") s.bloom = on;
        else if (k == "depth_of_field") s.depth_of_field = v == "enhanced" || v == "2" ? "enhanced" : on || v == "original" ? "original" : "off";
        else if (k == "ambient_occlusion") s.ambient_occlusion = on;
        else if (k == "global_illumination") s.global_illumination = on;
        else if (k == "vignette") s.vignette = on;
        else if (k == "texture_packs") s.texture_packs = on;
        else if (k == "look") s.look = v == "tri" ? "tri" : "original";
        else if (k == "fps_limit") s.fps_limit = v == "unlimited" ? -1 : std::max(0, std::atoi(v.c_str()));
        else if (k == "vsync") s.vsync = on;
    }
    return s;
}

bool save_settings(const AppPaths& p, const Settings& s) {
    std::vector<std::pair<std::string, std::string>> keep;
    for (auto& kv : read_ini(p.config()))
        if (kv.first == "game") keep.push_back(std::move(kv));
    std::ofstream f(p.config(), std::ios::trunc);
    f << "# Cha-Cha settings. game = your own dump of Monster Hunter 3 Ultimate (US) + update v32:\n"
         "# a .wua file or game folders (code, content, meta). Several game lines are combined.\n";
    for (const auto& [k, v] : keep) f << k << '=' << v << '\n';
    f << "skip_boot_notices=" << (s.skip_boot_notices ? 1 : 0) << '\n'
      << "render_scale=" << s.render_scale << '\n'
      << "display_mode=" << s.display_mode << '\n'
      << "window_size=" << s.window_size << '\n'
      << "bloom=" << (s.bloom ? 1 : 0) << '\n'
      << "depth_of_field=" << s.depth_of_field << '\n'
      << "ambient_occlusion=" << (s.ambient_occlusion ? 1 : 0) << '\n'
      << "global_illumination=" << (s.global_illumination ? 1 : 0) << '\n'
      << "vignette=" << (s.vignette ? 1 : 0) << '\n'
      << "texture_packs=" << (s.texture_packs ? 1 : 0) << '\n'
      << "look=" << s.look << '\n'
      << "fps_limit=" << (s.fps_limit < 0 ? std::string("unlimited") : std::to_string(s.fps_limit)) << '\n'
      << "vsync=" << (s.vsync ? 1 : 0) << '\n';
    return static_cast<bool>(f);
}

std::vector<stdfs::path> repo_game_sources(const AppPaths& p) {
    std::vector<stdfs::path> out;
    if (p.repo.empty()) return out;
    std::error_code ec;
    if (stdfs::exists(p.repo / "extracted" / "0005000e10118300_v32", ec)) out.push_back(p.repo / "extracted");
    else
        for (const auto& e : stdfs::directory_iterator(p.repo / "game", ec))
            if (e.path().extension() == ".wua") {
                out.push_back(e.path());
                break;
            }
    return out;
}

}  // namespace mh3u::boot
