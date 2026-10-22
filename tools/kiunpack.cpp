// kiunpack: runs the recompiled boot ROM stub, which decompresses the boot program into
// work RAM, and writes that RAM image out so kirecomp can recompile it.
// usage: kiunpack <boot_rom> <out.bin>
#include <cstdio>
#include <fstream>
#include "recomp.h"

extern const ki::FuncEntry boot_stub_funcs[];
extern const size_t boot_stub_count;

int main(int argc, char** argv) {
    if (argc != 3) { std::fprintf(stderr, "usage: kiunpack <boot_rom> <out.bin>\n"); return 1; }
    ki::Bus bus;
    if (!bus.load_rom(argv[1])) return 1;
    ki::Ctx c;
    c.bus = &bus;
    ki::register_funcs(boot_stub_funcs, boot_stub_count);
    try {
        ki::run(&c, 0xbfc00000);
    } catch (const ki::MissingFunc& m) {
        if (m.vaddr != 0x88000000) { std::fprintf(stderr, "stub exited to unexpected %08x\n", m.vaddr); return 1; }
        std::ofstream(argv[2], std::ios::binary).write((const char*)bus.ram2.data(), bus.ram2.size());
        return 0;
    } catch (const ki::Unimplemented& u) {
        std::fprintf(stderr, "unimplemented %08x at %08x\n", u.word, u.pc);
    }
    return 1;
}
