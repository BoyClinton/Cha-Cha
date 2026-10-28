// mh3u: boots the recompiled game.
//   1. load + relocate the executable (recomp loader) and map its sections into guest memory
//   2. set up the import area (data imports), heaps, the main thread's stack and guest OSThread
//   3. register the HLE Cafe OS functions
//   4. call the RPX entry point __rpx_start(argc, argv): it runs the static constructor table (0x101632C4, via
//      0x02E842EC) and then main(); exit() unwinds back here as GuestExit.
//   The host main thread owns the SDL window and pumps its events; the guest boots on its own host thread.
// Usage: mh3u [--game <dump>]... [--install <dump>...] [--data-dir <dir>] [--play] [--headless] [--console]
//   Starts in the launcher (runtime/boot/setup.cpp); --play goes straight into the game.
//   <dump> = the user's .wua or game folders (runtime/fs/game_image.h); --install records them in config.ini.
//   env MH3U_TRACE=1 prints every Cafe OS import call,
//        MH3U_EXIT_AFTER_FRAMES=N stops after N frames and prints statistics.
#include "app_paths.h"
#include "boot_prompts.h"
#include "filters.h"
#include "frame_limit.h"
#include "monster_hp.h"
#include "carving.h"
#include "item_sets.h"
#include "text_trace.h"
#include "setup.h"
#include "game_image.h"
#include "guest_memory.h"
#include "hle.h"
#include "host_input.h"
#include "loader.h"
#include "ax.h"
#include "gx2.h"
#include "overlay_fs.h"
#include "presenter.h"
#include "profiler.h"
#include "read_watch.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

namespace stdfs = std::filesystem;
using namespace mh3u;

