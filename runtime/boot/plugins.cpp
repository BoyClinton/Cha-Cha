// Mods and native plugins (plugins.h, include/chacha_plugin.h, docs/modding.md).
#include "plugins.h"

#include "chacha_plugin.h"
#include "hle.h"
#include "ppc_context.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace stdfs = std::filesystem;

namespace mh3u::plugins {
namespace {

std::string utf8(const stdfs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}
std::string trim(std::string s) {
    const auto b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

// key = value lines ([sections] and # comments skipped)
std::map<std::string, std::string> read_ini(const stdfs::path& f) {
    std::map<std::string, std::string> kv;
    std::ifstream in(f);
    for (std::string line; std::getline(in, line);) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        const auto eq = line.find('=');
        if (eq != std::string::npos) kv[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return kv;
}

// ---- state -------------------------------------------------------------------------------------------------
struct FontSlot {
    stdfs::path path;
    ImFont* font = nullptr;
};

struct Host;  // one per plugin, behind ChaChaPlugin::host
struct ModConfig {
    stdfs::path file;
    std::map<std::string, std::string> kv;
    std::map<std::string, std::string> returned;  // config_get's strings stay valid until the key changes
};

struct Host {
    size_t mod = 0;
    ChaChaPlugin plugin{};
    std::string dir;  // the mod's folder, UTF-8
    void* lib = nullptr;
};

stdfs::path g_data;
bool g_scanned = false, g_loaded = false;
std::vector<Mod> g_mods;
std::vector<std::unique_ptr<Host>> g_hosts;
std::map<size_t, ModConfig> g_config;
std::vector<std::unique_ptr<FontSlot>> g_fonts;  // [0]: the HUD font
bool g_fonts_pending = false;
std::function<void(const std::string&)> g_toast;
uint8_t* g_base = nullptr;
std::string g_data_utf8, g_sha1;
const auto g_t0 = std::chrono::steady_clock::now();

// the HUD's draw list while on_hud runs, the settings rows while on_settings runs
ImDrawList* g_dl = nullptr;
float g_display_w = 1280, g_display_h = 720;
const SettingsUi* g_ui = nullptr;
bool g_group_open = false;
const char* g_group_default = nullptr;

// ---- hooks: one chain per hooked function ------------------------------------------------------------------
struct Hook {
    ChaChaHookFn before, after;
    void* user;
};
struct Chain {
    uint32_t addr = 0;
    ppc::GuestFunction next = nullptr;  // what ran there before: Cha-Cha's own replacement or the translation
    std::vector<Hook> hooks;
};
constexpr int kMaxChains = 64;
std::array<Chain, kMaxChains> g_chains;
int g_nchains = 0;

void run_chain(Chain& c, ppc::Context& ctx, uint8_t* base) {
    ChaChaCall call{};
    call.addr = c.addr;
    for (int i = 0; i < 8; ++i) {
        call.arg_r[i] = ctx.r[3 + i];
        call.arg_f[i] = ctx.f[1 + i].ps0;
    }
    call.ctx = &ctx;
    for (const Hook& h : c.hooks)
        if (h.before) h.before(h.user, &call);
    if (!call.skip) c.next(ctx, base);
    call.ret_r3 = ctx.r[3];
    call.ret_f1 = ctx.f[1].ps0;
    for (const Hook& h : c.hooks)
        if (h.after) h.after(h.user, &call);
}
template <int N>
void trampoline(ppc::Context& ctx, uint8_t* base) {
    run_chain(g_chains[N], ctx, base);
}
template <int... N>
constexpr std::array<ppc::GuestFunction, sizeof...(N)> make_trampolines(std::integer_sequence<int, N...>) {
    return {&trampoline<N>...};
}
constexpr auto kTrampolines = make_trampolines(std::make_integer_sequence<int, kMaxChains>{});

bool add_hook(uint32_t addr, ChaChaHookFn before, ChaChaHookFn after, void* user) {
    for (int i = 0; i < g_nchains; ++i)
        if (g_chains[i].addr == addr) {
            g_chains[i].hooks.push_back({before, after, user});
            return true;
        }
    const ppc::GuestFunction orig = ppc::override_original(addr);
    if (!orig || g_nchains == kMaxChains) return false;  // no hook point there
    ppc::GuestFunction* slot = ppc::override_slot(addr);
    Chain& c = g_chains[g_nchains];
    c.addr = addr;
    c.next = *slot ? *slot : orig;
    c.hooks.push_back({before, after, user});
    *slot = kTrampolines[g_nchains];
    ++g_nchains;
    return true;
}

// ---- the live camera: uGameCamera's per-frame move (vtable slot 8, 0x0227EDE8) ------------------------------
constexpr uint32_t kCameraMove = 0x0227EDE8;
std::mutex g_cam_m;
ChaChaCamera g_cam{};
bool g_cam_ok = false;
std::chrono::steady_clock::time_point g_cam_at;

void camera_after(void*, ChaChaCall* call) {
    const uint32_t cam = call->arg_r[0];
    if (!cam || !os::rt().mem || !os::rt().mem->committed(cam) || !os::rt().mem->committed(cam + 0x70)) return;
    ChaChaCamera c{};
    // uCamera properties (uCamera::createProperty 0x02A040F0): mFarPlane 0x30, mNearPlane 0x34, mAspect 0x38,
    // mFov 0x3C, mCameraPos 0x40, mCameraUp 0x50, mTargetPos 0x60
    c.far_plane = chacha_ldf(g_base, cam + 0x30);
    c.near_plane = chacha_ldf(g_base, cam + 0x34);
    c.aspect = chacha_ldf(g_base, cam + 0x38);
    c.fov_y_degrees = chacha_ldf(g_base, cam + 0x3C);
    for (int i = 0; i < 3; ++i) {
        c.pos[i] = chacha_ldf(g_base, cam + 0x40 + 4 * i);
        c.up[i] = chacha_ldf(g_base, cam + 0x50 + 4 * i);
        c.target[i] = chacha_ldf(g_base, cam + 0x60 + 4 * i);
    }
    c.guest_addr = cam;
    std::lock_guard g(g_cam_m);
    c.frame = g_cam.frame + 1;
    if (g_cam.guest_addr != cam) std::printf("mods: camera uGameCamera %08X (fov %.2f, aspect %.3f)\n", cam, c.fov_y_degrees, c.aspect);
    g_cam = c;
    g_cam_ok = true;
    g_cam_at = std::chrono::steady_clock::now();
}

void game_rect(float* x, float* y, float* w, float* h) {
    const float k = std::min(g_display_w / 1280.0f, g_display_h / 720.0f);
    const float gw = 1280.0f * k, gh = 720.0f * k;
    *x = (g_display_w - gw) * 0.5f;
    *y = (g_display_h - gh) * 0.5f;
    *w = gw;
    *h = gh;
}

bool get_camera(ChaChaCamera* out) {
    std::lock_guard g(g_cam_m);
    // a camera not updated for a second is gone (a menu, a cutscene with its own camera)
    if (!g_cam_ok || std::chrono::steady_clock::now() - g_cam_at > std::chrono::seconds(1)) return false;
    *out = g_cam;
    return true;
}

int world_to_screen(const float p[3], float* sx, float* sy, float* depth) {
    ChaChaCamera c;
    if (!get_camera(&c)) return 0;
    float f[3] = {c.target[0] - c.pos[0], c.target[1] - c.pos[1], c.target[2] - c.pos[2]};
    const auto norm = [](float* v) {
        const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (l < 1e-6f) return false;
        v[0] /= l, v[1] /= l, v[2] /= l;
        return true;
    };
    if (!norm(f)) return 0;
    float r[3] = {f[1] * c.up[2] - f[2] * c.up[1], f[2] * c.up[0] - f[0] * c.up[2], f[0] * c.up[1] - f[1] * c.up[0]};
    if (!norm(r)) return 0;
    const float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
    const float d[3] = {p[0] - c.pos[0], p[1] - c.pos[1], p[2] - c.pos[2]};
    const float z = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
    if (depth) *depth = z;
    const float near_plane = c.near_plane > 0 ? c.near_plane : 1.0f;
    if (z < near_plane) return 0;
    const float x = d[0] * r[0] + d[1] * r[1] + d[2] * r[2], y = d[0] * u[0] + d[1] * u[1] + d[2] * u[2];
    float fov = c.fov_y_degrees;
    if (fov > 3.2f) fov *= 3.14159265f / 180.0f;  // degrees (MT Framework) - radians accepted too
    if (!(fov > 0.01f && fov < 3.1f)) fov = 50.0f * 3.14159265f / 180.0f;
    float gx, gy, gw, gh;
    game_rect(&gx, &gy, &gw, &gh);
    // the picture's 16:9: uCamera's mAspect stays 1.0 on the game's cameras (seen 2026-11-02), the projection
    // takes the viewport's
    const float aspect = gw / gh;
    const float t = std::tan(fov * 0.5f);
    const float nx = x / (z * t * aspect), ny = y / (z * t);
    *sx = gx + (nx * 0.5f + 0.5f) * gw;
    *sy = gy + (0.5f - ny * 0.5f) * gh;
    return 1;
}

// ---- the API ----------------------------------------------------------------------------------------------
Host* host_of(ChaChaPlugin* p) { return p ? static_cast<Host*>(p->host) : nullptr; }
std::string tag(ChaChaPlugin* p) {
    Host* h = host_of(p);
    return h ? g_mods[h->mod].folder + (p->name ? std::string("/") + p->name : std::string()) : std::string("?");
}

void api_log(ChaChaPlugin* self, const char* msg) { std::printf("[%s] %s\n", tag(self).c_str(), msg ? msg : ""); }
void api_toast(ChaChaPlugin*, const char* msg) {
    if (g_toast && msg) g_toast(msg);
}
int api_readable(uint32_t addr, uint32_t len) {
    const core::GuestMemory* m = os::rt().mem;
    if (!m || !len || addr + (len - 1) < addr) return 0;
    for (uint32_t a = addr & ~0xFFFu; a <= addr + (len - 1); a += 0x1000)
        if (!m->committed(a)) return 0;
    return 1;
}
int api_hook(ChaChaPlugin* self, uint32_t addr, ChaChaHookFn before, ChaChaHookFn after, void* user) {
    if (g_loaded) return 0;  // only while plugins initialise, before guest code runs
    const bool ok = add_hook(addr, before, after, user);
    char msg[96];
    std::snprintf(msg, sizeof msg, ok ? "hooked %08X" : "no hook point at %08X", addr);
    api_log(self, msg);
    return ok;
}
uint32_t get_reg(void* ctx, int n) { return n >= 0 && n < 32 ? static_cast<ppc::Context*>(ctx)->r[n] : 0; }
void set_reg(void* ctx, int n, uint32_t v) {
    if (n >= 0 && n < 32) static_cast<ppc::Context*>(ctx)->r[n] = v;
}
double get_freg(void* ctx, int n) { return n >= 0 && n < 32 ? static_cast<ppc::Context*>(ctx)->f[n].ps0 : 0.0; }
void set_freg(void* ctx, int n, double v) {
    if (n >= 0 && n < 32) static_cast<ppc::Context*>(ctx)->f[n].ps0 = static_cast<ppc::Context*>(ctx)->f[n].ps1 = v;
}
int api_camera(ChaChaCamera* out) { return out && get_camera(out); }
double api_time() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count(); }

ImU32 col(uint32_t rgba) { return IM_COL32(rgba >> 24, (rgba >> 16) & 0xFF, (rgba >> 8) & 0xFF, rgba & 0xFF); }
ImFont* font_of(void* f) {
    if (f && static_cast<FontSlot*>(f)->font) return static_cast<FontSlot*>(f)->font;
    if (!g_fonts.empty() && g_fonts[0]->font) return g_fonts[0]->font;
    return ImGui::GetFont();
}
void text_size(void* f, float size, const char* s, float* w, float* h) {
    const ImVec2 v = font_of(f)->CalcTextSizeA(size, FLT_MAX, 0, s ? s : "");
    if (w) *w = v.x;
    if (h) *h = v.y;
}
void draw_text(const ChaChaText* t, const char* s) {
    if (!g_dl || !t || !s) return;
    ImFont* font = font_of(t->font);
    const ImVec2 ts = font->CalcTextSizeA(t->size, FLT_MAX, 0, s);
    ImVec2 at(t->x, t->y - ts.y * 0.5f);
    if (t->align == CHACHA_ALIGN_CENTER) at.x -= ts.x * 0.5f;
    else if (t->align == CHACHA_ALIGN_RIGHT) at.x -= ts.x;
    const int v0 = g_dl->VtxBuffer.Size;
    if (t->outline > 0 && (t->outline_color & 0xFF)) {
        const ImU32 oc = col(t->outline_color);
        const float o = t->outline;
        static const float k[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {0.7f, 0.7f}, {-0.7f, 0.7f}, {0.7f, -0.7f}, {-0.7f, -0.7f}};
        for (const auto& d : k) g_dl->AddText(font, t->size, ImVec2(at.x + d[0] * o, at.y + d[1] * o), oc, s);
    }
    g_dl->AddText(font, t->size, at, col(t->color), s);
    if (t->shear != 0) {  // slant: x moves with the height above the text's middle
        for (int i = v0; i < g_dl->VtxBuffer.Size; ++i) g_dl->VtxBuffer[i].pos.x += (t->y - g_dl->VtxBuffer[i].pos.y) * t->shear;
    }
}
void draw_line(float x0, float y0, float x1, float y1, uint32_t c, float th) {
    if (g_dl) g_dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), col(c), th);
}
void draw_rect(float x0, float y0, float x1, float y1, uint32_t c, float r) {
    if (g_dl) g_dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col(c), r);
}
void draw_circle(float x, float y, float r, uint32_t c) {
    if (g_dl) g_dl->AddCircleFilled(ImVec2(x, y), r, col(c));
}
void draw_poly(const float* xy, int n, uint32_t c) {
    if (!g_dl || !xy || n < 3) return;
    std::vector<ImVec2> pts(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) pts[static_cast<size_t>(i)] = ImVec2(xy[2 * i], xy[2 * i + 1]);
    g_dl->AddConvexPolyFilled(pts.data(), n, col(c));
}
void* load_font(ChaChaPlugin* self, const char* path) {
    Host* h = host_of(self);
    if (!h || !path || g_loaded) return nullptr;
    stdfs::path p = stdfs::path(std::u8string(reinterpret_cast<const char8_t*>(path)));
    if (p.is_relative()) p = g_mods[h->mod].dir / p;
    std::error_code ec;
    if (!stdfs::is_regular_file(p, ec)) return nullptr;
    g_fonts.push_back(std::make_unique<FontSlot>(FontSlot{p, nullptr}));
    g_fonts_pending = true;
    return g_fonts.back().get();
}

