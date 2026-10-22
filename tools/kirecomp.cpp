// kirecomp: static recompiler for little-endian MIPS III (R4600) code -> C++.
// No symbols needed: functions are discovered by tracing control flow from the given entry
// points plus every jal target. Translation follows N64Recomp's model: one C++ function per
// MIPS function, branches become gotos, jal becomes a call, delay slots are emitted inline.
//
// usage: kirecomp <bin> <file_offset> <base_vaddr> <end_vaddr> <out.cpp> <table_name> [--patches FILE] <entry>...
//   bytes at <file_offset> are loaded at <base_vaddr>; code is only traced inside [base, end).
//   --patches: widescreen alternatives (see tools/widescreen.patches), one per line:
//     <pc> word <hex>    run this instruction word instead when c->bus->widescreen is set
//     <pc> code <C++>    run this C++ statement instead when c->bus->widescreen is set
//     <pc> after <C++>   run the instruction, then this C++, when c->bus->widescreen is set
//   Only non-control-flow instructions (including delay slots) can be patched.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

using std::format;
using std::string;

struct Insn {
    uint32_t w;
    uint32_t op() const { return w >> 26; }
    uint32_t rs() const { return (w >> 21) & 31; }
    uint32_t rt() const { return (w >> 16) & 31; }
    uint32_t rd() const { return (w >> 11) & 31; }
    uint32_t sa() const { return (w >> 6) & 31; }
    uint32_t fn() const { return w & 63; }
    int32_t simm() const { return int16_t(w & 0xffff); }
    uint32_t uimm() const { return w & 0xffff; }
};

enum class Flow { Normal, Branch, BranchLikely, BranchLink, BranchLinkLikely, Goto, Jal, JrRa, Jr, Jalr, Eret, Bad };

static std::vector<uint8_t> bin;
static uint32_t base, end_;
static std::set<uint32_t> entries;                 // every vaddr that gets its own function
static std::map<uint32_t, std::vector<uint32_t>> unimpl;  // pc -> word (reported once)
struct Patch { uint32_t word = 0; string code; bool after = false, used = false; };
static std::map<uint32_t, Patch> patches;

static bool in_code(uint32_t a) { return a >= base && a < end_ && (a & 3) == 0; }
static Insn at(uint32_t a) {
    uint32_t off = a - base, w;
    std::memcpy(&w, &bin[off], 4);
    return {w};
}
static uint32_t btarget(uint32_t pc, Insn i) { return pc + 4 + uint32_t(i.simm() << 2); }
static uint32_t jtarget(uint32_t pc, Insn i) { return ((pc + 4) & 0xf0000000) | ((i.w & 0x3ffffff) << 2); }
static string R(uint32_t n) { return format("c->r[{}]", n); }
static string fname(uint32_t a) { return format("f_{:08x}", a & 0x1fffffff); }

