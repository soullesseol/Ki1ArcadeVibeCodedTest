#pragma once
// Midway DCS audio board, ROM-based mono revision with 2K shared RAM (Killer Instinct).
// Modeled on mame-reference/src/mame/shared/dcs.cpp (REV_DCS1, dcs_2k_program_map /
// dcs_2k_data_map). The ADSP-2105 program is statically recompiled by tools/adsprecomp.cpp;
// this is the hardware around it. All DSP state is owned by the DSP thread; the host side
// (MIPS thread, audio callback) only touches the atomics.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "adsp.h"

namespace adsp {

// A recompiled program image: entry point plus the program-memory words it was compiled from,
// so the board can tell whether the code currently in PM is the code that was compiled.
using ProgramFn = void (*)(Dsp* d, uint32_t pc);
struct Program {
    const char* name;
    ProgramFn fn;
    const uint32_t (*words)[2]; // {address, 24-bit word}
    size_t count;
};

// Interrupt-check macro used at every instruction boundary of recompiled code.
extern bool g_trace; // KI_DCS_TRACE_RANGE=lo-hi: log instructions executed in that PC range
void trace(Dsp* d, uint32_t pc);
extern std::atomic<uint32_t> g_last_pc; // last traced pc (diagnostics)
#define DSP_CHECK(A)                                                                \
    if (::adsp::g_trace) ::adsp::trace(d, A);                                       \
    if (d->dirty[A]) { d->exit_pc = A; return; }                                    \
    if (d->irq_check.load(std::memory_order_relaxed)) {                             \
        uint32_t v_ = ::adsp::service(d, A);                                        \
        if (v_ != ::adsp::NO_IRQ) {                                                 \
            if (v_ >= ::adsp::HALT) { d->exit_pc = A; return; }                     \
            pc = v_;                                                                \
            goto dispatch;                                                          \
        }                                                                           \
    }

struct Board {
    enum : uint16_t { OUTPUT_EMPTY = 0x400, INPUT_EMPTY = 0x800 };
    static constexpr double CLOCK = 10e6; // ADSP-2105 @ 10MHz

    // ---- host side (any thread) ----
    bool load_sound(const std::string& path);              // data/sound.bin: the `sound` region as built by --install
    void start(std::vector<const Program*> programs);      // spawns the DSP thread
    void reset_w(bool run);                                // sound reset line (0 = held in reset)
    void data_w(uint16_t v);                               // host -> DCS command latch
    uint16_t control_r() const { return latch_control; }   // bit 11 = input latch empty
    size_t pull(int16_t* out, size_t n);                   // audio samples for the output device
    double sample_rate() const { return rate; }

    // ---- DSP side ----
    Dsp dsp;
    std::vector<uint16_t> sound = std::vector<uint16_t>(0x800000, 0xffff); // ROM region, 16-bit words
    uint32_t ipm[0x400]{};  // internal program RAM 0x0000-0x03ff
    uint32_t xpm[0x800]{};  // external RAM: PM 0x0800-0x1fff (mirrored), DM 0x0000-0x1fff upper 16 bits
    uint16_t iram[0x200]{}; // internal data RAM 0x3800-0x39ff
    uint16_t ctrl[0x20]{};  // control registers 0x3fe0-0x3fff
    uint32_t bank = 0;
    uint16_t input_data = 0, progflags = 0;
    std::atomic<uint16_t> latch_control{INPUT_EMPTY | OUTPUT_EMPTY};

    // run state
    void thread_main();
    void board_reset();
    void cpu_reset();
    void boot();
    void run_programs();
    const Program* find_program(uint32_t pc);
    std::vector<const Program*> programs;
    const Program* current = nullptr;
    // Self-modifying code: the word each address of `current` was compiled from, and whether
    // PM now holds something else there. Recompiled code exits when it reaches a dirty address.
    std::vector<uint32_t> compiled_word = std::vector<uint32_t>(0x4000);
    std::vector<uint8_t> compiled_here = std::vector<uint8_t>(0x4000), dirty = std::vector<uint8_t>(0x4000);
    bool soft_reboot = false;

    // host -> DSP events
    std::atomic<bool> reset_held{true}, stopping{false};
    std::atomic<bool> input_pending{false};
    std::atomic<uint16_t> pending_input{0};
    std::mutex mutex;
    std::condition_variable wake;
    void notify() { dsp.irq_check = true; wake.notify_one(); }

    // SPORT1 autobuffer -> DAC (dcs_irq / sound_tx_callback)
    using clock = std::chrono::steady_clock;
    bool ab_active = false;
    uint32_t ab_ireg = 0, ab_size = 0;
    int32_t ab_incs = 0;
    clock::duration ab_period{};
    clock::time_point ab_next{};
    double rate = 31250.0;
    void autobuffer_start();
    void autobuffer_tick();
    // ADSP internal timer
    bool timer_on = false;
    clock::duration timer_period{};
    clock::time_point timer_next{};

    // audio ring (SPSC: DSP thread produces, audio callback consumes)
    static constexpr size_t RING = 1 << 15;
    int16_t ring[RING]{};
    std::atomic<size_t> ring_head{0}, ring_tail{0};
    void push_sample(int16_t s);

    std::thread thread;
    uint32_t missing_reported = ~0u;
};

} // namespace adsp