// settings rows: a group titled after the plugin opens itself before the first row
void ensure_group() {
    if (!g_group_open && g_ui) {
        g_ui->group(g_group_default);
        g_group_open = true;
    }
}
void ui_group(const char* title) {
    if (!g_ui) return;
    if (g_group_open) g_ui->end_group();
    g_ui->group(title);
    g_group_open = true;
}
int ui_switch(const char* label, int* on, const char* desc) {
    if (!g_ui || !on) return 0;
    ensure_group();
    bool b = *on != 0;
    const bool changed = g_ui->toggle(label, b, desc);
    *on = b;
    return changed;
}
int ui_choice(const char* label, int* index, const char* const* items, int n, const char* desc) {
    if (!g_ui || !index || n <= 0) return 0;
    ensure_group();
    const int cur = std::clamp(*index, 0, n - 1);
    const int step = g_ui->value(label, items[cur], desc);
    if (!step) return 0;
    *index = (cur + (step < 0 ? -1 : 1) + n) % n;
    return 1;
}
int ui_float(const char* label, float* v, float lo, float hi, float step_size, const char* fmt, const char* desc) {
    if (!g_ui || !v) return 0;
    ensure_group();
    char buf[64];
    std::snprintf(buf, sizeof buf, fmt ? fmt : "%.2f", *v);
    const int step = g_ui->value(label, buf, desc);
    if (!step) return 0;
    float nv = *v + (step < 0 ? -step_size : step_size);
    if (step == 2 && nv > hi + step_size * 0.5f) nv = lo;  // activated past the top: wraps
    nv = std::clamp(std::round(nv / step_size) * step_size, lo, hi);
    if (nv == *v) return 0;
    *v = nv;
    return 1;
}
int ui_button(const char* label, const char* value, const char* desc) {
    if (!g_ui) return 0;
    ensure_group();
    return g_ui->button(label, desc, value ? value : "");
}
void ui_info(const char* label, const char* value, const char* desc) {
    if (!g_ui) return;
    ensure_group();
    g_ui->info(label, value ? value : "", desc);
}

