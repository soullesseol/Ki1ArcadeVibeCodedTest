// adsprecomp: static recompiler for the DCS board's ADSP-2105 program memory -> C++.
// Each program image becomes one state-machine function: every reachable instruction gets a
// label, branches become gotos, and calls/returns/DO loops/interrupts use the real guest
// stacks (the DSP code may read or push TOPPCSTACK itself). Data-path instructions become
// adsp::exec(d, <constant opcode>) calls into helpers ported from MAME, which the compiler
// folds per instruction.
//
// usage: adsprecomp pm   <pm_image.bin> <out.cpp> <name> [entry...]  image: 0x4000 LE u32 words
//        adsprecomp bootN <u10 sound rom> <out.cpp> <name> [entry...] boot page of bank N
//        adsprecomp ovl:BANK:OFF:COUNT:DEST <u10 sound rom> <out.cpp> <name>  OS overlay (hex)
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using std::format;
using std::string;

static std::vector<uint32_t> pm(0x4000);
static std::vector<bool> valid(0x4000);

static void add_succ(std::vector<uint32_t>& work, uint32_t a) { if (a < 0x4000 && valid[a]) work.push_back(a); }

// Data-path instruction, decoded now so the C++ is a direct helper call with constant operands.
// Operation order per group follows adsp21xx_device::execute_run exactly.
static string data_op(uint32_t op) {
    uint32_t hi = op >> 16, c = op & 15;
    auto R = [&](int s) { return (op >> s) & 15; };
    static const char* rd[4] = {"read_reg0", "read_reg1", "read_reg2", "read_reg3"};
    static const char* wr[4] = {"write_reg0", "write_reg1", "write_reg2", "write_reg3"};
    static const char* fn[4] = {"mac_op_mr", "alu_op_ar", "mac_op_mf", "alu_op_af"};
    static const char* rdm[3] = {"pgm_read_dag2", "data_read_dag1", "data_read_dag2"};
    static const char* wrm[3] = {"pgm_write_dag2", "data_write_dag1", "data_write_dag2"};
    string o = format("0x{:06x}", op), cnd = c == 15 ? "" : format("if (cond(d, {})) ", c);
    int32_t imm14 = int32_t((op >> 4) << 18) >> 18;
    if (hi == 0x00) return "/* nop */";
    if (hi >= 0x40 && hi <= 0x4f) return format("write_reg0(d, {}, 0x{:x});", R(0), (op >> 4) & 0xffff);
    if (hi >= 0x30 && hi <= 0x3f) return format("{}(d, {}, {});", wr[(hi >> 2) & 3], R(0), imm14);
    if (hi >= 0x80 && hi <= 0x8f) return format("{}(d, {}, int32_t(data_read(d, 0x{:04x})));", wr[(hi >> 2) & 3], R(0), (op >> 4) & 0x3fff);
    if (hi >= 0x90 && hi <= 0x9f) return format("data_write(d, 0x{:04x}, uint16_t({}(d, {})));", (op >> 4) & 0x3fff, rd[(hi >> 2) & 3], R(0));
    if (hi >= 0xa0 && hi <= 0xaf) return format("data_write_dag1(d, {}, 0x{:x});", o, (op >> 4) & 0xffff);
    if (hi >= 0xb0 && hi <= 0xbf) return format("data_write_dag2(d, {}, 0x{:x});", o, (op >> 4) & 0xffff);
    if (hi == 0x0d) return format("{}(d, {}, {}(d, {}));", wr[(op >> 10) & 3], R(4), rd[(op >> 8) & 3], R(0));
    if (hi >= 0x20 && hi <= 0x27) return format("{}{}(d, {});", cnd, fn[(hi >> 1) & 3], o);
    if (hi >= 0x28 && hi <= 0x2f) return format("{{ int32_t t = read_reg0(d, {}); {}(d, {}); write_reg0(d, {}, t); }}", R(0), fn[(hi >> 1) & 3], o, R(4));
    if (hi >= 0x50 && hi <= 0x7f) {
        uint32_t g = hi & 0x7e, mem = (g - 0x50) >> 4;
        const char* f = fn[(g >> 1) & 3];
        if (!((g >> 3) & 1)) return format("{}(d, {}); write_reg0(d, {}, int32_t({}(d, {})));", f, o, R(4), rdm[mem], o);
        return format("{}(d, {}, read_reg0(d, {})); {}(d, {});", wrm[mem], o, R(4), f, o);
    }
    if (hi >= 0xc0) {
        static const char* xd[4] = {"ax0", "ax1", "mx0", "mx1"};
        static const char* yd[4] = {"ay0", "ay1", "my0", "my1"};
        return format("{}(d, {}); d->core.{}.u = uint16_t(data_read_dag1(d, {})); d->core.{}.u = uint16_t(pgm_read_dag2(d, 0x{:x}));",
                      (hi & 2) ? "alu_op_ar" : "mac_op_mr", o, xd[(hi >> 2) & 3], o, yd[(hi >> 4) & 3], op >> 4);
    }
    if (hi == 0x0e) return format("{}shift_op(d, {});", cnd, o);
    if (hi == 0x0f) return format("shift_op_imm(d, {});", o);
    if (hi == 0x10) return format("shift_op(d, {}); write_reg0(d, {}, read_reg0(d, {}));", o, R(4), R(0));
    if (hi >= 0x11 && hi <= 0x13) {
        uint32_t mem = hi - 0x11;
        if (op & 0x8000) return format("{}(d, {}, read_reg0(d, {})); shift_op(d, {});", wrm[mem], o, R(4), o);
        return format("shift_op(d, {}); write_reg0(d, {}, int32_t({}(d, {})));", o, R(4), rdm[mem], o);
    }
    return format("exec(d, {});", o); // flags, stack control, DIVS/DIVQ, modify address, mode control
}

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: adsprecomp pm|bootN|ovl:B:O:C:D <input> <out.cpp> <name> [entry...]\n");
        return 1;
    }
    std::ifstream in(argv[2], std::ios::binary);
    std::vector<uint8_t> raw((std::istreambuf_iterator<char>(in)), {});
    std::vector<uint32_t> work = {0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x18};
    unsigned bank, off, count, dest;
    if (std::sscanf(argv[1], "ovl:%x:%x:%x:%x", &bank, &off, &count, &dest) == 4) {
        // Overlay copied into PM by the OS loader (0x03e8 in the bank-1 OS): 3 bytes per word,
        // 4th byte skipped. Entered at arbitrary addresses, so every word is an entry.
        size_t src = size_t(bank) * 0x1000 + off;
        if (raw.size() < src + count * 4) { std::fprintf(stderr, "%s too small for %s\n", argv[2], argv[1]); return 1; }
        for (uint32_t k = 0; k < count && dest + k < 0x4000; k++) {
            const uint8_t* b = &raw[src + k * 4];
            pm[dest + k] = (uint32_t(b[0]) << 16) | (uint32_t(b[1]) << 8) | b[2];
            valid[dest + k] = true;
            work.push_back(dest + k);
        }
    } else if (std::strncmp(argv[1], "boot", 4) == 0) { // "bootN": load_boot_data on the low bytes of bank N
        size_t base = size_t(std::atoi(argv[1] + 4)) * 0x1000;
        if (raw.size() < base + 0x1000) { std::fprintf(stderr, "%s too small for %s\n", argv[2], argv[1]); return 1; }
        const uint8_t* src = raw.data() + base;
        uint32_t pagelen = (src[3] + 1u) * 8;
        for (uint32_t i = 0; i < pagelen && i < 0x400; i++) {
            pm[i] = (uint32_t(src[i * 4]) << 16) | (uint32_t(src[i * 4 + 1]) << 8) | src[i * 4 + 2];
            valid[i] = true;
            work.push_back(i); // handlers are often reachable only via vectors patched at runtime
        }
    } else {
        if (raw.size() != 0x4000 * 4) { std::fprintf(stderr, "%s: expected 0x4000 words\n", argv[2]); return 1; }
        std::memcpy(pm.data(), raw.data(), raw.size());
        for (uint32_t a = 0; a < 0x4000; a++) valid[a] = a < 0x400 || (a >= 0x800 && a < 0x2000);
    }

    // Trace reachable code from reset, the interrupt vectors and any extra entries.
    for (int a = 5; a < argc; a++) work.push_back(std::stoul(argv[a], nullptr, 16));
    std::set<uint32_t> code, loop_ends;
    while (!work.empty()) {
        uint32_t a = work.back();
        work.pop_back();
        if (a >= 0x4000 || !valid[a] || !code.insert(a).second) continue;
        uint32_t op = pm[a], c = op & 15, hi = op >> 16;
        if (hi >= 0x18 && hi <= 0x1b) { add_succ(work, (op >> 4) & 0x3fff); if (c != 15) add_succ(work, a + 1); }
        else if (hi >= 0x1c && hi <= 0x1f) { add_succ(work, (op >> 4) & 0x3fff); add_succ(work, a + 1); }
        else if (hi == 0x0a) { if (c != 15) add_succ(work, a + 1); }
        else if (hi == 0x0b) { if (c != 15 || (op & 0x10)) add_succ(work, a + 1); }
        else if (hi == 0x03) { add_succ(work, ((op >> 4) & 0xfff) | (((op >> 2) & 3) << 12)); add_succ(work, a + 1); }
        else if (hi >= 0x14 && hi <= 0x17) { loop_ends.insert((op >> 4) & 0x3fff); add_succ(work, a + 1); }
        else add_succ(work, a + 1);
    }

    string name = argv[4], body;
    auto go = [&](uint32_t t) { return code.count(t) ? format("goto L_{:04x};", t) : format("{{ pc = 0x{:04x}; goto dispatch; }}", t); };
    for (uint32_t a : code) {
        uint32_t op = pm[a], c = op & 15, hi = op >> 16;
        bool le = loop_ends.count(a) > 0;
        string cnd = c == 15 ? "true" : format("cond(d, {})", c);
        string nx = le ? "nx" : format("0x{:04x}u", a + 1);
        string s;
        if (hi >= 0x18 && hi <= 0x1b) s = format("if ({}) {}", cnd, go((op >> 4) & 0x3fff));
        else if (hi >= 0x1c && hi <= 0x1f) s = format("if ({}) {{ pc_stack_push_val(d, {}); {} }}", cnd, nx, go((op >> 4) & 0x3fff));
        else if (hi == 0x0a) s = format("if ({}) {{ pc = pc_stack_pop_val(d); {}goto dispatch; }}", cnd, (op & 0x10) ? "stat_stack_pop(d); " : "");
        else if (hi == 0x0b)
            s = format("if ({}) {{ {}pc = d->i[{}] & 0x3fff; goto dispatch; }}", cnd,
                       (op & 0x10) ? format("pc_stack_push_val(d, {}); ", nx) : "", 4 + ((op >> 6) & 3));
        else if (hi == 0x03)
            s = format("if ({}d->flagin) {{ {}{} }}", (op & 2) ? "" : "!", (op & 1) ? format("pc_stack_push_val(d, {}); ", nx) : "",
                       go(((op >> 4) & 0xfff) | (((op >> 2) & 3) << 12)));
        else if (hi >= 0x14 && hi <= 0x17) s = format("loop_stack_push(d, 0x{:05x}); pc_stack_push_val(d, {});", op & 0x3ffff, nx);
        else if (hi == 0x02 && (op & 0x8000))
            s = format("{{ uint32_t v_ = idle(d, {0}); if (v_ >= HALT) {{ d->exit_pc = {0}; return; }} pc = v_; goto dispatch; }}", nx);
        else if (hi == 0x01) s = "/* IO space: ADSP-218x only */";
        else s = data_op(op);

        body += format("L_{:04x}: DSP_CHECK(0x{:04x})\n", a, a);
        if (le) // DO UNTIL end: the termination condition is evaluated before this instruction runs
            body += format("    {{ uint32_t nx = 0x{:04x}u; bool lp = false;\n"
                           "      if (d->loop == 0x{:04x}) {{ if (cond(d, d->loop_cond)) {{ lp = true; nx = pc_stack_top(d); }}"
                           " else {{ loop_stack_pop(d); pc_stack_pop_val(d); }} }}\n"
                           "      {}\n      if (lp) {{ pc = nx; goto dispatch; }} }}\n", a + 1, a, s);
        else body += "    " + s + "\n";
        if (!code.count(a + 1)) body += format("    pc = 0x{:04x}; goto dispatch;\n", a + 1);
    }

    string out = format("// Generated by adsprecomp from {}. Do not edit.\n#include \"dcs.h\"\nusing namespace adsp;\n\n", argv[2]);
    out += format("static void {}_fn(Dsp* d, uint32_t pc) {{\ndispatch:\n    switch (pc) {{\n", name);
    for (uint32_t a : code) out += format("    case 0x{:04x}: goto L_{:04x};\n", a, a);
    out += "    default: d->exit_pc = pc; return; // not in this image\n    }\n" + body + "}\n\n";
    out += format("static const uint32_t {}_words[][2] = {{\n", name);
    for (uint32_t a : code) out += format("    {{0x{:04x}, 0x{:06x}}},\n", a, pm[a]);
    out += format("}};\nextern const Program {0} = {{\"{0}\", {0}_fn, {0}_words, sizeof({0}_words) / sizeof({0}_words[0])}};\n", name);
    std::ofstream(argv[3], std::ios::binary) << out;
    std::printf("adsprecomp: %zu instructions, %zu loop ends -> %s\n", code.size(), loop_ends.size(), argv[3]);
    return 0;
}