// Classify control flow. For branches, fills cond (C++ bool expression) and target.
static Flow flow(uint32_t pc, Insn i, string* cond = nullptr, uint32_t* tgt = nullptr) {
    auto br = [&](string c, bool likely) {
        if (cond) *cond = c;
        if (tgt) *tgt = btarget(pc, i);
        return likely ? Flow::BranchLikely : Flow::Branch;
    };
    string s = R(i.rs()), t = R(i.rt()), ss = format("int64_t({})", s);
    switch (i.op()) {
    case 0x00:
        if (i.fn() == 8) return i.rs() == 31 ? Flow::JrRa : Flow::Jr;
        if (i.fn() == 9) return Flow::Jalr;
        return Flow::Normal;
    case 0x01:
        switch (i.rt()) {
        case 0: return br(ss + " < 0", false);
        case 1: if (i.rs() == 0) { if (tgt) *tgt = btarget(pc, i); return Flow::Goto; }
                return br(ss + " >= 0", false);
        case 2: return br(ss + " < 0", true);
        case 3: return br(ss + " >= 0", true);
        }
        if (i.rt() >= 16 && i.rt() <= 19) { // bltzal, bgezal, bltzall, bgezall: conditional calls
            if (cond) *cond = ss + ((i.rt() & 1) ? " >= 0" : " < 0");
            if (tgt) *tgt = btarget(pc, i);
            return (i.rt() & 2) ? Flow::BranchLinkLikely : Flow::BranchLink;
        }
        return Flow::Normal; // traps fall through to emit() and get reported there
    case 0x02: if (tgt) *tgt = jtarget(pc, i); return Flow::Goto;
    case 0x03: if (tgt) *tgt = jtarget(pc, i); return Flow::Jal;
    case 0x04: if (i.rs() == i.rt()) { if (tgt) *tgt = btarget(pc, i); return Flow::Goto; }
               return br(s + " == " + t, false);
    case 0x05: return br(s + " != " + t, false);
    case 0x06: return br(ss + " <= 0", false);
    case 0x07: return br(ss + " > 0", false);
    case 0x14: return br(s + " == " + t, true);
    case 0x15: return br(s + " != " + t, true);
    case 0x16: return br(ss + " <= 0", true);
    case 0x17: return br(ss + " > 0", true);
    case 0x10: return (i.rs() == 16 && i.fn() == 0x18) ? Flow::Eret : Flow::Normal;
    case 0x11:
        if (i.rs() == 8) return br(string((i.rt() & 1) ? "" : "!") + "((c->fcr[31] >> 23) & 1)", (i.rt() & 2) != 0);
        return Flow::Normal;
    }
    return Flow::Normal;
}

static string emit_insn(uint32_t pc, Insn i);

// C++ for a non-control-flow instruction, with its widescreen alternative if it has one.
static string emit(uint32_t pc, Insn i) {
    auto p = patches.find(pc);
    if (p == patches.end()) return emit_insn(pc, i);
    p->second.used = true;
    string alt = p->second.after ? emit_insn(pc, i) + " " + p->second.code
               : p->second.code.empty() ? emit_insn(pc, {p->second.word}) : p->second.code;
    return format("if (c->bus->widescreen) {{ {} }} else {{ {} }}", alt, emit_insn(pc, i));
}