ModConfig& config_of(Host* h) {
    auto it = g_config.find(h->mod);
    if (it == g_config.end()) {
        ModConfig c;
        c.file = g_mods[h->mod].dir / "settings.ini";
        c.kv = read_ini(c.file);
        it = g_config.emplace(h->mod, std::move(c)).first;
    }
    return it->second;
}
const char* config_get(ChaChaPlugin* self, const char* key, const char* fallback) {
    Host* h = host_of(self);
    if (!h || !key) return fallback;
    ModConfig& c = config_of(h);
    const auto it = c.kv.find(key);
    if (it == c.kv.end()) return fallback;
    return (c.returned[key] = it->second).c_str();
}
void config_set(ChaChaPlugin* self, const char* key, const char* value) {
    Host* h = host_of(self);
    if (h && key) config_of(h).kv[key] = value ? value : "";
}
int config_save(ChaChaPlugin* self) {
    Host* h = host_of(self);
    if (!h) return 0;
    ModConfig& c = config_of(h);
    std::ofstream out(c.file, std::ios::trunc);
    if (!out) return 0;
    out << "# " << g_mods[h->mod].name << " settings (written by Cha-Cha; edit while the game is closed)\n";
    for (const auto& [k, v] : c.kv) out << k << " = " << v << '\n';
    return out.good() ? 1 : 0;
}
const char* mod_dir(ChaChaPlugin* self) {
    Host* h = host_of(self);
    return h ? h->dir.c_str() : "";
}