namespace {

constexpr uint32_t kEntry = 0x02E84370;           // __rpx_start (ELF e_entry)
constexpr uint32_t kMainStackSize = 0x00100000;   // 1 MB

std::map<std::string, uint64_t> g_import_calls;
void trace_import(const char* name) {
    if (os::rt().trace_imports) os::note_import(name);
    uint64_t n = ++g_import_calls[name];
    if (os::rt().trace_imports && n <= 3)
        std::fprintf(stderr, "[import f%u] %s%s\n", os::rt().frames.load(), name, n == 3 ? " (further calls not shown)" : "");
}

void print_import_summary() {
    profiler::report();
    read_watch::report();
    if (!os::rt().trace_imports) return;  // counted only when tracing
    std::printf("import calls: %zu distinct\n", g_import_calls.size());
    uint64_t gx2 = 0;
    for (auto& [name, n] : g_import_calls)
        if (name.rfind("gx2.", 0) == 0) gx2 += n;
    std::printf("gx2 calls: %llu\n", static_cast<unsigned long long>(gx2));
    // per-function counts, most frequent first
    std::vector<std::pair<uint64_t, std::string>> top;
    for (auto& [name, n] : g_import_calls)
        top.emplace_back(n, name);
    std::sort(top.rbegin(), top.rend());
    for (auto& [n, name] : top) std::printf("  %10llu %s\n", static_cast<unsigned long long>(n), name.c_str());
}

// Debugging: MH3U_PEEK="<vtable hex>:<off>,<off>,..." prints, at exit, every guest object whose first word is that
// vtable, with the u32 at each offset (hex offsets; all committed memory is scanned - slow in Debug).
// MH3U_PEEK="@<address hex>:<off>,..." prints the fields of one object.
void peek_objects(core::GuestMemory& mem) {
    char* env = nullptr;
    size_t len = 0;
    if (_dupenv_s(&env, &len, "MH3U_PEEK") != 0 || !env) return;
    std::string spec = env;
    std::free(env);
    const bool direct = !spec.empty() && spec[0] == '@';
    if (direct) spec.erase(0, 1);
    const size_t colon = spec.find(':');
    const uint32_t vtable = static_cast<uint32_t>(std::strtoul(spec.c_str(), nullptr, 16));
    std::vector<uint32_t> offs;
    for (size_t p = colon == std::string::npos ? spec.size() : colon + 1; p < spec.size();) {
        offs.push_back(static_cast<uint32_t>(std::strtoul(spec.c_str() + p, nullptr, 16)));
        const size_t c = spec.find(',', p);
        p = c == std::string::npos ? spec.size() : c + 1;
    }
    const uint8_t* base = mem.base();
    if (direct) {
        std::printf("peek %08X:", vtable);
        for (uint32_t o : offs) std::printf(" +%X=%08X", o, mem.committed(vtable + o) ? ppc::ld32(base, vtable + o) : 0);
        std::printf("\n");
        return;
    }
    for (uint64_t pg = 0x01000000; pg < 0x100000000ull; pg += 0x1000) {
        const uint32_t page = static_cast<uint32_t>(pg);
        if (!mem.committed(page)) continue;
        for (uint32_t a = page; a < page + 0x1000; a += 4) {
            if (ppc::ld32(base, a) != vtable) continue;
            std::printf("peek %08X:", a);
            for (uint32_t o : offs) std::printf(" +%X=%08X", o, ppc::ld32(base, a + o));
            std::printf("\n");
        }
    }
}

bool map_sections(core::GuestMemory& mem, const recomp::Image& img) {
    for (const auto& s : img.elf.sections) {
        if (s.addr == 0 || s.addr >= core::layout::kImportBase || s.size == 0) continue;
        if (s.type != 1 && s.type != 8) continue;  // PROGBITS / NOBITS
        if (!mem.commit(s.addr, s.size)) {
            std::fprintf(stderr, "cannot commit %s at %08X\n", s.name.c_str(), s.addr);
            return false;
        }
        if (!s.data.empty()) std::memcpy(mem.base() + s.addr, s.data.data(), s.data.size());
        std::printf("  mapped %-10s %08X-%08X\n", s.name.c_str(), s.addr, s.addr + s.size);
    }
    return true;
}

struct Args {
    bool headless = false, install = false, console = false, play = false;
    stdfs::path data_dir;
    std::vector<stdfs::path> games;  // --game / --install / bare paths
};

// Cha-Cha.exe is a GUI-subsystem program, so players get no console window. Output goes where a developer expects it:
// the pipe/file the parent gave us (scripts, tests), else the console of the terminal it was started from, else a
// new console (--console), else <data>/log.txt.
void setup_output(const boot::AppPaths& paths, bool new_console) {
#ifndef _WIN32  // started from a terminal or a launcher: stdout / stderr are already where they should go
    (void)paths;
    (void)new_console;
#else
    const auto usable = [](DWORD which) {
        const HANDLE h = GetStdHandle(which);
        return h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN;
    };
    FILE* f = nullptr;
    if (usable(STD_OUTPUT_HANDLE)) {
        // inherited pipe or file: the CRT already writes there
    } else if (new_console ? AllocConsole() : AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    } else {
        const std::wstring log = (paths.data / "log.txt").wstring();
        _wfreopen_s(&f, log.c_str(), L"w", stdout);
        _wfreopen_s(&f, log.c_str(), L"a", stderr);
    }
#endif
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
}

// From the wide command line, so paths with any characters survive (argv is in the ANSI code page).
#ifndef _WIN32
// argv is UTF-8 already
Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        const auto path = [](const char* v) { const std::string u = v; return stdfs::path(std::u8string(u.begin(), u.end())); };
        if (s == "--headless") a.headless = true;
        else if (s == "--console") a.console = true;
        else if (s == "--play") a.play = true;
        else if (s == "--install") a.install = true;
        else if (s == "--data-dir" && i + 1 < argc) a.data_dir = path(argv[++i]);
        else if (s == "--game" && i + 1 < argc) a.games.push_back(path(argv[++i]));
        else if (!s.empty() && s[0] != '-') a.games.push_back(path(s.c_str()));
    }
    return a;
}
#else
Args parse_args(int, char**) {
    Args a;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        const std::wstring s = argv[i];
        if (s == L"--headless") a.headless = true;
        else if (s == L"--console") a.console = true;
        else if (s == L"--play") a.play = true;
        else if (s == L"--install") a.install = true;
        else if (s == L"--data-dir" && i + 1 < argc) a.data_dir = argv[++i];
        else if (s == L"--game" && i + 1 < argc) a.games.emplace_back(argv[++i]);
        else if (!s.empty() && s[0] != L'-') a.games.emplace_back(s);
    }
    LocalFree(argv);
    return a;
}
#endif

