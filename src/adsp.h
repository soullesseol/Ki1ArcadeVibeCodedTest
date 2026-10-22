#pragma once
// ADSP-2105 core state and data-path instruction semantics, for code emitted by
// tools/adsprecomp.cpp. The helpers are ported from MAME's ADSP-21xx core
// (mame-reference/src/devices/cpu/adsp2100/2100ops.hxx + adsp2100.cpp, BSD-3-Clause,
// copyright Aaron Giles) so results match MAME bit for bit. Control flow (jumps, calls,
// DO loops, interrupts, IDLE) is not here: the recompiler turns it into C++ control flow.
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>

namespace adsp {

enum : uint32_t { ZFLAG = 0x01, NFLAG = 0x02, VFLAG = 0x04, CFLAG = 0x08, SFLAG = 0x10, QFLAG = 0x20, MVFLAG = 0x40, SSFLAG = 0x80 };
enum : uint32_t { MSTAT_BANK = 0x01, MSTAT_REVERSE = 0x02, MSTAT_STICKYV = 0x04, MSTAT_SATURATE = 0x08,
                  MSTAT_INTEGER = 0x10, MSTAT_TIMER = 0x20, MSTAT_GOMODE = 0x40 };
enum : uint32_t { PC_EMPTY = 0x01, PC_OVER = 0x02, COUNT_EMPTY = 0x04, COUNT_OVER = 0x08,
                  STATUS_EMPTY = 0x10, STATUS_OVER = 0x20, LOOP_EMPTY = 0x40, LOOP_OVER = 0x80 };
// ADSP-2105 interrupt inputs (MAME adsp2100.h ADSP2105_*)
enum { IRQ0 = 0, IRQ1 = 1, IRQ2 = 2, SPORT0_RX = 3, SPORT0_TX = 4, TIMER = 5 };
constexpr uint32_t NO_IRQ = ~0u;

union Reg16 { uint16_t u; int16_t s; };
struct Core {
    Reg16 ax0, ax1, ay0, ay1, ar, af, mx0, mx1, my0, my1;
    union { struct { Reg16 mr0, mr1, mr2, mrzero; } x; uint64_t mr; } mr; // little-endian host
    Reg16 mf, si, se, sb;
    union { struct { Reg16 sr0, sr1; } x; uint32_t sr; } sr;
    Reg16 zero;
};

struct Board; // the DCS board: memory map and peripherals (dcs.h)

struct Dsp {
    Core core{}, alt{};
    uint32_t i[8]{}, l[8]{}, base[8]{}, lmask[8]{};
    int32_t m[8]{};
    uint8_t px = 0;     // 8 bits, like MAME m_px: wider values get truncated on write
    uint32_t cntr = 0;
    uint32_t astat = 0, astat_clear = ~(CFLAG | VFLAG | NFLAG | ZFLAG), mstat = 0, mstat_prev = 0, sstat = 0x55;
    uint32_t imask = 0, icntl = 0, ifc = 0;
    uint32_t pc_stack[16]{}, pc_sp = 0;
    uint32_t cntr_stack[4]{}, cntr_sp = 0;
    uint32_t loop_stack[4]{}, loop_sp = 0, loop = 0xffff, loop_cond = 0;
    uint16_t stat_stack[4][3]{};
    uint32_t stat_sp = 0;
    uint8_t irq_state[6]{}, irq_latch[6]{};
    uint32_t flagin = 0, flagout = 0, fl0 = 0, fl1 = 0, fl2 = 0;
    std::atomic<bool> irq_check{false}; // set when interrupt state may have changed; polled per instruction
    uint32_t exit_pc = 0;               // where a recompiled program stopped (see Board::run)
    const uint8_t* dirty = nullptr;     // per PM address: compiled code no longer matches PM
    Board* board = nullptr;
};

// Results of service()/idle() at or above HALT make the recompiled program return.
constexpr uint32_t HALT = 0xfffffff0u;
// Called by recompiled code at an instruction boundary when irq_check is set: applies host and
// timer events, then returns an interrupt vector to take, NO_IRQ, or >= HALT.
uint32_t service(Dsp* d, uint32_t pc);
// IDLE: wait for an interrupt (returns its vector, pushing next_pc) or >= HALT.
uint32_t idle(Dsp* d, uint32_t next_pc);

// Memory map, implemented by the board (dcs.cpp).
uint16_t data_read(Dsp* d, uint32_t a);
void data_write(Dsp* d, uint32_t a, uint16_t v);
uint32_t program_read(Dsp* d, uint32_t a);
void program_write(Dsp* d, uint32_t a, uint32_t v);
void sport_tx(Dsp* d, int port, uint16_t v);
void timer_enable(Dsp* d, bool on);

// ---- tables (adsp21xx_device::create_tables) ----
// m_reverse_table / m_mask_table as functions: both are only hit on rare paths
// (bit-reversed DAG1 addressing, writes to L registers).
inline uint32_t reverse14(uint32_t i) {
    uint32_t v = 0;
    for (int b = 0; b < 14; b++) if (i & (1u << b)) v |= 1u << (13 - b);
    return v;
}
inline uint32_t lmask_for(uint32_t l) { // circular-buffer base mask for length l
    for (int n = 13; n >= 0; n--) if (l > (1u << n)) return 0x3fff & ~((2u << n) - 1);
    return 0x3fff;
}
inline constexpr auto condition_table = [] {
    std::array<uint8_t, 0x1000> t{};
    for (int i = 0; i < 0x100; i++) {
        bool az = i & ZFLAG, an = i & NFLAG, av = i & VFLAG, ac = i & CFLAG, mv = i & MVFLAG, as = i & SFLAG;
        bool c[16] = {az, !az, !((an != av) || az), (an != av) || az, an != av, an == av, av, !av,
                      ac, !ac, as, !as, mv, !mv, false, true};
        for (int k = 0; k < 16; k++) t[(k << 8) | i] = c[k];
    }
    return t;
}();

inline int32_t sext(uint32_t v, int bits) { return int32_t(v << (32 - bits)) >> (32 - bits); }

// ---- MSTAT ----
inline void update_mstat(Dsp* d) {
    if ((d->mstat ^ d->mstat_prev) & MSTAT_BANK) std::swap(d->core, d->alt);
    if ((d->mstat ^ d->mstat_prev) & MSTAT_TIMER) timer_enable(d, (d->mstat & MSTAT_TIMER) != 0);
    d->astat_clear = (d->mstat & MSTAT_STICKYV) ? ~(CFLAG | NFLAG | ZFLAG) : ~(CFLAG | VFLAG | NFLAG | ZFLAG);
    d->mstat_prev = d->mstat;
}

// ---- stacks ----
inline uint32_t pc_stack_top(Dsp* d) { return d->pc_stack[d->pc_sp > 0 ? d->pc_sp - 1 : 0]; }
inline void pc_stack_push_val(Dsp* d, uint32_t v) {
    if (d->pc_sp < 16) { d->pc_stack[d->pc_sp++] = v; d->sstat &= ~PC_EMPTY; }
    else d->sstat |= PC_OVER;
}
inline uint32_t pc_stack_pop_val(Dsp* d) {
    if (d->pc_sp > 0 && --d->pc_sp == 0) d->sstat |= PC_EMPTY;
    return d->pc_stack[d->pc_sp];
}
inline void cntr_stack_push(Dsp* d) {
    if (d->cntr_sp < 4) { d->cntr_stack[d->cntr_sp++] = d->cntr; d->sstat &= ~COUNT_EMPTY; }
    else d->sstat |= COUNT_OVER;
}
inline void cntr_stack_pop(Dsp* d) {
    if (d->cntr_sp > 0 && --d->cntr_sp == 0) d->sstat |= COUNT_EMPTY;
    d->cntr = d->cntr_stack[d->cntr_sp];
}
inline void loop_stack_push(Dsp* d, uint32_t v) {
    if (d->loop_sp < 4) {
        d->loop_stack[d->loop_sp++] = v;
        d->loop = v >> 4;
        d->loop_cond = v & 15;
        d->sstat &= ~LOOP_EMPTY;
    } else d->sstat |= LOOP_OVER;
}
inline void loop_stack_pop(Dsp* d) {
    if (d->loop_sp == 0) return;
    if (--d->loop_sp == 0) { d->loop = 0xffff; d->loop_cond = 0; d->sstat |= LOOP_EMPTY; }
    else { d->loop = d->loop_stack[d->loop_sp - 1] >> 4; d->loop_cond = d->loop_stack[d->loop_sp - 1] & 15; }
}
inline void stat_stack_push(Dsp* d) {
    if (d->stat_sp < 4) {
        d->stat_stack[d->stat_sp][0] = uint16_t(d->mstat);
        d->stat_stack[d->stat_sp][1] = uint16_t(d->imask);
        d->stat_stack[d->stat_sp][2] = uint16_t(d->astat);
        d->stat_sp++;
        d->sstat &= ~STATUS_EMPTY;
    } else d->sstat |= STATUS_OVER;
}
inline void stat_stack_pop(Dsp* d) {
    if (d->stat_sp > 0 && --d->stat_sp == 0) d->sstat |= STATUS_EMPTY;
    d->mstat = d->stat_stack[d->stat_sp][0];
    update_mstat(d);
    d->imask = d->stat_stack[d->stat_sp][1];
    d->astat = d->stat_stack[d->stat_sp][2];
    d->irq_check = true;
}

// ---- conditions ----
inline bool cond(Dsp* d, uint32_t c) {
    if (c != 14) return condition_table[(c << 8) | d->astat];
    if (int32_t(--d->cntr) > 0) return true; // CE: count not expired
    cntr_stack_pop(d);
    return false;
}

// ---- registers ----
inline void update_i(Dsp* d, int w) { d->base[w] = d->i[w] & d->lmask[w]; }
inline void update_l(Dsp* d, int w) { d->lmask[w] = lmask_for(d->l[w] & 0x3fff); d->base[w] = d->i[w] & d->lmask[w]; }

inline void write_reg0(Dsp* d, int r, int32_t v) {
    Core& c = d->core;
    switch (r) {
    case 0x0: c.ax0.s = int16_t(v); break;
    case 0x1: c.ax1.s = int16_t(v); break;
    case 0x2: c.mx0.s = int16_t(v); break;
    case 0x3: c.mx1.s = int16_t(v); break;
    case 0x4: c.ay0.s = int16_t(v); break;
    case 0x5: c.ay1.s = int16_t(v); break;
    case 0x6: c.my0.s = int16_t(v); break;
    case 0x7: c.my1.s = int16_t(v); break;
    case 0x8: c.si.s = int16_t(v); break;
    case 0x9: c.se.s = int8_t(v); break;
    case 0xa: c.ar.s = int16_t(v); break;
    case 0xb: c.mr.x.mr0.s = int16_t(v); break;
    case 0xc: c.mr.x.mr1.s = int16_t(v); c.mr.x.mr2.s = int16_t(int16_t(v) >> 15); break;
    case 0xd: c.mr.x.mr2.s = int8_t(v); break;
    case 0xe: c.sr.x.sr0.s = int16_t(v); break;
    case 0xf: c.sr.x.sr1.s = int16_t(v); break;
    }
}
inline void write_dag_reg(Dsp* d, int index, int kind, int32_t v) { // write_reg1/2: kind 0=I 1=M 2=L
    switch (kind) {
    case 0: d->i[index] = uint32_t(v) & 0x3fff; update_i(d, index); break;
    case 1: d->m[index] = sext(uint32_t(v), 14); break;
    case 2: d->l[index] = uint32_t(v) & 0x3fff; update_l(d, index); break;
    }
}
inline void write_reg1(Dsp* d, int r, int32_t v) { write_dag_reg(d, r & 3, r >> 2, v); }
inline void write_reg2(Dsp* d, int r, int32_t v) { write_dag_reg(d, 4 + (r & 3), r >> 2, v); }
inline void write_reg3(Dsp* d, int r, int32_t v) {
    switch (r) {
    case 0x0: d->astat = uint32_t(v) & 0xff; break;
    case 0x1: d->mstat = uint32_t(v) & 0x7f; update_mstat(d); break;
    case 0x3: d->imask = uint32_t(v) & 0x3f; d->irq_check = true; break;
    case 0x4: d->icntl = uint32_t(v) & 0x1f; d->irq_check = true; break;
    case 0x5: cntr_stack_push(d); d->cntr = uint32_t(v) & 0x3fff; break;
    case 0x6: d->core.sb.s = int16_t(sext(uint32_t(v), 5)); break;
    case 0x7: d->px = uint8_t(v); break;
    case 0x9: sport_tx(d, 0, uint16_t(v)); break;
    case 0xb: sport_tx(d, 1, uint16_t(v)); break;
    case 0xc: // IFC: clear (bits 1-5) / force (bits 7-11) latched interrupts, ADSP-2101 layout
        d->ifc = uint32_t(v);
        if (v & 0x002) d->irq_latch[IRQ0] = 0;
        if (v & 0x004) d->irq_latch[IRQ1] = 0;
        if (v & 0x008) d->irq_latch[SPORT0_RX] = 0;
        if (v & 0x010) d->irq_latch[SPORT0_TX] = 0;
        if (v & 0x020) d->irq_latch[IRQ2] = 0;
        if (v & 0x080) d->irq_latch[IRQ0] = 1;
        if (v & 0x100) d->irq_latch[IRQ1] = 1;
        if (v & 0x200) d->irq_latch[SPORT0_RX] = 1;
        if (v & 0x400) d->irq_latch[SPORT0_TX] = 1;
        if (v & 0x800) d->irq_latch[IRQ2] = 1;
        d->irq_check = true;
        break;
    case 0xd: d->cntr = uint32_t(v) & 0x3fff; break;
    case 0xf: pc_stack_push_val(d, uint32_t(v) & 0x3fff); break;
    }
}

inline int32_t read_reg0(Dsp* d, int r) {
    const Core& c = d->core;
    const Reg16* regs[16] = {&c.ax0, &c.ax1, &c.mx0, &c.mx1, &c.ay0, &c.ay1, &c.my0, &c.my1,
                             &c.si, &c.se, &c.ar, &c.mr.x.mr0, &c.mr.x.mr1, &c.mr.x.mr2, &c.sr.x.sr0, &c.sr.x.sr1};
    return regs[r]->s;
}
inline int32_t read_dag_reg(Dsp* d, int index, int kind) {
    return kind == 0 ? int32_t(d->i[index]) : kind == 1 ? d->m[index] : int32_t(d->l[index]);
}
inline int32_t read_reg1(Dsp* d, int r) { return read_dag_reg(d, r & 3, (r >> 2) == 3 ? 2 : r >> 2); }
inline int32_t read_reg2(Dsp* d, int r) { return read_dag_reg(d, 4 + (r & 3), (r >> 2) == 3 ? 2 : r >> 2); }
inline int32_t read_reg3(Dsp* d, int r) {
    switch (r) {
    case 0x0: return int32_t(d->astat);
    case 0x1: return int32_t(d->mstat);
    case 0x2: return int32_t(d->sstat);
    case 0x3: return int32_t(d->imask);
    case 0x4: return int32_t(d->icntl);
    case 0x5: return int32_t(d->cntr);
    case 0x6: return d->core.sb.s;
    case 0x7: return int32_t(d->px);
    case 0xf: return int32_t(pc_stack_pop_val(d));
    }
    return 0; // SPORT receive (8, 10) is unused on DCS
}

// ---- DAGs ----
inline void dag_step(Dsp* d, uint32_t ireg, uint32_t mreg) {
    uint32_t base = d->base[ireg], i = (d->i[ireg] + uint32_t(d->m[mreg])) & 0x3fff, l = d->l[ireg];
    if (i < base) i += l;
    else if (i >= base + l) i -= l;
    d->i[ireg] = i;
}
inline void modify_address(Dsp* d, uint32_t ireg, uint32_t mreg) { dag_step(d, ireg, mreg); }
inline void data_write_dag1(Dsp* d, uint32_t op, int32_t v) {
    uint32_t ireg = (op >> 2) & 3, mreg = op & 3, i = d->i[ireg];
    data_write(d, (d->mstat & MSTAT_REVERSE) ? reverse14(i & 0x3fff) : i, uint16_t(v));
    dag_step(d, ireg, mreg);
}
inline uint32_t data_read_dag1(Dsp* d, uint32_t op) {
    uint32_t ireg = (op >> 2) & 3, mreg = op & 3, i = d->i[ireg];
    uint32_t r = data_read(d, (d->mstat & MSTAT_REVERSE) ? reverse14(i & 0x3fff) : i);
    dag_step(d, ireg, mreg);
    return r;
}
inline void data_write_dag2(Dsp* d, uint32_t op, int32_t v) {
    uint32_t ireg = 4 + ((op >> 2) & 3), mreg = 4 + (op & 3);
    data_write(d, d->i[ireg], uint16_t(v));
    dag_step(d, ireg, mreg);
}
inline uint32_t data_read_dag2(Dsp* d, uint32_t op) {
    uint32_t ireg = 4 + ((op >> 2) & 3), mreg = 4 + (op & 3);
    uint32_t r = data_read(d, d->i[ireg]);
    dag_step(d, ireg, mreg);
    return r;
}
inline void pgm_write_dag2(Dsp* d, uint32_t op, int32_t v) {
    uint32_t ireg = 4 + ((op >> 2) & 3), mreg = 4 + (op & 3);
    program_write(d, d->i[ireg], ((uint32_t(v) << 8) | d->px) & 0xffffff);
    dag_step(d, ireg, mreg);
}
inline uint32_t pgm_read_dag2(Dsp* d, uint32_t op) {
    uint32_t ireg = 4 + ((op >> 2) & 3), mreg = 4 + (op & 3);
    uint32_t r = program_read(d, d->i[ireg]);
    d->px = uint8_t(r);
    dag_step(d, ireg, mreg);
    return r >> 8;
}

// ---- operand selection (m_alu_xregs etc.) ----
inline Reg16& xreg(Dsp* d, uint32_t x) { // ALU/MAC xop 2..7
    Core& c = d->core;
    Reg16* r[8] = {&c.ax0, &c.ax1, &c.ar, &c.mr.x.mr0, &c.mr.x.mr1, &c.mr.x.mr2, &c.sr.x.sr0, &c.sr.x.sr1};
    return *r[x];
}
inline uint16_t alu_x(Dsp* d, uint32_t x) { return xreg(d, x).u; }
inline uint16_t alu_y(Dsp* d, uint32_t y) { Core& c = d->core; Reg16* r[4] = {&c.ay0, &c.ay1, &c.af, &c.zero}; return r[y]->u; }
inline Reg16& mac_xr(Dsp* d, uint32_t x) { return x == 0 ? d->core.mx0 : x == 1 ? d->core.mx1 : xreg(d, x); }
inline Reg16& mac_yr(Dsp* d, uint32_t y) { Core& c = d->core; Reg16* r[4] = {&c.my0, &c.my1, &c.mf, &c.zero}; return *r[y]; }
inline Reg16& shift_xr(Dsp* d, uint32_t x) { return x < 2 ? d->core.si : xreg(d, x); }

// ---- ALU (alu_op_ar / alu_op_af) ----
template <bool AF> inline void alu_op(Dsp* d, uint32_t op) {
    int32_t xop = (op >> 8) & 7, yop = (op >> 11) & 3, res = 0;
    uint32_t& astat = d->astat;
    auto clr = [&] { astat &= d->astat_clear; };
    auto nz = [&](int32_t r) { clr(); astat |= (r >> 14) & 0x02; astat |= ((r & 0xffff) == 0); };
    auto v = [&](int32_t s, int32_t dd, int32_t r) { astat |= ((s ^ dd ^ r ^ (r >> 1)) >> 13) & 0x04; };
    switch ((op >> 13) & 15) {
    case 0x0: res = alu_y(d, yop); nz(res); break;
    case 0x1: yop = alu_y(d, yop); res = yop + 1; nz(res); if (yop == 0x7fff) astat |= VFLAG; else if (yop == 0xffff) astat |= CFLAG; break;
    case 0x2: xop = alu_x(d, xop); yop = alu_y(d, yop); yop += (astat & CFLAG) >> 3; res = xop + yop;
              nz(res); v(xop, yop, res); astat |= (res >> 13) & 0x08; break;
    case 0x3: xop = alu_x(d, xop); yop = alu_y(d, yop); res = xop + yop; nz(res); v(xop, yop, res); astat |= (res >> 13) & 0x08; break;
    case 0x4: res = alu_y(d, yop) ^ 0xffff; nz(res); break;
    case 0x5: yop = alu_y(d, yop); res = -yop; nz(res); if (yop == 0x8000) astat |= VFLAG; if (yop == 0) astat |= CFLAG; break;
    case 0x6: xop = alu_x(d, xop); yop = alu_y(d, yop); res = xop - yop + int32_t((astat & CFLAG) >> 3) - 1;
              nz(res); v(xop, yop, res); astat |= (~res >> 13) & 0x08; break;
    case 0x7: xop = alu_x(d, xop); yop = alu_y(d, yop); res = xop - yop; nz(res); v(xop, yop, res); astat |= (~res >> 13) & 0x08; break;
    case 0x8: yop = alu_y(d, yop); res = yop - 1; nz(res); if (yop == 0x8000) astat |= VFLAG; else if (yop == 0) astat |= CFLAG; break;
    case 0x9: xop = alu_x(d, xop); yop = alu_y(d, yop); res = yop - xop; nz(res); v(yop, xop, res); astat |= (~res >> 13) & 0x08; break;
    case 0xa: xop = alu_x(d, xop); yop = alu_y(d, yop); res = yop - xop + int32_t((astat & CFLAG) >> 3) - 1;
              nz(res); v(yop, xop, res); astat |= (~res >> 13) & 0x08; break;
    case 0xb: res = alu_x(d, xop) ^ 0xffff; nz(res); break;
    case 0xc: res = alu_x(d, xop) & alu_y(d, yop); nz(res); break;
    case 0xd: res = alu_x(d, xop) | alu_y(d, yop); nz(res); break;
    case 0xe: res = alu_x(d, xop) ^ alu_y(d, yop); nz(res); break;
    case 0xf:
        xop = alu_x(d, xop);
        res = (xop & 0x8000) ? -xop : xop;
        clr(); astat &= ~SFLAG;
        if (xop == 0) astat |= ZFLAG;
        if (xop == 0x8000) astat |= NFLAG | VFLAG;
        if (xop & 0x8000) astat |= SFLAG;
        break;
    }
    if constexpr (AF) d->core.af.u = uint16_t(res);
    else {
        if ((d->mstat & MSTAT_SATURATE) && (astat & VFLAG)) res = (astat & CFLAG) ? -32768 : 32767;
        d->core.ar.u = uint16_t(res);
    }
}
inline void alu_op_ar(Dsp* d, uint32_t op) { alu_op<false>(d, op); }
inline void alu_op_af(Dsp* d, uint32_t op) { alu_op<true>(d, op); }

// ---- MAC (mac_op_mr / mac_op_mf) ----
template <bool MF> inline void mac_op(Dsp* d, uint32_t op) {
    int shift = (d->mstat & MSTAT_INTEGER) ? 0 : 1;
    uint32_t xi = (op >> 8) & 7, yi = (op >> 11) & 3, f = (op >> 13) & 15;
    if (f == 0) return;
    // signedness of x/y per function: RND/SS rows signed*signed; SU, US, UU otherwise
    static constexpr uint8_t xs[16] = {0, 1, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0};
    static constexpr uint8_t ys[16] = {0, 1, 1, 1, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0};
    int32_t xop = xs[f] ? mac_xr(d, xi).s : mac_xr(d, xi).u;
    int32_t yop = ys[f] ? mac_yr(d, yi).s : mac_yr(d, yi).u;
    int32_t temp = int32_t((uint32_t(xop) * uint32_t(yop)) << shift); // wraps like MAME's int math, without UB
    int64_t mr = int64_t(d->core.mr.mr), res;
    switch (f) {
    case 0x1: case 0x4: case 0x5: case 0x6: case 0x7: res = int64_t(temp); break;
    case 0x2: case 0x8: case 0x9: case 0xa: case 0xb: res = mr + int64_t(temp); break;
    default: res = mr - int64_t(temp); break; // 0x3, 0xc-0xf
    }
    if (f <= 3) { // RND
        temp &= 0xffff;
        res += 0x8000;
        if (temp == 0x8000) res &= ~int64_t(0x10000);
    }
    if constexpr (MF) d->core.mf.u = uint16_t(uint32_t(res) >> 16);
    else {
        uint32_t t = uint32_t(uint64_t(res) >> 31) & 0x1ff;
        d->astat &= ~MVFLAG;
        if (t != 0x000 && t != 0x1ff) d->astat |= MVFLAG;
        d->core.mr.mr = uint64_t(res);
    }
}
inline void mac_op_mr(Dsp* d, uint32_t op) { mac_op<false>(d, op); }
inline void mac_op_mf(Dsp* d, uint32_t op) { mac_op<true>(d, op); }

// ---- shifter (shift_op uses SE, shift_op_imm the low byte; imm has no EXP forms) ----
inline void shift_op_sc(Dsp* d, uint32_t op, int8_t sc, bool imm) {
    int32_t xop;
    uint32_t res = 0, xi = (op >> 8) & 7, f = (op >> 11) & 15;
    uint32_t& sr = d->core.sr.sr;
    Reg16& x = shift_xr(d, xi);
    auto lsh = [&](int32_t v) -> uint32_t { return sc > 0 ? (sc < 32 ? uint32_t(v) << sc : 0) : (sc > -32 ? uint32_t(v) >> -sc : 0); };
    auto ash = [&](int32_t v) -> uint32_t { return sc > 0 ? (sc < 32 ? uint32_t(v << sc) : 0) : uint32_t(sc > -32 ? v >> -sc : v >> 31); };
    switch (f) {
    case 0x0: case 0x1: res = lsh(int32_t(uint32_t(x.u) << 16)); break;
    case 0x2: case 0x3: res = lsh(x.u); break;
    case 0x4: case 0x5: res = ash(int32_t(uint32_t(int32_t(x.s)) << 16)); break;
    case 0x6: case 0x7: res = ash(x.s); break;
    case 0x8: case 0x9:
        xop = int32_t(uint32_t(int32_t(x.s)) << 16);
        if (sc > 0) { xop = int32_t((uint32_t(xop) >> 1) | ((d->astat & CFLAG) << 28)); res = uint32_t(xop >> (sc - 1)); }
        else res = sc > -32 ? uint32_t(xop) << -sc : 0;
        break;
    case 0xa: case 0xb:
        xop = x.u;
        if (sc > 0) res = sc < 32 ? uint32_t(xop) >> sc : 0;
        else res = sc > -32 ? uint32_t(xop) << -sc : 0;
        break;
    default: {
        if (imm) return;
        auto clo = [](int32_t v) { return int32_t(std::countl_one(uint32_t(v))); };
        auto clz = [](int32_t v) { return int32_t(std::countl_zero(uint32_t(v))); };
        xop = x.s;
        int32_t r;
        switch (f) {
        case 0xc:
            if (xop < 0) { d->astat |= SSFLAG; r = clo(xop) - 16 - 1; } else { d->astat &= ~SSFLAG; r = clz(xop) - 16 - 1; }
            d->core.se.s = int16_t(-r);
            break;
        case 0xd:
            if (d->astat & VFLAG) { d->core.se.s = 1; if (xop < 0) d->astat &= ~SSFLAG; else d->astat |= SSFLAG; }
            else {
                if (xop < 0) { d->astat |= SSFLAG; r = clo(xop) - 16 - 1; } else { d->astat &= ~SSFLAG; r = clz(xop) - 16 - 1; }
                d->core.se.s = int16_t(-r);
            }
            break;
        case 0xe:
            if (d->core.se.s == -15) { r = clz(((d->astat & SSFLAG) ? ~xop : xop) & 0xffff) - 1; d->core.se.s = int16_t(-r); }
            break;
        case 0xf:
            r = clz(xop < 0 ? ~xop : xop) - 16 - 1;
            if (r < -d->core.sb.s) d->core.sb.s = int16_t(-r);
            break;
        }
        return;
    }
    }
    if (f & 1) sr |= res; else sr = res;
}
inline void shift_op(Dsp* d, uint32_t op) { shift_op_sc(d, op, int8_t(d->core.se.s), false); }
inline void shift_op_imm(Dsp* d, uint32_t op) { shift_op_sc(d, op, int8_t(op & 0xff), true); }

// ---- interrupts (adsp2101_device::check_irqs / generate_irq) ----
inline void set_line(Dsp* d, int line, bool on) {
    if (on && !d->irq_state[line]) d->irq_latch[line] = 1;
    d->irq_state[line] = on;
    d->irq_check = true;
}
// Takes the highest-priority pending interrupt before executing the instruction at pc.
// Returns its vector, or NO_IRQ.
inline uint32_t check_irqs(Dsp* d, uint32_t pc) {
    static constexpr int lines[6] = {IRQ2, SPORT0_TX, SPORT0_RX, IRQ1, IRQ0, TIMER};
    for (int idx = 0; idx < 6; idx++) {
        int w = lines[idx];
        bool pending = (w == IRQ2) ? ((d->icntl & 4) ? d->irq_latch[w] : d->irq_state[w])
                     : (w == IRQ1) ? ((d->icntl & 2) ? d->irq_latch[w] : d->irq_state[w])
                     : (w == IRQ0) ? ((d->icntl & 1) ? d->irq_latch[w] : d->irq_state[w])
                     : d->irq_latch[w];
        if (!pending || !(d->imask & (0x20u >> idx))) continue;
        d->irq_latch[w] = 0;
        pc_stack_push_val(d, pc);
        stat_stack_push(d);
        d->imask &= (d->icntl & 0x10) ? ~(0x3fu >> idx) : ~0x3fu;
        return 0x04 + idx * 4;
    }
    return NO_IRQ;
}

// ---- non-control-flow instructions (the data-path cases of adsp21xx_device::execute_run) ----
inline void exec(Dsp* d, uint32_t op) { // fallback; adsprecomp emits specialized calls
    uint32_t t;
    auto r4 = [&](int s) { return (op >> s) & 15; };
    switch (op >> 16) {
    case 0x00: break;
    case 0x02: // modify flag out
        if (cond(d, op & 15)) {
            if (op & 0x020) d->flagout = 0;
            if (op & 0x010) d->flagout ^= 1;
            if (op & 0x080) d->fl0 = 0;
            if (op & 0x040) d->fl0 ^= 1;
            if (op & 0x200) d->fl1 = 0;
            if (op & 0x100) d->fl1 ^= 1;
            if (op & 0x800) d->fl2 = 0;
            if (op & 0x400) d->fl2 ^= 1;
        }
        break;
    case 0x04: // stack control
        if (op & 0x10) pc_stack_pop_val(d);
        if (op & 0x08) loop_stack_pop(d);
        if (op & 0x04) cntr_stack_pop(d);
        if (op & 0x02) { if (op & 0x01) stat_stack_pop(d); else stat_stack_push(d); }
        break;
    case 0x05: // saturate MR
        if (d->astat & MVFLAG) {
            Core& c = d->core;
            if (c.mr.x.mr2.u & 0x80) c.mr.x.mr2.u = 0xffff, c.mr.x.mr1.u = 0x8000, c.mr.x.mr0.u = 0x0000;
            else c.mr.x.mr2.u = 0x0000, c.mr.x.mr1.u = 0x7fff, c.mr.x.mr0.u = 0xffff;
        }
        break;
    case 0x06: { // DIVS
        uint32_t xop = alu_x(d, (op >> 8) & 7), yop = alu_y(d, (op >> 11) & 3);
        t = xop ^ yop;
        d->astat = (d->astat & ~QFLAG) | ((t >> 10) & QFLAG);
        d->core.af.u = uint16_t((yop << 1) | (d->core.ay0.u >> 15));
        d->core.ay0.u = uint16_t((d->core.ay0.u << 1) | (t >> 15));
        break;
    }
    case 0x07: { // DIVQ
        uint32_t xop = alu_x(d, (op >> 8) & 7);
        uint32_t res = (d->astat & QFLAG) ? d->core.af.u + xop : d->core.af.u - xop;
        t = res ^ xop;
        d->astat = (d->astat & ~QFLAG) | ((t >> 10) & QFLAG);
        d->core.af.u = uint16_t((res << 1) | (d->core.ay0.u >> 15));
        d->core.ay0.u = uint16_t((d->core.ay0.u << 1) | ((~t >> 15) & 1));
        break;
    }
    case 0x08: break;
    case 0x09: t = (op >> 2) & 7; modify_address(d, t, (t & 4) | (op & 3)); break;
    case 0x0c: // mode control
        if (op & 0x0008) d->mstat = (op & 0x0004) ? (d->mstat | MSTAT_GOMODE) : (d->mstat & ~MSTAT_GOMODE);
        if (op & 0x2000) d->mstat = (op & 0x1000) ? (d->mstat | MSTAT_INTEGER) : (d->mstat & ~MSTAT_INTEGER);
        if (op & 0x8000) d->mstat = (op & 0x4000) ? (d->mstat | MSTAT_TIMER) : (d->mstat & ~MSTAT_TIMER);
        if (op & 0x0020) d->mstat = (op & 0x0010) ? (d->mstat | MSTAT_BANK) : (d->mstat & ~MSTAT_BANK);
        if (op & 0x0080) d->mstat = (op & 0x0040) ? (d->mstat | MSTAT_REVERSE) : (d->mstat & ~MSTAT_REVERSE);
        if (op & 0x0200) d->mstat = (op & 0x0100) ? (d->mstat | MSTAT_STICKYV) : (d->mstat & ~MSTAT_STICKYV);
        if (op & 0x0800) d->mstat = (op & 0x0400) ? (d->mstat | MSTAT_SATURATE) : (d->mstat & ~MSTAT_SATURATE);
        update_mstat(d);
        break;
    case 0x0d: { // internal data move
        int32_t (*rd[4])(Dsp*, int) = {read_reg0, read_reg1, read_reg2, read_reg3};
        void (*wr[4])(Dsp*, int, int32_t) = {write_reg0, write_reg1, write_reg2, write_reg3};
        wr[(op >> 10) & 3](d, r4(4), rd[(op >> 8) & 3](d, r4(0)));
        break;
    }
    case 0x0e: if (cond(d, op & 15)) shift_op(d, op); break;
    case 0x0f: shift_op_imm(d, op); break;
    case 0x10: shift_op(d, op); t = uint32_t(read_reg0(d, r4(0))); write_reg0(d, r4(4), int32_t(t)); break;
    case 0x11:
        if (op & 0x8000) { pgm_write_dag2(d, op, read_reg0(d, r4(4))); shift_op(d, op); }
        else { shift_op(d, op); write_reg0(d, r4(4), int32_t(pgm_read_dag2(d, op))); }
        break;
    case 0x12:
        if (op & 0x8000) { data_write_dag1(d, op, read_reg0(d, r4(4))); shift_op(d, op); }
        else { shift_op(d, op); write_reg0(d, r4(4), int32_t(data_read_dag1(d, op))); }
        break;
    case 0x13:
        if (op & 0x8000) { data_write_dag2(d, op, read_reg0(d, r4(4))); shift_op(d, op); }
        else { shift_op(d, op); write_reg0(d, r4(4), int32_t(data_read_dag2(d, op))); }
        break;
    case 0x20: case 0x21: if (cond(d, op & 15)) mac_op_mr(d, op); break;
    case 0x22: case 0x23: if (cond(d, op & 15)) alu_op_ar(d, op); break;
    case 0x24: case 0x25: if (cond(d, op & 15)) mac_op_mf(d, op); break;
    case 0x26: case 0x27: if (cond(d, op & 15)) alu_op_af(d, op); break;
    case 0x28: case 0x29: t = uint32_t(read_reg0(d, r4(0))); mac_op_mr(d, op); write_reg0(d, r4(4), int32_t(t)); break;
    case 0x2a: case 0x2b: t = uint32_t(read_reg0(d, r4(0))); alu_op_ar(d, op); write_reg0(d, r4(4), int32_t(t)); break;
    case 0x2c: case 0x2d: t = uint32_t(read_reg0(d, r4(0))); mac_op_mf(d, op); write_reg0(d, r4(4), int32_t(t)); break;
    case 0x2e: case 0x2f: t = uint32_t(read_reg0(d, r4(0))); alu_op_af(d, op); write_reg0(d, r4(4), int32_t(t)); break;
    case 0x30: case 0x31: case 0x32: case 0x33: write_reg0(d, r4(0), sext(op >> 4, 14)); break;
    case 0x34: case 0x35: case 0x36: case 0x37: write_reg1(d, r4(0), sext(op >> 4, 14)); break;
    case 0x38: case 0x39: case 0x3a: case 0x3b: write_reg2(d, r4(0), sext(op >> 4, 14)); break;
    case 0x3c: case 0x3d: case 0x3e: case 0x3f: write_reg3(d, r4(0), sext(op >> 4, 14)); break;
    default:
        if ((op >> 16) >= 0x40 && (op >> 16) <= 0x4f) { write_reg0(d, r4(0), int32_t((op >> 4) & 0xffff)); break; }
        if ((op >> 16) >= 0x50 && (op >> 16) <= 0x7f) { // ALU/MAC with one memory transfer
            uint32_t g = (op >> 16) & 0x7e; // pairs
            void (*fn[4])(Dsp*, uint32_t) = {mac_op_mr, alu_op_ar, mac_op_mf, alu_op_af};
            auto f = fn[(g >> 1) & 3];
            int kind = (g >> 3) & 1;           // 0 = read, 1 = write
            int mem = (g - 0x50) >> 4;          // 0 = PM DAG2, 1 = DM DAG1, 2 = DM DAG2
            if (!kind) {
                f(d, op);
                uint32_t v = mem == 0 ? pgm_read_dag2(d, op) : mem == 1 ? data_read_dag1(d, op) : data_read_dag2(d, op);
                write_reg0(d, r4(4), int32_t(v));
            } else {
                int32_t v = read_reg0(d, r4(4));
                if (mem == 0) pgm_write_dag2(d, op, v); else if (mem == 1) data_write_dag1(d, op, v); else data_write_dag2(d, op, v);
                f(d, op);
            }
            break;
        }
        if ((op >> 16) >= 0x80 && (op >> 16) <= 0x8f) {
            int32_t v = int32_t(data_read(d, (op >> 4) & 0x3fff));
            void (*wr[4])(Dsp*, int, int32_t) = {write_reg0, write_reg1, write_reg2, write_reg3};
            wr[(op >> 18) & 3](d, r4(0), v);
            break;
        }
        if ((op >> 16) >= 0x90 && (op >> 16) <= 0x9f) {
            int32_t (*rd[4])(Dsp*, int) = {read_reg0, read_reg1, read_reg2, read_reg3};
            data_write(d, (op >> 4) & 0x3fff, uint16_t(rd[(op >> 18) & 3](d, r4(0))));
            break;
        }
        if ((op >> 16) >= 0xa0 && (op >> 16) <= 0xaf) { data_write_dag1(d, op, int32_t((op >> 4) & 0xffff)); break; }
        if ((op >> 16) >= 0xb0 && (op >> 16) <= 0xbf) { data_write_dag2(d, op, int32_t((op >> 4) & 0xffff)); break; }
        if ((op >> 16) >= 0xc0) { // ALU/MAC with dual read: DM (DAG1) -> x reg, PM (DAG2) -> y reg
            uint32_t g = op >> 16;
            if (g & 2) alu_op_ar(d, op); else mac_op_mr(d, op);
            Core& c = d->core;
            Reg16* xd[4] = {&c.ax0, &c.ax1, &c.mx0, &c.mx1};
            Reg16* yd[4] = {&c.ay0, &c.ay1, &c.my0, &c.my1};
            xd[(g >> 2) & 3]->u = uint16_t(data_read_dag1(d, op));
            yd[(g >> 4) & 3]->u = uint16_t(pgm_read_dag2(d, op >> 4));
            break;
        }
        break;
    }
}

} // namespace adsp
