#include "dcs.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace adsp {

// Control registers at 0x3fe0 + n (dcs.cpp)
enum {
    PROG_FLAG_DATA_REG = 5, PROG_FLAG_CONTROL_REG = 6, S1_AUTOBUF_REG = 15, S1_SCLKDIV_REG = 17, S1_CONTROL_REG = 18,
    TIMER_SCALE_REG = 27, TIMER_COUNT_REG = 28, TIMER_PERIOD_REG = 29, SYSCONTROL_REG = 31,
};

static Board& B(Dsp* d) { return *d->board; }
static const bool dcs_trace = std::getenv("KI_DCS_TRACE") != nullptr;

static uint32_t trace_lo = 0, trace_hi = 0;
static int trace_left = 400;
bool g_trace = [] {
    const char* r = std::getenv("KI_DCS_TRACE_RANGE");
    return r && std::sscanf(r, "%x-%x", &trace_lo, &trace_hi) == 2;
}();
std::atomic<uint32_t> g_last_pc{0};
void trace(Dsp* d, uint32_t pc) {
    g_last_pc.store(pc, std::memory_order_relaxed);
    if (pc < trace_lo || pc > trace_hi || trace_left <= 0) return;
    trace_left--;
    const Core& c = d->core;
    std::fprintf(stderr, "dsp %04x: %06x ax0=%04x ay0=%04x ar=%04x si=%04x se=%02x sr=%08x px=%04x i0=%04x m1=%d m2=%d i4=%04x cntr=%x\n",
                 pc, program_read(d, pc), c.ax0.u, c.ay0.u, c.ar.u, c.si.u, c.se.u & 0xff, c.sr.sr, d->px, d->i[0], d->m[1], d->m[2],
                 d->i[4], d->cntr);
}

// ---------------------------------------------------------------- memory map (dcs_2k_*_map)

uint16_t data_read(Dsp* d, uint32_t a) {
    Board& b = B(d);
    a &= 0x3fff;
    if (a < 0x2000) return uint16_t(b.xpm[a & 0x7ff] >> 8); // 2K RAM shared with PM, mirrored
    if (a < 0x3000) return b.sound[(b.bank * 0x1000 + (a - 0x2000)) % b.sound.size()];
    if (a >= 0x3400 && a < 0x3800) { // input latch (auto-ack on DCS1)
        b.latch_control |= Board::INPUT_EMPTY;
        set_line(d, IRQ2, false);
        return b.input_data;
    }
    if (a >= 0x3800 && a < 0x3a00) return b.iram[a - 0x3800];
    if (a >= 0x3fe0) {
        int r = int(a - 0x3fe0);
        if (r == PROG_FLAG_DATA_REG) {
            uint16_t v = uint16_t((b.ctrl[PROG_FLAG_CONTROL_REG] & b.ctrl[PROG_FLAG_DATA_REG]) | (b.progflags & ~b.ctrl[PROG_FLAG_CONTROL_REG]));
            b.progflags ^= 0x6;
            return v;
        }
        return b.ctrl[r]; // ponytail: TIMER_COUNT isn't updated live
    }
    return 0;
}

static void note_code_write(Board& b, uint32_t pm_addr) {
    if (b.compiled_here[pm_addr]) b.dirty[pm_addr] = program_read(&b.dsp, pm_addr) != b.compiled_word[pm_addr];
}

void data_write(Dsp* d, uint32_t a, uint16_t v) {
    Board& b = B(d);
    a &= 0x3fff;
    if (a < 0x2000) {
        uint32_t& w = b.xpm[a & 0x7ff];
        uint32_t nw = (uint32_t(v) << 8) | (w & 0xff);
        if (nw != w) {
            w = nw;
            for (uint32_t m : {0x800u, 0x1000u, 0x1800u}) note_code_write(b, m + (a & 0x7ff));
        }
        return;
    }
    if (a >= 0x3000 && a < 0x3400) { b.bank = v & 0x7ff; return; } // bit 11 = sound board LED
    if (a >= 0x3400 && a < 0x3800) return; // output latch: KI has no host read-back
    if (a >= 0x3800 && a < 0x3a00) { b.iram[a - 0x3800] = v; return; }
    if (a < 0x3fe0) return;
    int r = int(a - 0x3fe0);
    b.ctrl[r] = v;
    switch (r) {
    case SYSCONTROL_REG:
        if (v & 0x0200) { // bit 9: reset the ADSP and reboot from the current bank
            b.soft_reboot = true;
            d->irq_check = true;
            b.ctrl[SYSCONTROL_REG] = 0;
        }
        if (!(v & 0x0800)) { // SPORT1 disabled
            if (dcs_trace && b.ab_active) std::fprintf(stderr, "dcs: autobuffer off (SYSCONTROL %04x)\n", v);
            b.ab_active = false;
        }
        break;
    case S1_AUTOBUF_REG:
        if (!(v & 0x0002)) {
            if (dcs_trace && b.ab_active) std::fprintf(stderr, "dcs: autobuffer off (S1_AUTOBUF %04x)\n", v);
            b.ab_active = false;
        }
        break;
    case TIMER_SCALE_REG: case TIMER_COUNT_REG: case TIMER_PERIOD_REG:
        if (b.timer_on) timer_enable(d, true); // restart with the new values
        break;
    }
}