// Settings reach the presenter, gx2 and the renderer as environment values (MH3U_FPS, MH3U_VSYNC, MH3U_LOOK, ...).
// A value a developer set before starting wins; ours are rewritten when the settings change.
std::set<std::string> g_dev_env;
void settings_env(const boot::Settings& s, bool headless) {
    const auto set = [](const char* name, const std::string& value) {
        static bool first = true;
        if (first) {
            first = false;
            for (const char* n : {"MH3U_FPS", "MH3U_VSYNC", "MH3U_LOOK", "MH3U_AO", "MH3U_GI", "MH3U_DOF", "MH3U_RENDER_SCALE", "MH3U_VIGNETTE",
                                  "MH3U_TEXTURE_PACKS"}) {
                char* v = nullptr;
                size_t len = 0;
                if (_dupenv_s(&v, &len, n) == 0 && v) g_dev_env.insert(n);
                std::free(v);
            }
        }
        if (!g_dev_env.count(name)) _putenv_s(name, value.c_str());
    };
    if (!headless) {  // headless keeps the game's own pacing (input scripts count frames)
        set("MH3U_FPS", s.fps_limit < 0 ? "max" : s.fps_limit == 0 ? "" : std::to_string(s.fps_limit));
        set("MH3U_VSYNC", s.vsync ? "1" : "0");
    }
    set("MH3U_LOOK", s.look);
    set("MH3U_AO", s.ambient_occlusion ? "1" : "0");
    set("MH3U_GI", s.global_illumination ? "1" : "0");
    set("MH3U_VIGNETTE", s.vignette ? "1" : "0");
    set("MH3U_DOF", s.depth_of_field == "enhanced" ? "1" : "0");
    set("MH3U_RENDER_SCALE", std::to_string(s.render_scale));
    set("MH3U_TEXTURE_PACKS", s.texture_packs ? "1" : "0");
}

// Window events: host input, and the launcher while it is up.
boot::Launcher* g_launcher = nullptr;

