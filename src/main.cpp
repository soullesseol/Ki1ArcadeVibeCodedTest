#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <thread>
#include "Renderer.hpp"
#include "dcs.h"
#include "install.h"
#include "recomp.h"

extern const ki::FuncEntry boot_stub_funcs[];
extern const size_t boot_stub_count;
extern const ki::FuncEntry boot_main_funcs[];
extern const size_t boot_main_count;
extern const adsp::Program dcs_boot, dcs_os, dcs_ovl_a, dcs_ovl_b;

static ki::Bus bus;
static ki::Ctx ctx;
static adsp::Board dcs;

// The picture to show, as a tightly packed copy in `out` (width x 240). With widescreen, margins
// nothing drew into this frame (see Bus::screen) get kinst.ini `widescreen_fill`.
static MarginFill ws_fill = MarginFill::Black;
static Frame screen_frame(std::vector<uint16_t>& out) {
    ki::Bus::Screen s = bus.screen();
    out.resize(size_t(s.width) * ki::SCREEN_H);
    for (int y = 0; y < ki::SCREEN_H; y++) {
        const uint16_t* row = s.pixels + y * (s.pitch / 2);
        uint16_t* dst = &out[size_t(y) * s.width];
        std::memcpy(dst, row, size_t(s.width) * 2);
    }
    if (bus.widescreen && !s.margins_valid) fill_margins(out.data(), s.width, ki::SCREEN_H, ki::WS_M, ws_fill);
    return {out.data(), s.width, ki::SCREEN_H, s.width * 2};
}

// SDL pulls DCS samples; on underrun it holds the last sample (silence would click).
static void audio_callback(void*, Uint8* stream, int len) {
    static int16_t last = 0;
    auto* out = reinterpret_cast<int16_t*>(stream);
    size_t n = size_t(len) / 2, got = dcs.pull(out, n);
    if (got) last = out[got - 1];
    for (size_t i = got; i < n; i++) out[i] = last;
}
static std::atomic<bool> guest_stopped = false;

// Dump RAM so the next round of recompilation can pick up what the code unpacked/loaded.
static void dump_state() {
    std::ofstream("ram_00000000.bin", std::ios::binary).write((const char*)bus.ram.data(), bus.ram.size());
    std::ofstream("ram_08000000.bin", std::ios::binary).write((const char*)bus.ram2.data(), bus.ram2.size());
    if (bus.dcs) {
        const adsp::Dsp& d = dcs.dsp;
        std::printf("dcs: last pc %04x i7 %04x ab %d imask %02x icntl %02x irq2 state/latch %d/%d latch_control %04x pc_sp %u stat_sp %u\n",
                    adsp::g_last_pc.load(), d.i[7], int(dcs.ab_active), d.imask, d.icntl, d.irq_state[adsp::IRQ2], d.irq_latch[adsp::IRQ2],
                    dcs.control_r(), d.pc_sp, d.stat_sp);
    }
    std::printf("last function entered: %08x, recent:", ctx.func);
    for (uint32_t i = 0; i < 64; i++) std::printf("%s%08x", i % 8 ? " " : "\n  ", ctx.trail[(ctx.trail_pos + i) & 63]);
    std::printf("\n");
    for (int i = 0; i < 32; i++) std::printf("r%-2d=%016llx%s", i, (unsigned long long)ctx.r[i], i % 4 == 3 ? "\n" : " ");
    std::fflush(stdout);
}