uint32_t program_read(Dsp* d, uint32_t a) {
    Board& b = B(d);
    a &= 0x3fff;
    if (a < 0x400) return b.ipm[a];
    if (a >= 0x800 && a < 0x2000) return b.xpm[a & 0x7ff];
    return 0;
}

void program_write(Dsp* d, uint32_t a, uint32_t v) {
    Board& b = B(d);
    a &= 0x3fff;
    uint32_t* w = a < 0x400 ? &b.ipm[a] : (a >= 0x800 && a < 0x2000) ? &b.xpm[a & 0x7ff] : nullptr;
    if (!w || *w == v) return;
    *w = v;
    if (a < 0x400) note_code_write(b, a);
    else for (uint32_t m : {0x800u, 0x1000u, 0x1800u}) note_code_write(b, m + (a & 0x7ff));
}

// ---------------------------------------------------------------- SPORT1 autobuffer -> DAC

void sport_tx(Dsp* d, int port, uint16_t) {
    Board& b = B(d);
    // sound_tx_callback: only SPORT1 in autobuffer mode is used
    if (port != 1) return;
    if ((b.ctrl[SYSCONTROL_REG] & 0x0800) && (b.ctrl[S1_AUTOBUF_REG] & 0x0002)) b.autobuffer_start();
    else { // transmitting without autobuffer: MAME disables the DAC
        if (dcs_trace && b.ab_active) std::fprintf(stderr, "dcs: autobuffer off (TX without autobuffer)\n");
        b.ab_active = false;
    }
}

void Board::autobuffer_start() {
    Dsp* d = &dsp;
    ab_ireg = (ctrl[S1_AUTOBUF_REG] >> 9) & 7;
    uint32_t mreg = ((ctrl[S1_AUTOBUF_REG] >> 7) & 3) | (ab_ireg & 4);
    ab_incs = d->m[mreg];
    ab_size = d->l[ab_ireg];
    uint32_t source = d->i[ab_ireg] & ~0xfu; // some DCS versions haven't incremented yet
    d->i[ab_ireg] = source;
    update_i(d, ab_ireg);
    // recompute_sample_rate
    rate = (ctrl[S1_CONTROL_REG] & 0x4000) ? CLOCK / (2.0 * (ctrl[S1_SCLKDIV_REG] + 1)) / 16.0 : 31250.0;
    if (ab_incs <= 0 || ab_size == 0) { ab_active = false; return; }
    double half = (double(ab_size) / (2.0 * ab_incs)) / rate; // seconds per half buffer
    ab_period = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(half));
    ab_next = clock::now() + ab_period;
    ab_active = true;
    if (dcs_trace) std::fprintf(stderr, "dcs: autobuffer I%u base %04x size %04x incs %d @ %.0f Hz\n", ab_ireg, source, ab_size, ab_incs, rate);
}

void Board::autobuffer_tick() {
    Dsp* d = &dsp;
    uint32_t reg = d->i[ab_ireg];
    int count = int(ab_size / (2 * uint32_t(ab_incs)));
    for (int i = 0; i < count; i++) {
        push_sample(int16_t(data_read(d, reg)));
        reg += uint32_t(ab_incs);
    }
    uint32_t base = d->base[ab_ireg];
    if (reg >= base + ab_size) {
        reg = base;
        set_line(d, IRQ1, true); // pulse: latches, then drops
        set_line(d, IRQ1, false);
    }
    d->i[ab_ireg] = reg;
}

void Board::push_sample(int16_t s) {
    size_t h = ring_head.load(std::memory_order_relaxed);
    if (h - ring_tail.load(std::memory_order_acquire) >= RING) return; // full: drop
    ring[h % RING] = s;
    ring_head.store(h + 1, std::memory_order_release);
}

size_t Board::pull(int16_t* out, size_t n) {
    size_t t = ring_tail.load(std::memory_order_relaxed), h = ring_head.load(std::memory_order_acquire);
    size_t got = std::min(n, h - t);
    for (size_t i = 0; i < got; i++) out[i] = ring[(t + i) % RING];
    ring_tail.store(t + got, std::memory_order_release);
    return got;
}