// Checking the window without a screen: MH3U_SHOT="s1,s2,..." writes what the window shows at those seconds after
// start (overlay included) to <repo>/build/shot_<s>.bmp; MH3U_SETTINGS_AT="s,..." opens / closes the in-game settings
// at those times (as F1 would).
struct Timed {
    std::vector<double> at;
    size_t next = 0;
    explicit Timed(const char* env_name) {
        char* v = nullptr;
        size_t n = 0;
        if (_dupenv_s(&v, &n, env_name) == 0 && v)
            for (char* q = v; *q;) {
                char* end = nullptr;
                const double s = std::strtod(q, &end);
                if (end == q) break;
                at.push_back(s);
                q = *end ? end + 1 : end;
            }
        std::free(v);
        std::sort(at.begin(), at.end());
    }
    // the time that has come since the last call, or a negative value
    double due(double secs) { return next < at.size() && secs >= at[next] ? at[next++] : -1.0; }
};
Timed g_shots("MH3U_SHOT"), g_settings_at("MH3U_SETTINGS_AT");
const auto g_process_start = std::chrono::steady_clock::now();
void timed_actions(gfx::Presenter& p, const boot::AppPaths& paths) {
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_process_start).count();
    if (const double s = g_shots.due(secs); s >= 0) {
        const stdfs::path dir = paths.repo.empty() ? paths.data : paths.repo / "build";
        p.capture((dir / ("shot_" + std::to_string(static_cast<int>(s)) + ".bmp")).string());
    }
    if (g_settings_at.due(secs) >= 0 && g_launcher) g_launcher->toggle_settings();
}
void on_event(const void* e) {
    input::handle_event(e);
    if (!g_launcher) return;
    // the settings over the game: F1, or a controller's Home / guide (PS, Xbox) or misc (mic, capture) button - none of
    // them is bound to a game button
    const SDL_Event& ev = *static_cast<const SDL_Event*>(e);
    if ((ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat && ev.key.key == SDLK_F1) ||
        (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
         (ev.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE || ev.gbutton.button == SDL_GAMEPAD_BUTTON_MISC1))) {
        g_launcher->toggle_settings();
        return;
    }
    if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat && ev.key.key == SDLK_F2) {
        g_launcher->toggle_mods();
        return;
    }
    g_launcher->handle_event(e);
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);
    const boot::AppPaths paths = boot::find_paths(argv[0], args.data_dir);
    setup_output(paths, args.console);
    boot::Settings settings = boot::load_settings(paths);
    std::printf("data folder: %s\n", paths.data.string().c_str());

    // The game: --game/--install paths, else config.ini, else (in the source checkout) extracted/ or game/*.wua.
    std::vector<stdfs::path> sources = args.games;
    if (sources.empty()) sources = boot::load_game_sources(paths);
    if (sources.empty()) sources = boot::repo_game_sources(paths);
    std::string err;
    std::optional<fs::GameImage> game;
    if (!sources.empty()) game = fs::open_game(sources, &err);
    else err = "No game set up yet. Start without --headless to choose it, or run: mh3u --install <.wua or game folder>";
    if (args.install) {
        if (!game) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        if (!boot::save_game_sources(paths, sources)) {
            std::fprintf(stderr, "cannot write %s\n", paths.config().string().c_str());
            return 1;
        }
        std::printf("installed: %s\n", paths.config().string().c_str());
    }
    // The window comes first: the launcher is drawn over it (and over the game once it runs). Headless / --install
    // runs prepare the shaders on the console instead.
    const bool headless = args.headless;
    gfx::Presenter presenter;
    std::unique_ptr<boot::Launcher> launcher;
    stdfs::path shader_dir;
    if (headless || args.install) {
        boot::SetupResult setup = boot::run_setup(paths, std::move(game), err);
        if (!setup.game) return 1;
        game = std::move(setup.game);
        shader_dir = setup.shader_dir;
    } else {
        settings_env(settings, headless);  // the swapchain reads MH3U_VSYNC
        input::set_hints();
        int win_w = 1280, win_h = 720;  // the window_size setting; MH3U_WINDOW=WxH overrides (checking the layout)
        sscanf_s(settings.window_size.c_str(), "%dx%d", &win_w, &win_h);
        size_t ws_len = 0;
        if (char* ws = nullptr; _dupenv_s(&ws, &ws_len, "MH3U_WINDOW") == 0 && ws) {
            sscanf_s(ws, "%dx%d", &win_w, &win_h);
            std::free(ws);
        }
        if (!presenter.init("Cha-Cha", win_w, win_h)) {
            std::fprintf(stderr, "window/Vulkan init failed: %s\n", presenter.error().c_str());
            return 1;
        }
        std::printf("Vulkan device: %s\n", presenter.device_name().c_str());
        input::init(paths.data, true);
        input::set_window(presenter.window_handle());
        input::set_suspended(true);  // the launcher has the input until PLAY
        presenter.set_display_mode(settings.display_mode == "fullscreen" ? 2 : settings.display_mode == "borderless" ? 1 : 0);
        launcher = std::make_unique<boot::Launcher>(paths, presenter, sources, std::move(game), err, settings, !args.play);
        g_launcher = launcher.get();
        presenter.set_event_hook(&on_event);
        // until the game can boot: the launcher over its backdrop (first start: choosing the dump, translating shaders)
        while (!launcher->ready_to_boot()) {
            timed_actions(presenter, paths);
            if (!presenter.pump_events() || !launcher->frame({})) return 0;
            input::update();
            presenter.present_clear(0.0f, 0.0f, 0.0f);
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        game = launcher->take_game();
        settings = launcher->settings();
        shader_dir = launcher->shader_dir();
    }
    std::printf("game: base %s, update %s (v%u)\n", game->base.where.c_str(),
                game->update.tree ? game->update.where.c_str() : "merged", game->update.version);
    if (args.install) return 0;
    auto img = recomp::load_image(game->rpx, &err);
    if (!img) {
        std::fprintf(stderr, "cannot load the executable: %s\n", err.c_str());
        return 1;
    }

    core::GuestMemory mem;
    mem.install_fault_reporter();
    read_watch::start_from_env(mem.base());
    using namespace core::layout;
    std::printf("guest memory at %p\n", static_cast<void*>(mem.base()));
    if (!map_sections(mem, *img)) return 1;
    bool ok = mem.commit(kImportBase, kImportEnd - kImportBase) && mem.commit(kStackBase, kStackEnd - kStackBase) &&
              mem.commit(kHleBase, kHleEnd - kHleBase) && mem.commit(kMem2HeapBase, kMem2HeapEnd - kMem2HeapBase) &&
              mem.commit(0xF4000000, 0x02000000) &&  // MEM1
              mem.commit(0xE0000000, 0x02800000);    // foreground bucket (app part)
    if (!ok) {
        std::fprintf(stderr, "cannot commit guest memory regions\n");
        return 1;
    }

    os::Runtime& r = os::rt();
    r.mem = &mem;
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // keep the game's OSReport output when the process is killed
    char* frames_env = nullptr;
    size_t frames_len = 0;
    if (_dupenv_s(&frames_env, &frames_len, "MH3U_EXIT_AFTER_FRAMES") == 0 && frames_env)
        r.exit_after_frames = static_cast<uint32_t>(std::strtoul(frames_env, nullptr, 10));
    std::free(frames_env);
    char* trace = nullptr;
    size_t trace_len = 0;
    r.trace_imports = _dupenv_s(&trace, &trace_len, "MH3U_TRACE") == 0 && trace != nullptr;
    std::free(trace);
    // per-import counting only when tracing: it runs on every Cafe OS call, from every guest thread
    if (r.trace_imports) ppc::import_trace_hook = &trace_import;
    profiler::name_this_thread(L"host main (window events)");
    profiler::start_from_env();
    r.main_thread = mem.alloc_hle(0x6A0, 8);  // sizeof(OSThread) on Cafe OS
    os::register_coreinit_base(r);
    os::register_coreinit_memory(r);
    os::register_coreinit_sync(r);
    os::register_coreinit_time(r);
    os::register_nn_act(r);
    // files: /vol/content and /vol/code = update over base from the user's dump, /vol/save = <data>/save
    fs::OverlayFs vfs = game->make_fs(paths.saves());
    r.vfs = &vfs;
    os::register_coreinit_fs(r);
    os::register_nn_save(r);
    os::register_coreinit_config(r);
    os::register_coreinit_thread(r);
    os::register_input(r);
    os::register_coreinit_dynload(r);
    os::register_applets(r);
    os::register_offline(r);
    // hand-written game code (src/): replacements of recompiled functions listed in config/overrides.txt
    game::register_boot_prompts(settings.skip_boot_notices);
    game::register_filters(settings.bloom, settings.depth_of_field != "off");
    game::register_frame_limit(!headless);  // windowed runs pace frames themselves (FRAME RATE LIMIT)
    game::register_text_trace();  // MH3U_TRACE_TEXT: who asks for which text
    game::register_monster_hp();  // logs each monster's max HP (monster HP scaling, in development)
    game::register_carving([] { input::repeat_held(input::kA, 3000); });  // hold to gather: A repeats while held
    game::register_item_sets(paths.data / "save" / "80000001", [](const std::string& msg) {
        if (g_launcher) g_launcher->post_toast(msg);  // shown over the game
    });
    game::set_frame_capture(&gfx::dump_next_frame);
    os::register_coreinit_misc(r);
    os::register_h264(r);
    // Static initialisers exported by system libraries (e.g. nn_uds.__sti___11_uds_Api_cpp_...) are called from the
    // game's ctor table; the libraries are HLE, so they have nothing to initialise.
    for (const auto& im : img->imports) {
        if (im.data || im.name.rfind("__sti__", 0) != 0) continue;
        std::string rpl = im.rpl.size() > 4 && im.rpl.ends_with(".rpl") ? im.rpl.substr(0, im.rpl.size() - 4) : im.rpl;
        ppc::register_import((rpl + "." + im.name).c_str(), [](ppc::Context&, uint8_t*) {});
    }
    // remaining coreinit data imports (values, not functions)
    ppc::st32(mem.base(), 0xC0007FD8, 20);  // __gh_FOPEN_MAX (value assumed: GHS default)
    ppc::st32(mem.base(), 0xC0008000, 0);   // environ = NULL

    // main thread: stack top, back chain 0; argv = {"MH3G_Cafe_US.rpx", NULL}
    ppc::Context ctx{};
    const uint32_t stack_top = kStackBase + kMainStackSize;
    ctx.r[1] = stack_top - 0x20;
    const char* name = "MH3G_Cafe_US.rpx";
    uint32_t name_addr = mem.alloc_hle(static_cast<uint32_t>(std::strlen(name) + 1));
    std::memcpy(mem.base() + name_addr, name, std::strlen(name) + 1);
    uint32_t argv_addr = mem.alloc_hle(8, 4);
    ppc::st32(mem.base(), argv_addr, name_addr);
    ctx.r[3] = 1;
    ctx.r[4] = argv_addr;
    ctx.lr = 0;

    settings_env(settings, headless);  // as chosen in the launcher before the game booted
    {  // texture packs: <data>/texture_replacements (MH3U_TEXTURE_DIR overrides)
        size_t n = 0;
        char* v = nullptr;
        if (_dupenv_s(&v, &n, "MH3U_TEXTURE_DIR") != 0 || !v) {
            const stdfs::path dir = paths.data / "texture_replacements";
            std::error_code ec;
            stdfs::create_directories(dir, ec);
            const auto u = dir.u8string();
            _putenv_s("MH3U_TEXTURE_DIR", reinterpret_cast<const char*>(u.c_str()));
        }
        std::free(v);
    }
    if (!headless) presenter.refresh_swapchain();  // VSync may have changed since the window opened
    r.data_dir = paths.data;
    r.shader_dir = shader_dir;
    if (!headless) {
        r.presenter = &presenter;
    } else if (presenter.init("Cha-Cha", 1280, 720, true)) {
        std::printf("Vulkan device: %s (headless)\n", presenter.device_name().c_str());
        r.presenter = &presenter;
        input::init(paths.data, false);
    } else {
        std::fprintf(stderr, "Vulkan init failed (%s): running without a GPU\n", presenter.error().c_str());
        input::init(paths.data, false);
    }
    gfx::register_gx2(r);
    audio::register_ax(r, !headless);

    std::atomic<bool> guest_done{false}, pipelines_ready{false};
    std::atomic<size_t> pipelines_done{0}, pipelines_total{0};
    int code = 0;
    std::thread guest([&] {
        profiler::name_this_thread(L"guest main thread (core 1)");
        // the pipelines earlier runs used, before the game starts (the launcher shows a toast meanwhile)
        gfx::precompile_pipelines([&](size_t done, size_t total) {
            pipelines_done = done;
            pipelines_total = total;
        });
        pipelines_ready = true;
        std::printf("calling __rpx_start (%08X)\n", kEntry);
        try {
            ppc::call_indirect(ctx, mem.base(), kEntry);
            std::printf("__rpx_start returned, r3=%08X\n", ctx.r[3]);
        } catch (const os::GuestExit& e) {
            code = e.code;
            std::printf("guest called exit(%d)\n", code);
        }
        guest_done = true;
    });
    // main thread: window events until the guest finishes or the window is closed
    const auto start = std::chrono::steady_clock::now();
    // MH3U_EXIT_AFTER_SECONDS: wall-clock exit (with the usual summary/dumps) for runs where frames stop advancing
    double exit_after_secs = 0;
    {
        char* s = nullptr;
        size_t sl = 0;
        if (_dupenv_s(&s, &sl, "MH3U_EXIT_AFTER_SECONDS") == 0 && s) exit_after_secs = std::strtod(s, nullptr);
        std::free(s);
    }
    // FPS in the window title: game frames (GX2SwapScanBuffers) per wall-clock second, updated twice a second
    auto fps_since = start;
    uint32_t fps_frames = 0;
    const bool profiling = profiler::running();  // also log the FPS every 5 s
    auto last_clear = start;
    while (!guest_done) {
        bool closed = !headless && !presenter.pump_events();
        if (launcher) {
            // every iteration, also while hidden: the launcher notices the game asking for text (on-screen keyboard)
            // itself. Visible, it is drawn over the game, or over its backdrop until the game shows frames.
            const bool game_frames = presenter.showing_image(250);
            boot::LaunchState st;
            st.game_running = true;
            st.game_frames = game_frames;
            st.pipelines_done = pipelines_done;
            st.pipelines_total = pipelines_total;
            st.pipelines_ready = pipelines_ready;
            if (!launcher->frame(st)) closed = true;
            if (launcher->visible() && !game_frames && std::chrono::steady_clock::now() - last_clear > std::chrono::milliseconds(15)) {
                presenter.present_clear(0.0f, 0.0f, 0.0f);
                last_clear = std::chrono::steady_clock::now();
            }
        }
        if (launcher) input::set_suspended(launcher->visible());
        if (!headless) timed_actions(presenter, paths);
        const auto now = std::chrono::steady_clock::now();
        const double secs = std::chrono::duration<double>(now - start).count();
        if (const double span = std::chrono::duration<double>(now - fps_since).count(); (!headless || profiling) && span >= 0.5) {
            const uint32_t frames = r.frames.load();
            char status[32];
            std::snprintf(status, sizeof status, "%.0f FPS", (frames - fps_frames) / span);
            input::set_title_status(status);
            if (profiling && static_cast<int>(secs) / 5 != static_cast<int>(secs - span) / 5)
                std::printf("fps: %5.1f s  %s\n", secs, status);
            fps_frames = frames;
            fps_since = now;
        }
        const bool timed_out = exit_after_secs > 0 && secs >= exit_after_secs;
        if (closed || r.exit_requested || timed_out) {
            std::printf("%s after %u frames (GX2SwapScanBuffers) in %.1f s\n",
                        closed ? "window closed" : timed_out ? "MH3U_EXIT_AFTER_SECONDS reached" : "MH3U_EXIT_AFTER_FRAMES reached",
                        r.frames.load(), secs);
            print_import_summary();
            mh3u::gfx::print_gx2_stats();
            mh3u::gfx::save_gx2_caches();
            if (os::rt().trace_imports) os::dump_threads();
            peek_objects(mem);
            std::fflush(stdout);
            std::_Exit(0);  // guest threads are not cancellable
        }
        input::update();
        read_watch::poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    guest.join();
    mh3u::gfx::save_gx2_caches();
    print_import_summary();
    return code;
}