static string emit_insn(uint32_t pc, Insn i) {
    uint32_t rs = i.rs(), rt = i.rt(), rd = i.rd(), sa = i.sa();
    string s = R(rs), t = R(rt);
    string ea = format("{} + {}", s, i.simm());
    auto set = [](uint32_t r, string e) { return r ? format("{} = {};", R(r), e) : string(); };
    auto load = [](uint32_t r, string e) { return r ? format("{} = {};", R(r), e) : format("(void){};", e); };
    auto bad = [&] { unimpl[pc].push_back(i.w); return format("unimplemented(c, 0x{:08x}, 0x{:08x});", pc, i.w); };

    switch (i.op()) {
    case 0x00:
        switch (i.fn()) {
        case 0x00: return set(rd, format("S32(uint32_t({}) << {})", t, sa));
        case 0x02: return set(rd, format("S32(uint32_t({}) >> {})", t, sa));
        case 0x03: return set(rd, format("S32(int32_t({}) >> {})", t, sa));
        case 0x04: return set(rd, format("S32(uint32_t({}) << ({} & 31))", t, s));
        case 0x06: return set(rd, format("S32(uint32_t({}) >> ({} & 31))", t, s));
        case 0x07: return set(rd, format("S32(int32_t({}) >> ({} & 31))", t, s));
        case 0x0c: return format("syscall(c, 0x{:08x});", pc);
        case 0x0d: return format("syscall(c, 0x{:08x}); /* break */", pc);
        case 0x0f: return "/* sync */";
        case 0x10: return set(rd, "c->hi");
        case 0x11: return format("c->hi = {};", s);
        case 0x12: return set(rd, "c->lo");
        case 0x13: return format("c->lo = {};", s);
        case 0x14: return set(rd, format("{} << ({} & 63)", t, s));
        case 0x16: return set(rd, format("{} >> ({} & 63)", t, s));
        case 0x17: return set(rd, format("uint64_t(int64_t({}) >> ({} & 63))", t, s));
        case 0x18: return format("{{ int64_t p = int64_t(int32_t({})) * int32_t({}); c->lo = S32(uint64_t(p)); c->hi = S32(uint64_t(p) >> 32); }}", s, t);
        case 0x19: return format("{{ uint64_t p = uint64_t(uint32_t({})) * uint32_t({}); c->lo = S32(p); c->hi = S32(p >> 32); }}", s, t);
        case 0x1a: return format("div32(c, int32_t({}), int32_t({}));", s, t);
        case 0x1b: return format("divu32(c, uint32_t({}), uint32_t({}));", s, t);
        case 0x1c: return format("dmult(c, int64_t({}), int64_t({}));", s, t);
        case 0x1d: return format("dmultu(c, {}, {});", s, t);
        case 0x1e: return format("div64(c, int64_t({}), int64_t({}));", s, t);
        case 0x1f: return format("divu64(c, {}, {});", s, t);
        // ponytail: add/addi/dadd/sub overflow traps not modeled, same as addu.
        case 0x20: case 0x21: return set(rd, format("S32(uint32_t({}) + uint32_t({}))", s, t));
        case 0x22: case 0x23: return set(rd, format("S32(uint32_t({}) - uint32_t({}))", s, t));
        case 0x24: return set(rd, format("{} & {}", s, t));
        case 0x25: return set(rd, format("{} | {}", s, t));
        case 0x26: return set(rd, format("{} ^ {}", s, t));
        case 0x27: return set(rd, format("~({} | {})", s, t));
        case 0x2a: return set(rd, format("uint64_t(int64_t({}) < int64_t({}))", s, t));
        case 0x2b: return set(rd, format("uint64_t({} < {})", s, t));
        case 0x2c: case 0x2d: return set(rd, format("{} + {}", s, t));
        case 0x2e: case 0x2f: return set(rd, format("{} - {}", s, t));
        case 0x38: return set(rd, format("{} << {}", t, sa));
        case 0x3a: return set(rd, format("{} >> {}", t, sa));
        case 0x3b: return set(rd, format("uint64_t(int64_t({}) >> {})", t, sa));
        case 0x3c: return set(rd, format("{} << {}", t, sa + 32));
        case 0x3e: return set(rd, format("{} >> {}", t, sa + 32));
        case 0x3f: return set(rd, format("uint64_t(int64_t({}) >> {})", t, sa + 32));
        }
        return bad();
    case 0x08: case 0x09: return set(rt, format("S32(uint32_t({}) + {}u)", s, uint32_t(i.simm())));
    case 0x0a: return set(rt, format("uint64_t(int64_t({}) < {})", s, i.simm()));
    case 0x0b: return set(rt, format("uint64_t({} < 0x{:x}ull)", s, uint64_t(int64_t(i.simm()))));
    case 0x0c: return set(rt, format("{} & 0x{:x}", s, i.uimm()));
    case 0x0d: return set(rt, format("{} | 0x{:x}", s, i.uimm()));
    case 0x0e: return set(rt, format("{} ^ 0x{:x}", s, i.uimm()));
    case 0x0f: return set(rt, format("0x{:x}ull", uint64_t(int64_t(int32_t(i.uimm() << 16)))));
    case 0x18: case 0x19: return set(rt, format("{} + {}", s, int64_t(i.simm())));
    case 0x1a: return set(rt, format("ldl<8>(c, {}, {})", t, ea));
    case 0x1b: return set(rt, format("ldr<8>(c, {}, {})", t, ea));
    case 0x20: return load(rt, format("uint64_t(int64_t(RD<int8_t>(c, {})))", ea));
    case 0x21: return load(rt, format("uint64_t(int64_t(RD<int16_t>(c, {})))", ea));
    case 0x22: return set(rt, format("ldl<4>(c, {}, {})", t, ea));
    case 0x23: case 0x30: return load(rt, format("S32(RD<uint32_t>(c, {}))", ea)); // lw, ll
    case 0x24: return load(rt, format("uint64_t(RD<uint8_t>(c, {}))", ea));
    case 0x25: return load(rt, format("uint64_t(RD<uint16_t>(c, {}))", ea));
    case 0x26: return set(rt, format("ldr<4>(c, {}, {})", t, ea));
    case 0x27: return load(rt, format("uint64_t(RD<uint32_t>(c, {}))", ea));
    case 0x28: return format("WR<uint8_t>(c, {}, uint8_t({}));", ea, t);
    case 0x29: return format("WR<uint16_t>(c, {}, uint16_t({}));", ea, t);
    case 0x2a: return format("sdl<4>(c, {}, {});", t, ea);
    case 0x2b: return format("WR<uint32_t>(c, {}, uint32_t({}));", ea, t);
    case 0x2c: return format("sdl<8>(c, {}, {});", t, ea);
    case 0x2d: return format("sdr<8>(c, {}, {});", t, ea);
    case 0x2e: return format("sdr<4>(c, {}, {});", t, ea);
    case 0x2f: return "/* cache */";
    case 0x37: return load(rt, format("RD<uint64_t>(c, {})", ea));
    case 0x38: return format("WR<uint32_t>(c, {}, uint32_t({})); {}", ea, t, set(rt, "1")); // sc always succeeds
    case 0x3f: return format("WR<uint64_t>(c, {}, {});", ea, t);
    case 0x10: // COP0
        switch (rs) {
        case 0: case 1: { // mfc0/dmfc0: Cause and Count are live hardware state
            string v = rd == 13 ? "cause(c)" : rd == 9 ? "uint64_t(count(c))" : format("c->cop0[{}]", rd);
            return set(rt, rs == 0 ? format("S32({})", v) : v);
        }
        case 4: case 5: // mtc0/dmtc0
            if (rd == 9) return format("set_count(c, uint32_t({}));", t);
            if (rd == 11) return format("set_compare(c, uint32_t({}));", t);
            return format("c->cop0[{}] = {};", rd, rs == 4 ? format("S32({})", t) : t);
        case 16:
            if (i.fn() == 2) // tlbwi: Index, EntryHi, EntryLo0/1, PageMask
                return "c->bus->tlb_write(uint32_t(c->cop0[0]) & 63, uint32_t(c->cop0[10]), uint32_t(c->cop0[2]), uint32_t(c->cop0[3]), uint32_t(c->cop0[5]));";
            // ponytail: tlbr/tlbwr/tlbp are no-ops (the game only uses tlbwi).
            if (i.fn() == 1 || i.fn() == 6 || i.fn() == 8) return "/* tlb op */";
        }
        return bad();
    case 0x11: // COP1 moves only; FPU arithmetic gets added when recompiled code needs it
        switch (rs) {
        case 0: return set(rt, format("S32(c->f[{}])", rd));
        case 1: return set(rt, format("c->f[{}]", rd));
        case 2: return set(rt, format("S32(c->fcr[{}])", rd));
        case 4: return format("c->f[{0}] = (c->f[{0}] & ~0xffffffffull) | uint32_t({1});", rd, t);
        case 5: return format("c->f[{}] = {};", rd, t);
        case 6: return format("c->fcr[{}] = uint32_t({});", rd, t);
        }
        if (rs == 16 || rs == 17 || rs == 20 || rs == 21) {
            const char* T = rs == 16 ? "float" : rs == 17 ? "double" : rs == 20 ? "int32_t" : "int64_t";
            uint32_t fs = rd, ft = rt, fd = sa, fn = i.fn();
            string a = format("fr<{}>(c, {})", T, fs), b = format("fr<{}>(c, {})", T, ft);
            auto put = [&](const char* U, string e) { return format("fw<{}>(c, {}, {});", U, fd, e); };
            bool flt = rs <= 17;
            if (flt && fn <= 7) {
                switch (fn) {
                case 0: return put(T, a + " + " + b);
                case 1: return put(T, a + " - " + b);
                case 2: return put(T, a + " * " + b);
                case 3: return put(T, a + " / " + b);
                case 4: return put(T, format("std::sqrt({})", a));
                case 5: return put(T, format("std::fabs({})", a));
                case 6: return format("c->f[{}] = c->f[{}];", fd, fs);
                case 7: return put(T, "-" + a);
                }
            }
            if (flt && fn >= 0x08 && fn <= 0x0f) { // round/trunc/ceil/floor .l (8-b) .w (c-f)
                const char* U = fn < 0x0c ? "int64_t" : "int32_t";
                const char* how[] = {"std::nearbyint", "std::trunc", "std::ceil", "std::floor"};
                return put(U, format("{}({}({}))", U, how[fn & 3], a));
            }
            if (fn == 0x20) return put("float", format("float({})", a));
            if (fn == 0x21) return put("double", format("double({})", a));
            if (flt && fn == 0x24) return put("int32_t", format("fround<int32_t>({})", a));
            if (flt && fn == 0x25) return put("int64_t", format("fround<int64_t>({})", a));
            if (flt && fn >= 0x30) // c.cond: bit0 unordered, bit1 equal, bit2 less
                return format("{{ auto a = {}; auto b = {}; fcond(c, {}{}{}false); }}", a, b,
                              (fn & 1) ? "std::isnan(a) || std::isnan(b) || " : "",
                              (fn & 2) ? "a == b || " : "", (fn & 4) ? "a < b || " : "");
        }
        return bad();
    case 0x31: return format("c->f[{0}] = (c->f[{0}] & ~0xffffffffull) | RD<uint32_t>(c, {1});", rt, ea);
    case 0x35: return format("c->f[{}] = RD<uint64_t>(c, {});", rt, ea);
    case 0x39: return format("WR<uint32_t>(c, {}, uint32_t(c->f[{}]));", ea, rt);
    case 0x3d: return format("WR<uint64_t>(c, {}, c->f[{}]);", ea, rt);
    }
    return bad();
}