ChaChaApi g_api{};

void* open_library(const stdfs::path& f, std::string& err) {
#ifdef _WIN32
    HMODULE m = LoadLibraryW(f.wstring().c_str());
    if (!m) err = "could not load (error " + std::to_string(GetLastError()) + ")";
    return m;
#else
    void* m = dlopen(f.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!m) err = dlerror();
    return m;
#endif
}
void* library_symbol(void* lib, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name));
#else
    return dlsym(lib, name);
#endif
}

size_t count_files(const stdfs::path& dir) {
    size_t n = 0;
    std::error_code ec;
    for (auto it = stdfs::recursive_directory_iterator(dir, ec); !ec && it != stdfs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec)) ++n;
    return n;
}

}  // namespace

stdfs::path mods_dir(const stdfs::path& data) { return data / "mods"; }

std::vector<Mod> scan(const stdfs::path& data) {
    std::vector<Mod> out;
    const stdfs::path root = mods_dir(data);
    std::error_code ec;
    const bool fresh = !stdfs::exists(root, ec);
    stdfs::create_directories(root, ec);
#ifndef _WIN32
    // the AppImage carries mods (usr/share/cha-cha/mods) it cannot run from (read-only): copied once, when the mods
    // folder is made
    if (const char* appdir = std::getenv("APPDIR"); fresh && appdir && *appdir) {
        const stdfs::path bundled = stdfs::path(appdir) / "usr" / "share" / "cha-cha" / "mods";
        for (const auto& e : stdfs::directory_iterator(bundled, ec))
            if (e.is_directory(ec)) {
                stdfs::copy(e.path(), root / e.path().filename(), stdfs::copy_options::recursive, ec);
                std::printf("mods: installed %s (comes with Cha-Cha)\n", utf8(e.path().filename()).c_str());
            }
    }
#else
    (void)fresh;
#endif
    const auto states = read_ini(data / "mods.ini");
#ifdef _WIN32
    const char* ext = ".dll";
#else
    const char* ext = ".so";
#endif
    for (const auto& e : stdfs::directory_iterator(root, ec)) {
        if (!e.is_directory(ec)) continue;
        Mod m;
        m.dir = e.path();
        m.folder = utf8(e.path().filename());
        const auto info = read_ini(m.dir / "mod.ini");
        const auto get = [&](const char* k, std::string d) {
            const auto it = info.find(k);
            return it == info.end() || it->second.empty() ? d : it->second;
        };
        m.name = get("name", m.folder);
        m.author = get("author", "");
        m.version = get("version", "");
        m.description = get("description", "");
        const auto st = states.find("mod." + m.folder);
        // never switched in MODS: the mod's own default (mod.ini "default = off" for optional mods), else on
        m.enabled = st == states.end() ? get("default", "on") != "off" : st->second != "off";
        m.has_content = stdfs::is_directory(m.dir / "content", ec);
        if (m.has_content) m.content_files = count_files(m.dir / "content");
        for (const auto& p : stdfs::directory_iterator(m.dir / "plugins", ec))
            if (p.is_regular_file(ec) && p.path().extension() == ext) m.plugin_files.push_back(p.path());
        std::sort(m.plugin_files.begin(), m.plugin_files.end());
        if (!m.has_content && m.plugin_files.empty() && info.empty()) continue;  // not a mod
        out.push_back(std::move(m));
    }
    std::sort(out.begin(), out.end(), [](const Mod& a, const Mod& b) { return a.folder < b.folder; });
    return out;
}

