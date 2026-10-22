#pragma once
// Runtime support for code emitted by tools/kirecomp.cpp.
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include "memmap.h"

namespace ki {

struct Ctx {
    uint64_t r[32]{};
    uint64_t hi = 0, lo = 0;
    uint64_t f[32]{};     // FR=1 (boot sets Status 0x34400000), so 32 x 64-bit FPRs
    uint32_t fcr[32]{};   // fcr[31] = FCSR, bit 23 = condition
    uint64_t cop0[32]{};
    int64_t count_bias = 0; // COP0 Count = host ticks - bias
    uint32_t last_count = 0;    // Count at the previous poll (for Compare crossing)
    bool timer_pending = false; // Cause.IP7, cleared by writing Compare
    volatile uint8_t poll_flag = 0; // set ~1kHz by a host thread; recompiled code then calls poll()
    Bus* bus = nullptr;
    volatile uint32_t func = 0; // vaddr of the most recently entered function (for diagnostics)
    uint32_t trail[64]{};       // ring of recent function entries
    uint32_t trail_pos = 0;
};

using RecompFn = void (*)(Ctx*);
struct FuncEntry { uint32_t vaddr; RecompFn fn; };

// Thrown when control reaches an address with no recompiled function, e.g. code that was
// decompressed or loaded into RAM at runtime. Catch it to dump RAM and recompile that.
struct MissingFunc { uint32_t vaddr; };
struct Unimplemented { uint32_t pc, word; };

// Non-local return: hand-written code can "jr" straight to an ancestor's return address
// (e.g. the IDE timeout path at 0x8802d498). Thrown by jump() for addresses with no function,
// caught by the call site whose return address matches.
struct Unwind { uint32_t target; };

void register_funcs(const FuncEntry* funcs, size_t n);
void alias(uint32_t vaddr, uint32_t target); // position-independent code copied elsewhere at runtime
void jump(Ctx* c, uint32_t vaddr);      // call the recompiled function at vaddr (any kseg alias)
inline void call(Ctx* c, RecompFn f, uint32_t ret) {
    try { f(c); } catch (const Unwind& u) { if (to_phys(u.target) != to_phys(ret)) throw; }
}
inline void callv(Ctx* c, uint32_t vaddr, uint32_t ret) {
    try { jump(c, vaddr); } catch (const Unwind& u) { if (to_phys(u.target) != to_phys(ret)) throw; }
}
void run(Ctx* c, uint32_t entry);       // top level: a top-level "jr ra" continues at ra
void syscall(Ctx* c, uint32_t pc);
void eret(Ctx* c);
[[noreturn]] void unimplemented(Ctx* c, uint32_t pc, uint32_t word);

inline uint64_t S32(uint64_t x) { return uint64_t(int64_t(int32_t(uint32_t(x)))); }

// COP0 Cause with live hardware interrupt lines in IP2..IP6 (the boot polls IP3 for the IDE)
// and the Count/Compare timer in IP7.
inline uint64_t cause(Ctx* c) {
    return (c->cop0[13] & ~0xfc00ull) | (uint64_t(c->bus->irq_lines()) << 10) | (c->timer_pending ? 0x8000 : 0);
}
// COP0 Count runs at half the 100MHz CPU clock (50MHz).
uint32_t count(Ctx* c);
void set_count(Ctx* c, uint32_t v);
void set_compare(Ctx* c, uint32_t v);

// Interrupt safe point, emitted at function entries and backward branches. The game's 60Hz
// Count/Compare interrupt (handler 0x88026388) debounces coins and runs its clocks.
void poll(Ctx* c);
#define POLL(c) do { if ((c)->poll_flag) ::ki::poll(c); } while (0)

// FPR access by format: float (S), double (D), int32_t (W), int64_t (L).
template <class T> T fr(Ctx* c, int n) {
    if constexpr (sizeof(T) == 4) return std::bit_cast<T>(uint32_t(c->f[n]));
    else return std::bit_cast<T>(c->f[n]);
}
template <class T> void fw(Ctx* c, int n, T v) {
    if constexpr (sizeof(T) == 4) c->f[n] = (c->f[n] & ~0xffffffffull) | std::bit_cast<uint32_t>(v);
    else c->f[n] = std::bit_cast<uint64_t>(v);
}
inline void fcond(Ctx* c, bool v) { c->fcr[31] = (c->fcr[31] & ~(1u << 23)) | (uint32_t(v) << 23); }
// ponytail: cvt.w/cvt.l use the host rounding mode (round-to-nearest, what boot sets in FCSR);
// honor FCSR bits 0-1 if the game changes rounding mode.
template <class I, class F> I fround(F x) { return I(std::nearbyint(x)); }

template <class T> T RD(Ctx* c, uint64_t a) { return c->bus->read<T>(uint32_t(a)); }
template <class T> void WR(Ctx* c, uint64_t a, T v) { c->bus->write<T>(uint32_t(a), v); }

// Widescreen patches (tools/widescreen.patches): view-B address of the wide buffer behind the
// view-A framebuffer pointer fb, at row 0 column -WS_M (ws_row0) or column 0 (ws_origin).
inline uint32_t ws_row0(uint32_t fb) { return 0x80000000u | (WS_BASE + (to_phys(fb) >= FB1_OFFSET ? WS_SIZE : 0)); }
inline uint32_t ws_origin(uint32_t fb) { return ws_row0(fb) + WS_M * 2; }

// Entry of a widened rasterizer (after it saved the caller's registers): x coordinates in the
// given registers move WS_M right so its unsigned left clip becomes x = -WS_M, and the target
// (v0) becomes wide row 0. A target other than the two framebuffers keeps its own pitch, which
// the patched routine can't draw with; those calls are counted in ws_foreign_targets.
// Sprite blitter 0x880104e8: args s2 = row pitch (sign = vertical flip), k0..k1 = x clip,
// gp..fp = y clip; it finds the back buffer itself. On frames whose background drew into the
// margins (3D stages), a call clipped to the whole screen is widened: pitch +-1024, x clip
// -WS_M..320+WS_M, origin at the wide buffer's column WS_M (patched inside the blitter).
inline bool ws_wide_sprite(Ctx* c) { return int64_t(c->r[18]) == 0x400 || int64_t(c->r[18]) == -0x400; }
inline void ws_sprite_enter(Ctx* c) {
    if (!c->bus->back_has_margins()) return;
    int64_t s2 = int64_t(c->r[18]);
    if ((s2 != 0x280 && s2 != -0x280) || c->r[26] != 0 || c->r[27] != 0x140) return;
    c->r[18] = s2 > 0 ? 0x400 : S32(uint32_t(-0x400));
    c->r[26] = S32(uint32_t(-WS_M));
    c->r[27] = 0x140 + WS_M;
}

inline uint32_t ws_foreign_targets = 0;
inline void ws_raster_enter(Ctx* c, std::initializer_list<int> xregs) {
    uint32_t fb = uint32_t(c->r[2]), p = to_phys(fb);
    if (p != FB0_OFFSET && p != FB1_OFFSET) { ws_foreign_targets++; return; }
    c->r[2] = S32(ws_row0(fb));
    for (int r : xregs) c->r[r] = S32(uint32_t(c->r[r]) + WS_M);
}

inline void div32(Ctx* c, int32_t a, int32_t b) {
    if (b == 0 || (a == INT32_MIN && b == -1)) return; // result undefined on MIPS, keep hi/lo
    c->lo = S32(uint32_t(a / b)); c->hi = S32(uint32_t(a % b));
}
inline void divu32(Ctx* c, uint32_t a, uint32_t b) {
    if (b == 0) return;
    c->lo = S32(a / b); c->hi = S32(a % b);
}
inline void div64(Ctx* c, int64_t a, int64_t b) {
    if (b == 0 || (a == INT64_MIN && b == -1)) return;
    c->lo = uint64_t(a / b); c->hi = uint64_t(a % b);
}
inline void divu64(Ctx* c, uint64_t a, uint64_t b) {
    if (b == 0) return;
    c->lo = a / b; c->hi = a % b;
}
void dmult(Ctx* c, int64_t a, int64_t b);
void dmultu(Ctx* c, uint64_t a, uint64_t b);

// Unaligned loads/stores, LITTLE-endian semantics. N = 4 (word) or 8 (doubleword).
inline uint64_t lmask(int bytes) { return bytes >= 8 ? ~0ull : (1ull << (bytes * 8)) - 1; }
template <int N> uint64_t rdN(Ctx* c, uint64_t a) {
    if constexpr (N == 4) return RD<uint32_t>(c, a); else return RD<uint64_t>(c, a);
}
template <int N> void wrN(Ctx* c, uint64_t a, uint64_t v) {
    if constexpr (N == 4) WR<uint32_t>(c, a, uint32_t(v)); else WR<uint64_t>(c, a, v);
}
template <int N> uint64_t ldl(Ctx* c, uint64_t old, uint64_t a) { // fills high b+1 bytes
    int b = int(a & (N - 1));
    uint64_t v = (old & lmask(N - 1 - b)) | ((rdN<N>(c, a & ~uint64_t(N - 1)) << (8 * (N - 1 - b))) & lmask(N));
    return N == 4 ? S32(v) : v;
}
template <int N> uint64_t ldr(Ctx* c, uint64_t old, uint64_t a) { // fills low N-b bytes
    int b = int(a & (N - 1));
    uint64_t v = (old & ~lmask(N - b) & lmask(N)) | (rdN<N>(c, a & ~uint64_t(N - 1)) >> (8 * b));
    return N == 4 ? S32(v) : v; // ponytail: LWR always sign-extends (spec says undefined upper bits when b!=0)
}
template <int N> void sdl(Ctx* c, uint64_t v, uint64_t a) {
    int b = int(a & (N - 1));
    uint64_t al = a & ~uint64_t(N - 1), m = rdN<N>(c, al);
    wrN<N>(c, al, (m & ~lmask(b + 1)) | ((v >> (8 * (N - 1 - b))) & lmask(b + 1)));
}
template <int N> void sdr(Ctx* c, uint64_t v, uint64_t a) {
    int b = int(a & (N - 1));
    uint64_t al = a & ~uint64_t(N - 1), m = rdN<N>(c, al);
    wrN<N>(c, al, (m & lmask(b)) | ((v << (8 * b)) & lmask(N)));
}

} // namespace ki