struct Func {
    std::map<uint32_t, string> body;  // head pc -> code
    std::set<uint32_t> labels;
    bool uses_ra0 = false;  // function has a jr via a non-ra register
};

// Code for a call/jump to target from inside function `self`.
static string call(uint32_t target) {
    return entries.count(target) ? format("{}(c);", fname(target)) : format("jump(c, 0x{:08x});", target);
}

static Func build(uint32_t entry, std::set<uint32_t>& new_entries) {
    Func f;
    std::vector<uint32_t> work{entry};
    std::map<uint32_t, uint32_t> next; // head pc -> fallthrough pc (if any)
    auto go = [&](uint32_t t) -> string {   // goto inside function, or tail call out of it
        if (in_code(t)) { work.push_back(t); f.labels.insert(t); return format("goto L_{:08x};", t); }
        return format("{} return;", call(t));
    };
    while (!work.empty()) {
        uint32_t pc = work.back(); work.pop_back();
        if (f.body.count(pc)) continue;
        if (!in_code(pc)) { f.body[pc] = format("{} return;", call(pc)); continue; }
        Insn i = at(pc);
        string cond; uint32_t tgt = 0;
        Flow fl = flow(pc, i, &cond, &tgt);
        Insn d = in_code(pc + 4) ? at(pc + 4) : Insn{0};
        string ds;
        string dcond; uint32_t dtgt = 0;
        Flow dfl = flow(pc + 4, d, &dcond, &dtgt);
        if (fl != Flow::Normal && fl != Flow::Bad && dfl == Flow::Normal) ds = emit(pc + 4, d);
        string code;
        switch (fl) {
        case Flow::Normal:
            code = emit(pc, i); next[pc] = pc + 4; break;
        case Flow::Bad:
            code = format("unimplemented(c, 0x{:08x}, 0x{:08x});", pc, i.w); unimpl[pc].push_back(i.w); break;
        case Flow::Branch:
            if (dfl == Flow::Branch) {
                // Branch in a delay slot (the stub does this at 0xbfc00e34). If the first branch is
                // taken the second one only evaluates; otherwise it runs as a normal branch.
                code = format("if ({}) {{ if ({}) unimplemented(c, 0x{:08x}, 0x{:08x}); {} }}", cond, dcond, pc + 4, d.w, go(tgt));
                next[pc] = pc + 4;
            } else if (dfl != Flow::Normal) {
                code = format("unimplemented(c, 0x{:08x}, 0x{:08x});", pc + 4, d.w); unimpl[pc + 4].push_back(d.w);
            } else {
                code = format("{{ bool t = {}; {} if (t) {} }}", cond, ds, go(tgt)); next[pc] = pc + 8;
            }
            break;
        case Flow::BranchLikely:
            code = format("if ({}) {{ {} {} }}", cond, ds, go(tgt)); next[pc] = pc + 8; break;
        case Flow::Goto:
            // A j to a different function's entry is a tail call; anything else stays in this function.
            if (entries.count(tgt) && tgt != entry) code = format("{} {} return;", ds, call(tgt));
            else code = format("{} {}", ds, go(tgt));
            break;
        case Flow::BranchLink: case Flow::BranchLinkLikely: {
            // ra is written whether or not the branch is taken; condition is read before that.
            if (in_code(tgt) && !entries.count(tgt)) new_entries.insert(tgt);
            string callee = in_code(tgt) ? format("call(c, {}, 0x{:08x});", fname(tgt), pc + 8)
                                         : format("callv(c, 0x{:08x}, 0x{:08x});", tgt, pc + 8);
            string ra = format("{} = 0x{:x}ull;", R(31), uint64_t(int64_t(int32_t(pc + 8))));
            code = fl == Flow::BranchLink ? format("{{ bool t = {}; {} {} if (t) {} }}", cond, ra, ds, callee)
                                          : format("{{ bool t = {}; {} if (t) {{ {} {} }} }}", cond, ra, ds, callee);
            next[pc] = pc + 8; break;
        }
        case Flow::Jal:
            if (in_code(tgt) && !entries.count(tgt)) new_entries.insert(tgt);
            code = format("{} = 0x{:x}ull; {} {}", R(31), uint64_t(int64_t(int32_t(pc + 8))), ds,
                          in_code(tgt) ? format("call(c, {}, 0x{:08x});", fname(tgt), pc + 8)
                                       : format("callv(c, 0x{:08x}, 0x{:08x});", tgt, pc + 8));
            next[pc] = pc + 8; break;
        case Flow::JrRa:
            code = format("{} return;", ds); break;
        case Flow::Jr:
            // Hand-written code often saves ra elsewhere and returns with e.g. "jr $a0". A jr to the
            // address this function was called from is a return; anything else is a tail jump.
            // Computed jumps can also land anywhere inside this function (Duff's-device style
            // unrolled loops in the renderer, e.g. 0x88010b74): @JRSWITCH@ becomes a switch over
            // every instruction of the function once the body is complete.
            code = format("{{ uint32_t t = uint32_t({}); {} if (to_phys(t) == to_phys(ra0)) return; @JRSWITCH@ jump(c, t); return; }}",
                          R(i.rs()), ds);
            f.uses_ra0 = true;
            // Unrolled blocks entered only via this jr usually follow it directly (0x880304a8).
            if (in_code(pc + 8)) work.push_back(pc + 8);
            break;
        case Flow::Jalr:
            code = format("{{ uint32_t t = uint32_t({}); {} {} callv(c, t, 0x{:08x}); }}", R(i.rs()),
                          i.rd() ? format("{} = 0x{:x}ull;", R(i.rd()), uint64_t(int64_t(int32_t(pc + 8)))) : "", ds, pc + 8);
            next[pc] = pc + 8; break;
        case Flow::Eret:
            code = "eret(c); return;"; break;
        }
        bool has_ds = fl != Flow::Normal && fl != Flow::Bad && fl != Flow::Eret;
        if (has_ds && dfl != Flow::Normal && !(fl == Flow::Branch && dfl == Flow::Branch)) {
            code = format("unimplemented(c, 0x{:08x}, 0x{:08x});", pc + 4, d.w); // jump in a jump's delay slot
            unimpl[pc + 4].push_back(d.w);
            next.erase(pc);
        }
        // Interrupt safe point on loop back-edges, so wait loops see the timer interrupt.
        bool back = (fl == Flow::Branch || fl == Flow::BranchLikely || fl == Flow::Goto) && tgt <= pc;
        f.body[pc] = format("/* {:08x}: {:08x} */ {}{}", pc, i.w, back ? "POLL(c); " : "", code);
        if (next.count(pc)) work.push_back(next[pc]);
    }
    // Make fallthrough explicit wherever the next emitted head isn't the sequential successor.
    for (auto& [pc, n] : next) {
        auto it = f.body.upper_bound(pc);
        if (it == f.body.end() || it->first != n) { f.body[pc] += format(" goto L_{:08x};", n); f.labels.insert(n); }
    }
    return f;
}

