#undef NDEBUG // keep asserts in Release builds
#include <cassert>
#include <cstdio>
#include "dcs.h"
#include "recomp.h"

int main() {
    using namespace ki;
    static_assert(to_phys(0xbfc00000) == 0x1fc00000);
    static_assert(to_phys(0x88000010) == 0x08000010);
    static_assert(to_phys(0x00001000) == 0x00001000);

    Bus bus;
    bus.write<uint32_t>(0x80000100, 0x11223344);           // kseg0 write...
    assert(bus.read<uint32_t>(0xa0000100) == 0x11223344);  // ...visible through kseg1
    assert(bus.read<uint8_t>(0x00000100) == 0x44);         // little-endian byte order
    bus.write<uint32_t>(0xa8000000, 0xdeadbeef);
    assert(bus.read<uint16_t>(0x88000002) == 0xdead);

    bus.rom[0] = 0xe2;
    bus.write<uint32_t>(0xbfc00000, 0);                    // ROM ignores writes
    assert(bus.rom[0] == 0xe2);

    assert(bus.read<uint32_t>(0xb0000080) == ~0u);         // P1 idle (active-low)
    bus.write<uint32_t>(0xb0000080, 4);
    assert(bus.framebuffer() == reinterpret_cast<const uint16_t*>(&bus.ram[FB1_OFFSET]));
    assert(bus.read<uint32_t>(0xb0001000) == ~0u);         // unmapped floats high

    // TLB: 4KB page pair at virtual 0 -> even page at phys 0x08001000, odd page invalid
    // (EntryLo = PFN << 6 | V << 1, as 0x8800b044 builds it).
    bus.tlb_write(5, 0x00000000, (0x08001000 >> 12) << 6 | 2, 0, 0);
    bus.write<uint16_t>(0x88001010, 0x7c1f);
    assert(bus.read<uint16_t>(0x00000010) == 0x7c1f);     // mapped
    assert(bus.translate(0x00001010) == 0x00001010);      // invalid odd page: identity
    assert(bus.translate(0x88001010) == 0x08001010);      // kseg0 never uses the TLB

    // Little-endian unaligned pairs, as the boot stub uses them: ldr X / ldl X+7
    Ctx c;
    c.bus = &bus;
    for (int i = 0; i < 16; i++) bus.ram[0x200 + i] = uint8_t(i);
    for (int b = 0; b < 8; b++) {
        uint64_t x = 0x80000200 + b, want = 0;
        for (int k = 7; k >= 0; k--) want = (want << 8) | uint64_t(b + k);
        uint64_t v = ldl<8>(&c, ldr<8>(&c, 0xdeadbeefdeadbeefull, x), x + 7);
        assert(v == want);
        uint32_t w = uint32_t(ldl<4>(&c, ldr<4>(&c, 0, x), x + 3));
        assert(w == uint32_t(want));
        sdr<8>(&c, 0x1122334455667788ull, 0x80000300 + b);   // sdr X / sdl X+7 round-trips
        sdl<8>(&c, 0x1122334455667788ull, 0x80000307 + b);
        assert(bus.read<uint64_t>(0x80000300 + b) == 0x1122334455667788ull);
    }

    // ADSP-2105 semantics (src/adsp.h), known answers
    {
        static adsp::Board board;
        adsp::Dsp* d = &board.dsp;
        d->board = &board;
        d->core.ax0.u = 0x7fff; d->core.ay0.u = 1;
        adsp::alu_op_ar(d, 3u << 13);                         // AR = AX0 + AY0
        assert(d->core.ar.u == 0x8000);
        assert((d->astat & (adsp::NFLAG | adsp::VFLAG | adsp::ZFLAG | adsp::CFLAG)) == (adsp::NFLAG | adsp::VFLAG));
        d->core.mx0.u = 0x4000; d->core.my0.u = 0x4000;       // 0.5 * 0.5 in 1.15
        adsp::mac_op_mr(d, 4u << 13);                         // MR = MX0 * MY0 (SS), fractional
        assert(d->core.mr.x.mr1.u == 0x2000 && d->core.mr.x.mr0.u == 0);
        adsp::write_reg1(d, 0x8, 4);                          // L0 = 4
        adsp::write_reg1(d, 0x0, 0x100);                      // I0 = 0x100 (circular base)
        adsp::write_reg1(d, 0x4, 1);                          // M0 = 1
        for (int k = 0; k < 4; k++) adsp::data_read_dag1(d, 0);
        assert(d->i[0] == 0x100);                             // wrapped back to the base
        adsp::write_reg3(d, 0x5, 3);                          // CNTR = 3
        assert(adsp::cond(d, 14) && adsp::cond(d, 14) && !adsp::cond(d, 14)); // CE after 3
    }

    std::puts("memmap ok");
}