// ---------------------------------------------------------------- ADSP internal timer

void timer_enable(Dsp* d, bool on) {
    Board& b = B(d);
    b.timer_on = on;
    if (!on) return;
    double scale = (b.ctrl[TIMER_SCALE_REG] & 0xff) + 1.0;
    auto cycles = [&](double c) { return std::chrono::duration_cast<Board::clock::duration>(std::chrono::duration<double>(c / Board::CLOCK)); };
    b.timer_period = cycles(scale * (b.ctrl[TIMER_PERIOD_REG] + 1.0));
    b.timer_next = Board::clock::now() + cycles(scale * (b.ctrl[TIMER_COUNT_REG] + 1.0));
    // ponytail: a 0-period timer would spin; clamp to 10us
    b.timer_period = std::max(b.timer_period, Board::clock::duration(std::chrono::microseconds(10)));
}

// ---------------------------------------------------------------- service / idle

uint32_t service(Dsp* d, uint32_t pc) {
    Board& b = B(d);
    d->irq_check = false;
    if (b.reset_held || b.stopping || b.soft_reboot) return HALT;
    if (b.input_pending.exchange(false)) { // dcs_delayed_data_w
        b.input_data = b.pending_input;
        set_line(d, IRQ2, true);
    }
    auto now = Board::clock::now();
    if (b.ab_active && now >= b.ab_next) {
        if (now - b.ab_next > std::chrono::milliseconds(100)) b.ab_next = now; // resync after a stall
        while (b.ab_active && now >= b.ab_next) { b.autobuffer_tick(); b.ab_next += b.ab_period; }
    }
    if (b.timer_on && now >= b.timer_next) {
        set_line(d, TIMER, true);
        set_line(d, TIMER, false);
        b.timer_next = std::max(b.timer_next + b.timer_period, now);
    }
    d->irq_check = false; // set_line() above re-armed it; we check right now instead
    return check_irqs(d, pc);
}

uint32_t idle(Dsp* d, uint32_t next_pc) {
    Board& b = B(d);
    for (;;) {
        uint32_t v = service(d, next_pc);
        if (v != NO_IRQ) return v;
        auto until = Board::clock::now() + std::chrono::milliseconds(5);
        if (b.ab_active) until = std::min(until, b.ab_next);
        if (b.timer_on) until = std::min(until, b.timer_next);
        std::unique_lock lock(b.mutex);
        b.wake.wait_until(lock, until, [&] { return d->irq_check.load() || b.input_pending.load(); });
    }
}

// ---------------------------------------------------------------- host interface

bool Board::load_sound(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (f.read(reinterpret_cast<char*>(sound.data()), std::streamsize(sound.size() * 2))) return true;
    std::fprintf(stderr, "dcs: bad sound data %s\n", path.c_str());
    return false;
}

void Board::reset_w(bool run) {
    reset_held = !run;
    notify();
}

void Board::data_w(uint16_t v) {
    if (dcs_trace) {
        static const auto t0 = clock::now();
        std::fprintf(stderr, "dcs: %6.2fs host data %04x%s\n", std::chrono::duration<double>(clock::now() - t0).count(), v,
                     (latch_control & INPUT_EMPTY) ? "" : " (latch was full)");
    }
    latch_control &= uint16_t(~INPUT_EMPTY);
    pending_input = v;
    input_pending = true;
    notify();
}

void Board::start(std::vector<const Program*> progs) {
    programs = std::move(progs);
    dsp.board = this;
    thread = std::thread([this] { thread_main(); });
    thread.detach();
    // Busy DSP code only polls irq_check, so nudge it often enough for the autobuffer deadlines.
    std::thread([this] {
        while (!stopping) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); dsp.irq_check = true; }
    }).detach();
}

// ---------------------------------------------------------------- DSP thread

void Board::board_reset() { // dcs_reset
    if (dcs_trace) std::fprintf(stderr, "dcs: board reset\n");
    bank = 0;
    ab_active = false;
    timer_on = false;
    std::fill(std::begin(ctrl), std::end(ctrl), 0);
    for (int l : {IRQ0, IRQ1, IRQ2}) dsp.irq_state[l] = 0;
    latch_control = INPUT_EMPTY | OUTPUT_EMPTY;
    input_pending = false;
}