int main(int argc, char** argv) {
    if (argc < 8) {
        std::fprintf(stderr, "usage: kirecomp <bin> <file_offset> <base_vaddr> <end_vaddr> <out.cpp> <table_name> <entry>...\n");
        return 1;
    }
    uint32_t off = std::stoul(argv[2], nullptr, 16);
    base = std::stoul(argv[3], nullptr, 16);
    end_ = std::stoul(argv[4], nullptr, 16);
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<uint8_t> all((std::istreambuf_iterator<char>(in)), {});
    if (all.size() < off + (end_ - base)) { std::fprintf(stderr, "%s too small\n", argv[1]); return 1; }
    bin.assign(all.begin() + off, all.end());

    std::vector<uint32_t> todo;
    for (int a = 7; a < argc; a++) {
        if (string(argv[a]) == "--patches" && a + 1 < argc) {
            std::ifstream pf(argv[++a]);
            if (!pf) { std::fprintf(stderr, "can't read %s\n", argv[a]); return 1; }
            for (string line; std::getline(pf, line);) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                char kind[8] = {};
                uint32_t pc = 0;
                int n = 0;
                if (line.empty() || line[0] == '#' || std::sscanf(line.c_str(), " %x %7s %n", &pc, kind, &n) < 2) continue;
                Patch& pt = patches[pc];
                if (string(kind) == "word") pt.word = std::stoul(line.substr(n), nullptr, 16); // trailing "# ..." ignored
                else if (string(kind) == "code" || string(kind) == "after") pt.code = line.substr(n), pt.after = kind[0] == 'a';
                else { std::fprintf(stderr, "%s: bad patch line: %s\n", argv[a], line.c_str()); return 1; }
                if (!in_code(pc) || flow(pc, at(pc)) != Flow::Normal) { std::fprintf(stderr, "patch %08x: not a plain instruction\n", pc); return 1; }
            }
            continue;
        }
        todo.push_back(std::stoul(argv[a], nullptr, 16));
    }
    // Pointer harvesting: any aligned word in the image that points into the code region may be
    // a jump/dispatch table entry (e.g. the renderer table at 0x880355b0), so make it an entry.
    // ponytail: false positives just cost code size; add a real jump-table analysis if that bites.
    size_t harvested = 0;
    for (size_t o = 0; o + 4 <= bin.size(); o += 4) {
        uint32_t w;
        std::memcpy(&w, &bin[o], 4);
        if (in_code(w)) { todo.push_back(w); harvested++; }
    }
    // Also code addresses built in registers ("lui rX, hi" then "addiu/ori rY, rX, lo") for jalr,
    // e.g. the renderer routine at 0x8801b948. Tracks each register's lui for 16 instructions.
    uint32_t hi[32] = {}, hi_at[32] = {};
    for (uint32_t pc = base; pc < end_; pc += 4) {
        Insn i = at(pc);
        if (i.op() == 0x0f) { hi[i.rt()] = i.uimm() << 16; hi_at[i.rt()] = pc; continue; }
        if ((i.op() == 0x09 || i.op() == 0x0d) && hi_at[i.rs()] && pc - hi_at[i.rs()] <= 64) {
            uint32_t v = i.op() == 0x09 ? hi[i.rs()] + uint32_t(i.simm()) : hi[i.rs()] | i.uimm();
            if (in_code(v)) { todo.push_back(v); harvested++; }
        }
    }
    entries.insert(todo.begin(), todo.end());
    std::map<uint32_t, Func> funcs;
    // Discover all entries first, so the final build sees the complete set for tail calls.
    while (!todo.empty()) {
        uint32_t e = todo.back(); todo.pop_back();
        std::set<uint32_t> found;
        build(e, found);
        for (uint32_t n : found) if (entries.insert(n).second) todo.push_back(n);
    }
    unimpl.clear();
    for (uint32_t e : entries) { std::set<uint32_t> unused; funcs[e] = build(e, unused); }

    string out = "// Generated by kirecomp. Do not edit.\n#include \"recomp.h\"\nusing namespace ki;\n\n";
    for (auto& [e, f] : funcs) out += format("static void {}(Ctx* c);\n", fname(e));
    for (auto& [e, f] : funcs) {
        out += format("\nstatic void {0}(Ctx* c) {{\n    c->func = 0x{1:08x}; c->trail[c->trail_pos++ & 63] = 0x{1:08x}; POLL(c);\n", fname(e), e);
        if (f.uses_ra0) out += "    const uint32_t ra0 = uint32_t(c->r[31]);\n";
        // Bodies are emitted in address order; the entry isn't necessarily the lowest address.
        if (f.body.begin()->first != e) { out += format("    goto L_{:08x};\n", e); f.labels.insert(e); }
        string sw;
        for (auto& [pc, code] : f.body)
            if (in_code(pc) && code.find("@JRSWITCH@") != string::npos) sw = "switch (to_phys(t)) {";
        if (!sw.empty()) {
            for (auto& [pc, code] : f.body)
                if (in_code(pc)) { sw += format(" case 0x{:08x}: goto L_{:08x};", pc & 0x1fffffff, pc); f.labels.insert(pc); }
            sw += " }";
        }
        for (auto& [pc, code] : f.body) {
            if (f.labels.count(pc)) out += format("L_{:08x}:;\n", pc);
            string line = code;
            if (size_t p = line.find("@JRSWITCH@"); p != string::npos) line.replace(p, 10, sw);
            out += "    " + line + "\n";
        }
        out += "}\n";
    }
    out += format("\nextern const FuncEntry {}_funcs[] = {{\n", argv[6]);
    for (auto& [e, f] : funcs) out += format("    {{0x{:08x}, {}}},\n", e, fname(e));
    out += format("}};\nextern const size_t {0}_count = sizeof({0}_funcs) / sizeof({0}_funcs[0]);\n", argv[6]);
    std::ofstream(argv[5], std::ios::binary) << out;

    std::printf("kirecomp: %zu functions (%zu harvested pointers), %zu patches -> %s\n", funcs.size(), harvested, patches.size(), argv[5]);
    for (auto& [pc, pt] : patches)
        if (!pt.used) { std::fprintf(stderr, "patch %08x: address is not in any traced code\n", pc); return 1; }
    for (auto& [pc, w] : unimpl) std::printf("  unimplemented %08x: %08x\n", pc, w[0]);
    return 0;
}