void save_enabled(const stdfs::path& data, const std::string& folder, bool on) {
    std::vector<std::string> lines;
    {
        std::ifstream in(data / "mods.ini");
        for (std::string l; std::getline(in, l);)
            if (l.rfind("mod." + folder + "=", 0) != 0) lines.push_back(l);
    }
    lines.push_back("mod." + folder + "=" + (on ? "on" : "off"));
    std::ofstream out(data / "mods.ini", std::ios::trunc);
    for (const auto& l : lines) out << l << '\n';
}

std::vector<Mod>& mods() { return g_mods; }

void ensure_scanned(const stdfs::path& data) {
    if (g_scanned) return;
    g_data = data;
    g_mods = scan(data);
    g_scanned = true;
}

void set_enabled(size_t i, bool on) {
    if (i >= g_mods.size()) return;
    g_mods[i].enabled = on;
    save_enabled(g_data, g_mods[i].folder, on);
}

void set_toast(std::function<void(const std::string&)> fn) { g_toast = std::move(fn); }

void load(const stdfs::path& data, uint8_t* guest_base, const std::string& rpx_sha1) {
    g_data = data;
    g_base = guest_base;
    g_data_utf8 = utf8(data);
    g_sha1 = rpx_sha1;
    g_mods = scan(data);
    g_scanned = true;
    g_api.version = CHACHA_API_VERSION;
    g_api.size = sizeof(ChaChaApi);
    g_api.guest_base = guest_base;
    g_api.data_dir = g_data_utf8.c_str();
    g_api.game_sha1 = g_sha1.c_str();
    g_api.log = &api_log;
    g_api.toast = &api_toast;
    g_api.readable = &api_readable;
    g_api.hook = &api_hook;
    g_api.get_reg = &get_reg;
    g_api.set_reg = &set_reg;
    g_api.get_freg = &get_freg;
    g_api.set_freg = &set_freg;
    g_api.camera = &api_camera;
    g_api.world_to_screen = &world_to_screen;
    g_api.game_rect = &game_rect;
    g_api.time = &api_time;
    g_api.draw_text = &draw_text;
    g_api.text_size = &text_size;
    g_api.draw_line = &draw_line;
    g_api.draw_rect = &draw_rect;
    g_api.draw_circle = &draw_circle;
    g_api.draw_poly = &draw_poly;
    g_api.load_font = &load_font;
    g_api.ui_group = &ui_group;
    g_api.ui_switch = &ui_switch;
    g_api.ui_choice = &ui_choice;
    g_api.ui_float = &ui_float;
    g_api.ui_button = &ui_button;
    g_api.ui_info = &ui_info;
    g_api.config_get = &config_get;
    g_api.config_set = &config_set;
    g_api.config_save = &config_save;
    g_api.mod_dir = &mod_dir;
    g_api.plugin_size = sizeof(ChaChaPlugin);

    size_t loaded = 0;
    for (size_t i = 0; i < g_mods.size(); ++i) {
        Mod& m = g_mods[i];
        m.active = m.enabled;
        if (!m.enabled) {
            std::printf("mods: %s off\n", m.folder.c_str());
            continue;
        }
        if (m.has_content) std::printf("mods: %s content (%zu files)\n", m.folder.c_str(), m.content_files);
        for (const auto& f : m.plugin_files) {
            PluginState ps;
            ps.file = utf8(f.filename());
            auto h = std::make_unique<Host>();
            h->mod = i;
            h->dir = utf8(m.dir);
            h->plugin.host = h.get();
            std::string err;
            h->lib = open_library(f, err);
            const auto init = h->lib ? reinterpret_cast<ChaChaPluginInit>(library_symbol(h->lib, "chacha_plugin_init")) : nullptr;
            if (h->lib && !init) err = "no chacha_plugin_init";
            if (init) {
                const int v = init(&g_api, &h->plugin);
                if (v == CHACHA_API_VERSION) {
                    ps.loaded = true;
                    ps.name = h->plugin.name ? h->plugin.name : ps.file;
                    ps.version = h->plugin.version ? h->plugin.version : "";
                    ps.handle = &h->plugin;
                    g_hosts.push_back(std::move(h));
                    ++loaded;
                } else {
                    err = v ? "made for plugin API " + std::to_string(v) + " (Cha-Cha has " + std::to_string(CHACHA_API_VERSION) + ")"
                            : "declined to load (wrong game version?)";
                }
            }
            if (!ps.loaded) std::printf("mods: %s/%s not loaded: %s\n", m.folder.c_str(), ps.file.c_str(), err.c_str());
            else std::printf("mods: %s/%s loaded (%s %s)\n", m.folder.c_str(), ps.file.c_str(), ps.name.c_str(), ps.version.c_str());
            ps.error = err;
            m.plugins.push_back(std::move(ps));
        }
    }
    if (loaded) add_hook(kCameraMove, nullptr, &camera_after, nullptr);  // the camera, for world_to_screen
    g_fonts.insert(g_fonts.begin(), std::make_unique<FontSlot>());  // [0]: the HUD font
    g_fonts_pending = loaded != 0;
    g_loaded = true;
}