// Guest thread: the boot stub already ran at install time (data/boot.bin holds the state it left),
// so start straight at the unpacked game program.
static void guest() {
    ki::register_funcs(boot_main_funcs, boot_main_count);
    // The game copies its exception vector stub from 0x88026344 to 0x80000180.
    ki::alias(0x80000180, 0x88026344);
    // Interrupt poll tick; the 60Hz timer needs far less resolution than this.
    std::thread([] { for (;;) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); ctx.poll_flag = 1; } }).detach();
    try {
        ki::run(&ctx, 0x88000000);
    } catch (const ki::MissingFunc& m) {
        std::printf("reached unrecompiled code at %08x\n", m.vaddr);
    } catch (const ki::Unimplemented& u) {
        std::printf("unimplemented instruction %08x at %08x\n", u.word, u.pc);
    }
    guest_stopped = true;
}

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// Print where the guest was when it crashed (e.g. native stack overflow from runaway recursion).
static LONG WINAPI crash_handler(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_STACK_OVERFLOW &&
        e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    std::printf("crash %08lx, recent functions:", e->ExceptionRecord->ExceptionCode);
    for (uint32_t i = 0; i < 64; i++) std::printf("%s%08x", i % 8 ? " " : "\n  ", ctx.trail[(ctx.trail_pos + i) & 63]);
    std::printf("\n");
    std::fflush(stdout);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

// Port bits (kinst.cpp INPUT_PORTS "P1"/"P2"), all active-low.
enum : uint32_t {
    HIGH_QUICK = 1 << 0, HIGH_MEDIUM = 1 << 1, HIGH_FIERCE = 1 << 2,
    LOW_QUICK = 1 << 3, LOW_MEDIUM = 1 << 4, LOW_FIERCE = 1 << 5,
    UP = 1 << 6, DOWN = 1 << 7, LEFT = 1 << 8, RIGHT = 1 << 9,
    START = 1 << 10, COIN = 1 << 11, TEST = 1 << 12, // TEST: P1 only (P2 bit 12 is tilt)
};
// Default bindings below; kinst.ini overrides them per action (see load_controls).
struct KeyBind { int player; uint32_t bit; SDL_Scancode key; };
static std::vector<KeyBind> key_binds = {
    {0, UP, SDL_SCANCODE_UP}, {0, DOWN, SDL_SCANCODE_DOWN}, {0, LEFT, SDL_SCANCODE_LEFT}, {0, RIGHT, SDL_SCANCODE_RIGHT},
    {0, HIGH_QUICK, SDL_SCANCODE_Q}, {0, HIGH_MEDIUM, SDL_SCANCODE_W}, {0, HIGH_FIERCE, SDL_SCANCODE_E},
    {0, LOW_QUICK, SDL_SCANCODE_A}, {0, LOW_MEDIUM, SDL_SCANCODE_S}, {0, LOW_FIERCE, SDL_SCANCODE_D},
    {0, START, SDL_SCANCODE_1}, {0, COIN, SDL_SCANCODE_5}, {0, TEST, SDL_SCANCODE_F2},
    {1, UP, SDL_SCANCODE_KP_8}, {1, DOWN, SDL_SCANCODE_KP_2}, {1, LEFT, SDL_SCANCODE_KP_4}, {1, RIGHT, SDL_SCANCODE_KP_6},
    {1, HIGH_QUICK, SDL_SCANCODE_KP_7}, {1, HIGH_MEDIUM, SDL_SCANCODE_KP_9}, {1, HIGH_FIERCE, SDL_SCANCODE_KP_DIVIDE},
    {1, LOW_QUICK, SDL_SCANCODE_KP_1}, {1, LOW_MEDIUM, SDL_SCANCODE_KP_3}, {1, LOW_FIERCE, SDL_SCANCODE_KP_MULTIPLY},
    {1, START, SDL_SCANCODE_2}, {1, COIN, SDL_SCANCODE_6},
};
// Gamepads: top face row = high attacks, bottom row + right trigger = low attacks.
// A pad input is a button, or a trigger (axis) pressed past halfway.
struct PadBind { uint32_t bit; SDL_GameControllerButton button; SDL_GameControllerAxis trigger = SDL_CONTROLLER_AXIS_INVALID; };
static const std::vector<PadBind> default_pad_binds = {
    {UP, SDL_CONTROLLER_BUTTON_DPAD_UP}, {DOWN, SDL_CONTROLLER_BUTTON_DPAD_DOWN},
    {LEFT, SDL_CONTROLLER_BUTTON_DPAD_LEFT}, {RIGHT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT},
    {HIGH_QUICK, SDL_CONTROLLER_BUTTON_X}, {HIGH_MEDIUM, SDL_CONTROLLER_BUTTON_Y}, {HIGH_FIERCE, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
    {LOW_QUICK, SDL_CONTROLLER_BUTTON_A}, {LOW_MEDIUM, SDL_CONTROLLER_BUTTON_B},
    {LOW_FIERCE, SDL_CONTROLLER_BUTTON_INVALID, SDL_CONTROLLER_AXIS_TRIGGERRIGHT},
    {START, SDL_CONTROLLER_BUTTON_START}, {COIN, SDL_CONTROLLER_BUTTON_BACK},
};
static std::vector<PadBind> pad_binds[2] = {default_pad_binds, default_pad_binds}; // per controller

// Remappable actions. kinst.ini keys: p1_<action> / p2_<action> = keyboard keys (SDL key names:
// Q, Up, Keypad 8, Left Shift, Space...); pad_<action> = controller inputs for both pads, then
// p1_pad_<action> / p2_pad_<action> for one controller (SDL names a b x y back start guide
// leftshoulder rightshoulder lefttrigger righttrigger leftstick rightstick dpup dpdown dpleft
// dpright touchpad misc1, or the PlayStation/Xbox aliases below).
// Several inputs are comma-separated; `none` unbinds. A key that's set replaces that action's defaults.
struct Action { const char* name; uint32_t bit; };
static const Action actions[] = {
    {"up", UP}, {"down", DOWN}, {"left", LEFT}, {"right", RIGHT},
    {"high_quick", HIGH_QUICK}, {"high_medium", HIGH_MEDIUM}, {"high_fierce", HIGH_FIERCE},
    {"low_quick", LOW_QUICK}, {"low_medium", LOW_MEDIUM}, {"low_fierce", LOW_FIERCE},
    {"start", START}, {"coin", COIN}, {"test", TEST},
};

static std::string trim(std::string s) {
    s.erase(0, s.find_first_not_of(" \t\r"));
    s.erase(s.find_last_not_of(" \t\r") + 1);
    return s;
}

static std::vector<std::string> split_list(const std::string& v) {
    std::vector<std::string> out;
    for (size_t at = 0; at <= v.size();) {
        size_t comma = std::min(v.find(',', at), v.size());
        if (std::string n = trim(v.substr(at, comma - at)); !n.empty() && n != "none") out.push_back(n);
        at = comma + 1;
    }
    return out;
}

static bool parse_pad(std::string n, PadBind& b) {
    for (char& ch : n) ch = char(std::tolower(uint8_t(ch)));
    // SDL names buttons by Xbox position: a = bottom (PS cross), b = right (circle), x = left (square), y = top (triangle).
    static const std::pair<const char*, const char*> aliases[] = {
        {"cross", "a"}, {"circle", "b"}, {"square", "x"}, {"triangle", "y"},
        {"l1", "leftshoulder"}, {"r1", "rightshoulder"}, {"l2", "lefttrigger"}, {"r2", "righttrigger"},
        {"l3", "leftstick"}, {"r3", "rightstick"}, {"options", "start"}, {"create", "back"}, {"share", "back"}, {"ps", "guide"},
        {"lb", "leftshoulder"}, {"rb", "rightshoulder"}, {"lt", "lefttrigger"}, {"rt", "righttrigger"},
        {"ls", "leftstick"}, {"rs", "rightstick"}, {"menu", "start"}, {"view", "back"}, {"xbox", "guide"},
    };
    for (auto [alias, sdl] : aliases) if (n == alias) n = sdl;
    b.trigger = SDL_GameControllerGetAxisFromString(n.c_str());
    if (b.trigger == SDL_CONTROLLER_AXIS_TRIGGERLEFT || b.trigger == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
        b.button = SDL_CONTROLLER_BUTTON_INVALID;
        return true;
    }
    b.trigger = SDL_CONTROLLER_AXIS_INVALID;
    b.button = SDL_GameControllerGetButtonFromString(n.c_str());
    return b.button != SDL_CONTROLLER_BUTTON_INVALID;
}

static void load_controls(const std::map<std::string, std::string>& cfg) {
    for (const Action& a : actions) {
        for (int p = 0; p < 2; p++) {
            auto it = cfg.find(std::format("p{}_{}", p + 1, a.name));
            if (it == cfg.end()) continue;
            std::erase_if(key_binds, [&](const KeyBind& b) { return b.player == p && b.bit == a.bit; });
            for (const std::string& n : split_list(it->second)) {
                SDL_Scancode sc = SDL_GetScancodeFromName(n.c_str());
                if (sc == SDL_SCANCODE_UNKNOWN) std::fprintf(stderr, "kinst.ini %s: unknown key '%s'\n", it->first.c_str(), n.c_str());
                else key_binds.push_back({p, a.bit, sc});
            }
        }
        // pad_ first (both controllers), then p1_pad_ / p2_pad_ override one.
        struct Scope { const char* prefix; int first, last; };
        for (auto [prefix, first, last] : {Scope{"pad_", 0, 1}, Scope{"p1_pad_", 0, 0}, Scope{"p2_pad_", 1, 1}}) {
            auto it = cfg.find(prefix + std::string(a.name));
            if (it == cfg.end()) continue;
            std::vector<PadBind> binds;
            for (const std::string& n : split_list(it->second)) {
                PadBind b{a.bit, SDL_CONTROLLER_BUTTON_INVALID};
                if (parse_pad(n, b)) binds.push_back(b);
                else std::fprintf(stderr, "kinst.ini %s: unknown controller input '%s'\n", it->first.c_str(), n.c_str());
            }
            for (int p = first; p <= last; p++) {
                std::erase_if(pad_binds[p], [&](const PadBind& b) { return b.bit == a.bit; });
                pad_binds[p].insert(pad_binds[p].end(), binds.begin(), binds.end());
            }
        }
    }
}

// --list-controls: the bindings in effect, in kinst.ini syntax (for a launcher's remap screen).
static void print_controls() {
    // Keyboard p1/p2, then controller p1/p2.
    for (int i = 0; i < 4; i++)
        for (const Action& a : actions) {
            int p = i & 1;
            std::string names;
            for (const KeyBind& b : key_binds)
                if (i < 2 && b.player == p && b.bit == a.bit) names += std::string(names.empty() ? "" : ", ") + SDL_GetScancodeName(b.key);
            for (const PadBind& b : pad_binds[p])
                if (i >= 2 && b.bit == a.bit)
                    names += std::string(names.empty() ? "" : ", ") +
                             (b.trigger != SDL_CONTROLLER_AXIS_INVALID ? SDL_GameControllerGetStringForAxis(b.trigger)
                                                                       : SDL_GameControllerGetStringForButton(b.button));
            std::printf("p%d_%s%s = %s\n", p + 1, i < 2 ? "" : "pad_", a.name, names.empty() ? "none" : names.c_str());
        }
}

static void update_inputs(SDL_GameController* const pads[2]) {
    uint32_t held[2] = {0, 0};
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    for (const KeyBind& b : key_binds) if (keys[b.key]) held[b.player] |= b.bit;
    for (int p = 0; p < 2; p++) {
        if (!pads[p]) continue;
        for (const PadBind& b : pad_binds[p])
            if (b.trigger != SDL_CONTROLLER_AXIS_INVALID ? SDL_GameControllerGetAxis(pads[p], b.trigger) > 16000
                                                         : SDL_GameControllerGetButton(pads[p], b.button))
                held[p] |= b.bit;
        int x = SDL_GameControllerGetAxis(pads[p], SDL_CONTROLLER_AXIS_LEFTX), y = SDL_GameControllerGetAxis(pads[p], SDL_CONTROLLER_AXIS_LEFTY);
        if (x < -16000) held[p] |= LEFT;
        if (x > 16000) held[p] |= RIGHT;
        if (y < -16000) held[p] |= UP;
        if (y > 16000) held[p] |= DOWN;
    }
    held[1] &= ~TEST;
    // Cabinet volume buttons (VOLUME port bits 2/3): the game itself steps the DCS volume.
    bus.volume = ~((keys[SDL_SCANCODE_EQUALS] || keys[SDL_SCANCODE_KP_PLUS] ? 4u : 0u) |
                   (keys[SDL_SCANCODE_MINUS] || keys[SDL_SCANCODE_KP_MINUS] ? 8u : 0u));
    bus.p1 = ~held[0];
    bus.p2 = ~held[1];
}

// kinst.ini next to the exe: `key = value` lines, `#` or `;` starts a comment line.
static std::map<std::string, std::string> load_config(const std::string& path) {
    std::map<std::string, std::string> cfg;
    std::ifstream in(path);
    auto trim = [](std::string s) {
        s.erase(0, s.find_first_not_of(" \t\r"));
        s.erase(s.find_last_not_of(" \t\r") + 1);
        return s;
    };
    for (std::string line; std::getline(in, line);) {
        line = trim(line);
        size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || line[0] == ';' || eq == std::string::npos) continue;
        cfg[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return cfg;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    AddVectoredExceptionHandler(1, crash_handler);
#endif
    // Command line (for a launcher):
    //   kinst [--data DIR]                          play (DIR defaults to "data" next to the exe)
    //   kinst --install PATH... [--data DIR]        one-time: build DIR from the user's ROM set
    //   kinst --check [--data DIR]                  exit code 0 if DIR is ready to play
    //   kinst --list-renderers                      one "name<TAB>description" line per renderer
    //   kinst --list-controls                       the bindings in effect, as kinst.ini lines
    //   --renderer NAME                             overrides kinst.ini `renderer`
    //   --fx NAME|FILE.glsl                         overrides kinst.ini `fx` (F9 cycles presets)
    //   --widescreen                                same as kinst.ini `widescreen = on`
    //   --config FILE                               settings file (default kinst.ini next to the exe)
    std::string base_dir;
    if (char* base = SDL_GetBasePath()) { base_dir = base; SDL_free(base); }
    std::string data_dir = base_dir + "data";
    std::string config_path = base_dir + "kinst.ini";
    for (int i = 1; i + 1 < argc; i++)
        if (std::string(argv[i]) == "--config") config_path = argv[i + 1];
    auto cfg = load_config(config_path);
    load_controls(cfg);
    std::string renderer_name = cfg.count("renderer") ? cfg["renderer"] : renderers()[0].name;
    std::string fx = cfg.count("fx") ? cfg["fx"] : "none";
    // widescreen = on: 16:9-ish picture (see WS_W in memmap.h); unpatched screens stay 4:3 with bars.
    bus.widescreen = cfg.count("widescreen") && (cfg["widescreen"] == "1" || cfg["widescreen"] == "on" || cfg["widescreen"] == "true");
    // widescreen_fill = black | mirror | blur: the bars beside 4:3 screens (pre-rendered stages, menus).
    if (std::string f = cfg.count("widescreen_fill") ? cfg["widescreen_fill"] : "black"; f == "mirror") ws_fill = MarginFill::Mirror;
    else if (f == "blur") ws_fill = MarginFill::Blur;
    else if (f != "black") std::fprintf(stderr, "kinst.ini widescreen_fill: unknown '%s' (black, mirror, blur)\n", f.c_str());
    auto cfg_on = [&](const char* k) { return cfg.count(k) && (cfg[k] == "1" || cfg[k] == "on" || cfg[k] == "true"); };
    bool fullscreen = cfg_on("fullscreen");                  // Alt+Enter / F11 toggle it while playing
    const bool pace_display = !cfg.count("frame_pacing") || cfg["frame_pacing"] != "original";
    const bool show_fps = cfg.count("show_fps") && (cfg["show_fps"] == "1" || cfg["show_fps"] == "on" || cfg["show_fps"] == "true");
    std::vector<std::string> sources;
    enum { PLAY, INSTALL, CHECK } mode = PLAY;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--data" && i + 1 < argc) data_dir = argv[++i];
        else if (a == "--install") mode = INSTALL;
        else if (a == "--check") mode = CHECK;
        else if (a == "--renderer" && i + 1 < argc) renderer_name = argv[++i];
        else if (a == "--fx" && i + 1 < argc) fx = argv[++i];
        else if (a == "--widescreen") bus.widescreen = true;
        else if (a == "--config" && i + 1 < argc) ++i; // read above
        else if (a == "--list-controls") { print_controls(); return 0; }
        else if (a == "--list-renderers") {
            for (const RendererInfo& r : renderers()) std::printf("%s\t%s\n", r.name, r.description);
            return 0;
        }
        else if (mode == INSTALL) sources.push_back(a);
        else { std::fprintf(stderr, "unknown argument %s\n", a.c_str()); return 2; }
    }
    if (mode == INSTALL) {
        if (sources.empty()) { std::printf("error: --install needs the folder (or files) holding kinst.zip and kinst.chd\n"); return 2; }
        return ki::install(sources, data_dir, boot_stub_funcs, boot_stub_count) ? 0 : 1;
    }
    if (mode == CHECK) return ki::data_ready(data_dir) ? 0 : 1;

    ctx.bus = &bus;
    if (!ki::data_ready(data_dir) || !ki::load_boot(data_dir, ctx, bus) ||
        !bus.ide.open(data_dir + "/" + ki::DISK_FILE) || !dcs.load_sound(data_dir + "/" + ki::SOUND_FILE)) {
        std::string msg = "Game data not found in " + data_dir +
                          ".\n\nRun once:  kinst --install <folder with kinst.zip and kinst.chd>";
        std::fprintf(stderr, "%s\n", msg.c_str());
        if (!std::getenv("KI_HEADLESS")) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Killer Instinct", msg.c_str(), nullptr);
        return 1;
    }
    dcs.start({&dcs_boot, &dcs_os, &dcs_ovl_b, &dcs_ovl_a});
    bus.dcs = &dcs;
    // ponytail: guest and presenter share RAM without locking; a torn frame is the worst case.
    std::thread(guest).detach();

    if (const char* h = std::getenv("KI_HEADLESS")) { // run N seconds without a window, then dump
        // Optional scripted input: KI_INPUT="6000:1:800,8000:1:400" holds bits 0x800 on P1 at 6s for 100ms
        // (the game treats switches held 12+ timer ticks, 200ms, as stuck).
        struct Press { int at, port; uint32_t bits; };
        std::vector<Press> script;
        if (const char* s = std::getenv("KI_INPUT"))
            for (Press p; std::sscanf(s, "%d:%d:%x", &p.at, &p.port, &p.bits) == 3; s = std::strchr(s, ',') + 1) {
                script.push_back(p);
                if (!std::strchr(s, ',')) break;
            }
        std::vector<int16_t> wav; // KI_WAV=path: record the DCS output
        const auto t0 = std::chrono::steady_clock::now();
        for (int t = 0; t < std::atoi(h) * 1000 && !guest_stopped;
             t = int(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count())) {
            uint32_t held[4] = {}; // port 3 = VOLUME
            for (const Press& p : script) if (t >= p.at && t < p.at + 100) held[p.port & 3] |= p.bits;
            bus.p1 = ~held[1];
            bus.p2 = ~held[2];
            bus.volume = ~held[3];
            int16_t buf[4096];
            for (size_t n; (n = dcs.pull(buf, 4096)) > 0;) wav.insert(wav.end(), buf, buf + n);
            static int next_shot = 0; // KI_SHOTS=1: dump the displayed framebuffer once a second
            if (std::getenv("KI_SHOTS") && t >= next_shot) {
                std::vector<uint16_t> shot; // width x 240 (432 with widescreen)
                Frame f = screen_frame(shot);
                std::ofstream(std::format("fb_{:03}.bin", next_shot / 1000), std::ios::binary).write((const char*)f.pixels, f.width * f.height * 2);
                next_shot += 1000;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (bus.dcs) {
            double sum = 0;
            for (int16_t s : wav) sum += double(s) * s;
            std::printf("video: %u frames\n", uint32_t(bus.flips));
            if (bus.widescreen) std::printf("widescreen: %u rasterizer calls on other targets\n", ki::ws_foreign_targets);
            std::printf("audio: %zu samples, rms %.1f\n", wav.size(), wav.empty() ? 0.0 : std::sqrt(sum / double(wav.size())));
        }
        if (const char* path = std::getenv("KI_WAV")) {
            uint32_t rate = uint32_t(dcs.sample_rate()), bytes = uint32_t(wav.size() * 2);
            std::ofstream f(path, std::ios::binary);
            auto u32 = [&](uint32_t v) { f.write((const char*)&v, 4); };
            auto u16 = [&](uint16_t v) { f.write((const char*)&v, 2); };
            f.write("RIFF", 4); u32(36 + bytes); f.write("WAVEfmt ", 8); u32(16); u16(1); u16(1);
            u32(rate); u32(rate * 2); u16(2); u16(16); f.write("data", 4); u32(bytes);
            f.write((const char*)wav.data(), bytes);
            std::printf("wrote %zu samples at %u Hz to %s\n", wav.size(), rate, path);
        }
        dump_state();
        std::quick_exit(0);
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    // The configured renderer; if it's unknown or can't start (e.g. no OpenGL 3.3), fall back to sdl.
    // The window title names the one in use.
    auto start = [](const std::string& name) {
        std::unique_ptr<Renderer> v = make_renderer(name);
        if (!v) std::fprintf(stderr, "unknown renderer '%s' (see --list-renderers)\n", name.c_str());
        else if (!v->init(("Killer Instinct [" + name + "]").c_str(), (bus.widescreen ? ki::WS_W : ki::SCREEN_W) * 3, ki::SCREEN_H * 3)) {
            std::fprintf(stderr, "renderer '%s': %s\n", name.c_str(), v->error().c_str());
            v.reset();
        }
        return v;
    };
    std::unique_ptr<Renderer> video = start(renderer_name);
    if (!video && !(video = start("sdl"))) return 1;
    // Effect: a preset, else a .glsl path (relative = next to the exe). The title shows it.
    std::span<const char* const> presets = video->fx_presets();
    std::string title = SDL_GetWindowTitle(video->window());
    title.pop_back(); // "Killer Instinct [opengl" + ", fx]"
    auto apply_fx = [&](const std::string& name) {
        std::string path = name;
        if (name != "none" && std::find(presets.begin(), presets.end(), name) == presets.end() &&
            std::filesystem::path(name).is_relative())
            path = base_dir + name;
        if (!video->set_fx(path)) { std::fprintf(stderr, "fx: %s\n", video->error().c_str()); return; }
        fx = name;
        std::string t = title + (name == "none" ? "" : ", " + std::filesystem::path(name).filename().string()) + "]";
        SDL_SetWindowTitle(video->window(), t.c_str());
    };
    apply_fx(fx);
    auto set_fullscreen = [&](bool on) {
        fullscreen = on;
        SDL_SetWindowFullscreen(video->window(), on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        SDL_ShowCursor(on ? SDL_DISABLE : SDL_ENABLE);
    };
    if (fullscreen) set_fullscreen(true);

    // DCS output: mono 16-bit at the board's rate (31.25kHz); SDL resamples to the device.
    if (bus.dcs) {
        SDL_AudioSpec want{}, have{};
        want.freq = int(dcs.sample_rate());
        want.format = AUDIO_S16SYS;
        want.channels = 1;
        want.samples = 512;
        want.callback = audio_callback;
        if (SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0)) SDL_PauseAudioDevice(dev, 0);
        else std::fprintf(stderr, "SDL audio: %s\n", SDL_GetError());
    }

    SDL_GameController* pads[2] = {};
    // show_fps: game frames per second (framebuffer flips, not host refreshes), yellow, top right.
    std::vector<uint16_t> overlay_fb;
    std::string fps_text;
    uint32_t fps_flips = bus.flips;
    auto fps_t0 = std::chrono::steady_clock::now();
    // frame_pacing = display (default): lock game frames to the display refresh when it is close to
    // the board's 58.96Hz (or a multiple: every k-th refresh), so no frame is shown twice.
    double last_swap = bus.seconds(), swap_period = 1 / 60.0;
    uint64_t swaps = 0;
    for (bool running = true; running;) {
        for (SDL_Event e; SDL_PollEvent(&e);) {
            if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)) running = false;
            if (e.type == SDL_KEYDOWN && !e.key.repeat &&
                (e.key.keysym.sym == SDLK_F11 || (e.key.keysym.sym == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT))))
                set_fullscreen(!fullscreen);
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F9 && !e.key.repeat && !presets.empty()) {
                auto it = std::find(presets.begin(), presets.end(), fx);
                apply_fx((it == presets.end() || it + 1 == presets.end()) ? presets[0] : *(it + 1));
            }
            if (e.type == SDL_CONTROLLERDEVICEADDED) // also sent at startup for pads already plugged in
                for (auto& p : pads) if (!p) { p = SDL_GameControllerOpen(e.cdevice.which); break; }
            if (e.type == SDL_CONTROLLERDEVICEREMOVED)
                for (auto& p : pads)
                    if (p && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(p)) == e.cdevice.which) { SDL_GameControllerClose(p); p = nullptr; }
        }
        update_inputs(pads);
        // The guest paces itself off VBLANK/Count; this loop just shows the latest frame at host refresh.
        Frame frame{bus.framebuffer(), ki::SCREEN_W, ki::SCREEN_H, ki::SCREEN_W * 2};
        if (show_fps || bus.widescreen) frame = screen_frame(overlay_fb);
        if (show_fps) {
            auto now = std::chrono::steady_clock::now();
            if (double secs = std::chrono::duration<double>(now - fps_t0).count(); secs >= 1.0) {
                uint32_t f = bus.flips;
                fps_text = std::to_string(int((f - fps_flips) / secs + 0.5)) + " FPS";
                fps_flips = f, fps_t0 = now;
            }
            stamp_text(overlay_fb.data(), frame.width, frame.height, frame.pitch,
                       frame.width - 3 - text_width(fps_text), 3, fps_text, 0x03ff); // BGR555 yellow
        }
        video->draw(frame);
        if (pace_display) { // draw() returns right after the refresh it presented on (vsync)
            double now = bus.seconds(), dt = now - last_swap;
            last_swap = now;
            if (dt > 0.002 && dt < 0.05) swap_period += (dt - swap_period) * 0.05;
            int k = std::max(1, int(std::lround(1 / ki::VBLANK_HZ / swap_period)));
            double period = swap_period * k;
            if (period > 1 / 62.0 && period < 1 / 55.0 && ++swaps % k == 0) bus.display_vsync(now, period);
        }
    }

    dump_state();
    video.reset();
    SDL_Quit();
    std::quick_exit(0); // the guest thread never returns on its own
}
