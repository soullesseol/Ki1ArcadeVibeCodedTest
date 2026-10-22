#include "recomp.h"
#include <chrono>
#include <cstdio>
#include <unordered_map>
#if !defined(__SIZEOF_INT128__)
#include <intrin.h>
#endif

namespace ki {

static std::unordered_map<uint32_t, RecompFn>& funcs() {
    static std::unordered_map<uint32_t, RecompFn> m;
    return m;
}

void register_funcs(const FuncEntry* f, size_t n) {
    for (size_t i = 0; i < n; i++) funcs()[to_phys(f[i].vaddr)] = f[i].fn;
}

void jump(Ctx* c, uint32_t vaddr) {
    auto it = funcs().find(to_phys(vaddr));
    if (it == funcs().end()) throw Unwind{vaddr};
    it->second(c);
}

void run(Ctx* c, uint32_t entry) {
    for (uint32_t pc = entry;;) {
        try {
            jump(c, pc);
            pc = uint32_t(c->r[31]);
        } catch (const Unwind& u) {
            // Nobody on the native stack returns there: it's code we haven't recompiled.
            if (!funcs().count(to_phys(u.target))) throw MissingFunc{u.target};
            pc = u.target;
        }
    }
}

// ponytail: wall-clock Count, like Bus::in_vblank(); move both to a guest-cycle clock together.
static int64_t ticks() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count() / 20;
}
uint32_t count(Ctx* c) { return uint32_t(ticks() - c->count_bias); }
void set_count(Ctx* c, uint32_t v) { c->count_bias = ticks() - v; c->last_count = v; }
void set_compare(Ctx* c, uint32_t v) {
    c->cop0[11] = v;
    c->timer_pending = false;
    c->last_count = count(c);
}

enum : uint32_t { SR_IE = 1, SR_EXL = 2, SR_ERL = 4 };

void poll(Ctx* c) {
    c->poll_flag = 0;
    // Timer fires when Count passes Compare, i.e. Compare in (last_count, now].
    uint32_t now = count(c), cmp = uint32_t(c->cop0[11]);
    if (uint32_t(cmp - c->last_count - 1) < uint32_t(now - c->last_count)) c->timer_pending = true;
    c->last_count = now;

    uint32_t sr = uint32_t(c->cop0[12]);
    if (!(sr & SR_IE) || (sr & (SR_EXL | SR_ERL)) || !(sr & uint32_t(cause(c)) & 0xff00)) return;
    // Take the interrupt: we're between guest instructions, so the handler's own register
    // save/restore makes it transparent. eret returns here natively instead of via EPC.
    c->cop0[14] = c->func;                       // EPC: diagnostic only
    c->cop0[13] &= ~0x7cull;                     // ExcCode = 0 (interrupt)
    c->cop0[12] |= SR_EXL;
    jump(c, 0x80000180);
}

// ponytail: syscall/break only log; the game uses syscall only on fatal boot errors.
void syscall(Ctx* c, uint32_t pc) { std::fprintf(stderr, "syscall at %08x (v0=%llx)\n", pc, (unsigned long long)c->r[2]); }
void eret(Ctx* c) { c->cop0[12] &= (c->cop0[12] & SR_ERL) ? ~uint64_t(SR_ERL) : ~uint64_t(SR_EXL); }

void alias(uint32_t vaddr, uint32_t target) { funcs()[to_phys(vaddr)] = funcs().at(to_phys(target)); }
void unimplemented(Ctx*, uint32_t pc, uint32_t word) { throw Unimplemented{pc, word}; }

#if defined(__SIZEOF_INT128__)
void dmult(Ctx* c, int64_t a, int64_t b) { __int128 p = (__int128)a * b; c->lo = uint64_t(p); c->hi = uint64_t(p >> 64); }
void dmultu(Ctx* c, uint64_t a, uint64_t b) { unsigned __int128 p = (unsigned __int128)a * b; c->lo = uint64_t(p); c->hi = uint64_t(p >> 64); }
#else
void dmult(Ctx* c, int64_t a, int64_t b) { int64_t h; c->lo = uint64_t(_mul128(a, b, &h)); c->hi = uint64_t(h); }
void dmultu(Ctx* c, uint64_t a, uint64_t b) { c->lo = _umul128(a, b, &c->hi); }
#endif


} // namespace ki