std::vector<stdfs::path> content_dirs() {
    std::vector<stdfs::path> out;
    for (const Mod& m : g_mods)
        if (m.active && m.has_content) out.push_back(m.dir / "content");
    return out;
}

bool has_hud() {
    for (const auto& h : g_hosts)
        if (h->plugin.on_hud) return true;
    return false;
}
bool fonts_pending() { return g_fonts_pending; }

void add_pending_fonts(const stdfs::path& hud_font_path) {
    if (!g_fonts_pending) return;
    g_fonts_pending = false;
    ImGuiIO& io = ImGui::GetIO();
    if (!g_fonts.empty() && !g_fonts[0]->font) g_fonts[0]->path = hud_font_path;
    for (auto& f : g_fonts) {
        if (f->font || f->path.empty()) continue;
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        cfg.OversampleV = 2;
        // large: the HUD scales text down from it (sharp at any size up to 96 px)
        f->font = io.Fonts->AddFontFromFileTTF(utf8(f->path).c_str(), 96.0f, &cfg);
        if (!f->font) std::printf("mods: font %s could not be read\n", utf8(f->path).c_str());
    }
}

void draw_hud(ImDrawList* dl, float w, float h) {
    g_display_w = w;
    g_display_h = h;
    g_dl = dl;
    for (const auto& host : g_hosts)
        if (host->plugin.on_hud) host->plugin.on_hud(&host->plugin);
    g_dl = nullptr;
}

bool has_settings(size_t index) {
    for (const auto& h : g_hosts)
        if (h->mod == index && h->plugin.on_settings) return true;
    return false;
}

void draw_settings(size_t index, const SettingsUi& ui) {
    for (const auto& h : g_hosts) {
        if (h->mod != index || !h->plugin.on_settings) continue;
        g_ui = &ui;
        g_group_open = false;
        g_group_default = h->plugin.name ? h->plugin.name : "Settings";
        ImGui::PushID(h.get());
        h->plugin.on_settings(&h->plugin);
        ImGui::PopID();
        if (g_group_open) ui.end_group();
        g_ui = nullptr;
    }
}

int save_all() {
    int n = 0;
    for (const auto& h : g_hosts)
        if (h->plugin.on_save) h->plugin.on_save(&h->plugin), ++n;
    return n;
}

int defaults_all() {
    int n = 0;
    for (const auto& h : g_hosts)
        if (h->plugin.on_defaults) h->plugin.on_defaults(&h->plugin), ++n;
    return n;
}

void shutdown() {
    for (const auto& h : g_hosts)
        if (h->plugin.on_shutdown) h->plugin.on_shutdown(&h->plugin);
}

}  // namespace mh3u::plugins