void Board::cpu_reset() { // adsp21xx_device::device_reset
    Dsp* d = &dsp;
    for (int r = 0; r < 4; r++) { write_reg1(d, 0x08 + r, int32_t(d->l[r])); write_reg1(d, r, int32_t(d->i[r])); }
    for (int r = 0; r < 4; r++) { write_reg2(d, 0x08 + r, int32_t(d->l[4 + r])); write_reg2(d, r, int32_t(d->i[4 + r])); }
    d->loop = 0xffff;
    d->loop_cond = 0;
    d->mstat = 0;
    d->sstat = 0x55;
    update_mstat(d);
    d->pc_sp = d->cntr_sp = d->stat_sp = d->loop_sp = 0;
    d->flagout = d->flagin = d->fl0 = d->fl1 = d->fl2 = 0;
    d->imask = 0;
    for (int i = 0; i < 6; i++) d->irq_state[i] = d->irq_latch[i] = 0;
}

void Board::boot() { // dcs_boot, REV_DCS1: boot page = low bytes of the current sound bank
    const uint16_t* base = &sound[(bank * 0x1000) % sound.size()];
    uint32_t pagelen = ((base[3] & 0xff) + 1) * 8;
    for (uint32_t i = 0; i < pagelen && i < 0x400; i++)
        ipm[i] = (uint32_t(base[i * 4] & 0xff) << 16) | (uint32_t(base[i * 4 + 1] & 0xff) << 8) | (base[i * 4 + 2] & 0xff);
}

// First image (in registration order) that has code for pc compiled from the word PM holds now.
// Its other words may differ; those are marked dirty and exit back here if reached.
const Program* Board::find_program(uint32_t pc) {
    for (const Program* p : programs)
        for (size_t k = 0; k < p->count; k++)
            if (p->words[k][0] == pc) {
                if (program_read(&dsp, pc) == p->words[k][1]) return p;
                break;
            }
    return nullptr;
}

void Board::run_programs() {
    uint32_t pc = 0;
    for (;;) {
        const Program* p = find_program(pc);
        if (!p) {
            // Interrupt vectors are patched at runtime with a single jump or RTI, which no image was
            // compiled from. Those are pure control flow, so take them here and dispatch onwards.
            uint32_t op = program_read(&dsp, pc);
            if ((op >> 18) == 0x18 >> 2 && (op & 15) == 15) { pc = (op >> 4) & 0x3fff; continue; } // jump imm
            if (op == 0x0a001f) { pc = pc_stack_pop_val(&dsp); stat_stack_pop(&dsp); continue; }  // rti
            if (op == 0x0a000f) { pc = pc_stack_pop_val(&dsp); continue; }                       // rts
        }
        if (!p) {
            if (missing_reported != pc) {
                std::fprintf(stderr, "dcs: no recompiled DSP code for pc %04x (pm word %06x)\n", pc, program_read(&dsp, pc));
                for (const Program* q : programs)
                    for (size_t k = 0; k < q->count; k++)
                        if (uint32_t a = q->words[k][0], w = program_read(&dsp, a); w != q->words[k][1]) {
                            std::fprintf(stderr, "  %s: pm[%04x] is %06x, compiled from %06x\n", q->name, a, w, q->words[k][1]);
                            break;
                        }
            }
            missing_reported = pc;
            return; // the board stays silent until the next reset
        }
        if (p != current) {
            current = p;
            std::fill(compiled_here.begin(), compiled_here.end(), 0);
            std::fill(dirty.begin(), dirty.end(), 0);
            for (size_t k = 0; k < p->count; k++) {
                compiled_here[p->words[k][0]] = 1;
                compiled_word[p->words[k][0]] = p->words[k][1];
            }
        }
        for (size_t k = 0; k < p->count; k++) // PM may have changed while another image ran
            dirty[p->words[k][0]] = program_read(&dsp, p->words[k][0]) != p->words[k][1];
        dsp.dirty = dirty.data();
        dsp.irq_check = true;
        p->fn(&dsp, pc);
        pc = dsp.exit_pc;
        if (reset_held || stopping) return;
        if (soft_reboot) {
            soft_reboot = false;
            if (dcs_trace) std::fprintf(stderr, "dcs: reboot from bank %03x\n", bank);
            cpu_reset();
            boot();
            pc = 0;
        }
        // reached changed code or left this image: look the new pc up again
    }
}

void Board::thread_main() {
    while (!stopping) {
        {
            std::unique_lock lock(mutex);
            wake.wait(lock, [&] { return !reset_held.load() || stopping.load(); });
        }
        if (stopping) break;
        board_reset();
        cpu_reset();
        boot();
        run_programs();
        // halted (reset asserted) or stuck on missing code: wait for the next release
        while (!reset_held && !stopping) {
            std::unique_lock lock(mutex);
            wake.wait_for(lock, std::chrono::milliseconds(50));
        }
    }
}

} // namespace adsp
