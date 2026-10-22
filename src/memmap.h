#pragma once
// Killer Instinct main board memory map.
// Source: mame-reference/src/mame/rare/kinst.cpp (kinst_map, screen_update, vram_control_w).
// CPU is an R4600 in LITTLE-endian mode @ 100MHz, so guest words match x86 byte order.
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "ide.h"

namespace adsp { struct Board; }

namespace ki {

// Physical address ranges
constexpr uint32_t RAM_BASE  = 0x00000000, RAM_SIZE  = 0x00080000; // 512KB, holds both framebuffers
constexpr uint32_t RAM2_BASE = 0x08000000, RAM2_SIZE = 0x00800000; // 8MB work RAM
constexpr uint32_t ROM_BASE  = 0x1fc00000, ROM_SIZE  = 0x00080000; // u98 boot EPROM; reset vector 0xbfc00000

// IO registers (KI1 layout; kinst2_map shuffles these)
enum : uint32_t {
    REG_P1_VRAMCTL   = 0x10000080, // r: P1 inputs   w: vram control (bit 2 = framebuffer select)
    REG_P2_SNDRESET  = 0x10000088, // r: P2 inputs   w: DCS sound reset (bit 0)
    REG_VOL_SNDCTL   = 0x10000090, // r: volume/svc  w: sound control (rising bit 1 latches data)
    REG_UNUSED_SNDDT = 0x10000098, // r: unused      w: sound data
    REG_DSW          = 0x100000a0, // r: DIP switches
    REG_COIN         = 0x100000b0, // w: coin counter
    IDE_CS0_BASE     = 0x10000100, IDE_CS0_END = 0x1000013f, // ATA command block, reg = (addr-base)/8
    IDE_CS1_ALTSTAT  = 0x10000170, // ATA control block reg 6
};

// Video: 320x240 visible, 15bpp BGR555 (R in bits 0-4), 640-byte rows, lives in RAM
constexpr int SCREEN_W = 320, SCREEN_H = 240;
constexpr uint32_t FB0_OFFSET = 0x30000, FB1_OFFSET = 0x58000;
constexpr double VBLANK_HZ = 50e6 / 8 / 406 / 261; // ~58.96Hz, from screen.set_raw

// Widescreen (kinst.ini `widescreen = on`): each framebuffer becomes a WS_W x 240 buffer with
// WS_PITCH-byte rows, the game's 320 columns in the middle (WS_M margin columns each side).
//  * View A: the game's own framebuffer addresses (FB0/FB1, 640-byte rows). With widescreen on the
//    bus redirects them into the centre of the wide buffer, so unpatched drawing works unchanged.
//  * View B: physical WS_BASE.., the wide buffers themselves (pitch 1024). Routines patched in
//    tools/widescreen.patches draw here, x = -WS_M..319+WS_M, filling the margins.
// Margins of a frame that no view-B write touched show black (menus, pre-rendered stages).
constexpr int WS_W = 432, WS_M = 56, WS_PITCH = 1024;
constexpr uint32_t WS_BASE = 0x00400000, WS_SIZE = 0x40000; // per buffer; 240 rows fit in 0x3c000

// Strip MIPS segment bits: kseg0 (0x8...) and kseg1 (0xa...) both alias physical 0-512MB.
// kuseg/kseg2 data accesses go through Bus::translate (TLB); code addresses never do.
constexpr uint32_t to_phys(uint32_t vaddr) {
    return (vaddr >= 0x80000000u && vaddr < 0xc0000000u) ? (vaddr & 0x1fffffffu) : vaddr;
}

struct Bus {
    std::vector<uint8_t> ram  = std::vector<uint8_t>(RAM_SIZE);
    std::vector<uint8_t> ram2 = std::vector<uint8_t>(RAM2_SIZE);
    std::vector<uint8_t> rom  = std::vector<uint8_t>(ROM_SIZE, 0xff);

    // Latched write-only registers
    uint32_t vram_control = 0, sound_reset = 0, sound_control = 0, sound_data = 0, coin_control = 0;
    std::atomic<uint32_t> flips = 0; // framebuffer select changes = frames the game finished (FPS counter)
    // Inputs are active-low; written by the host input thread, read by the guest
    std::atomic<uint32_t> p1 = ~0u, p2 = ~0u, volume = ~0u, dsw = ~0u;

    bool widescreen = false;
    std::vector<uint8_t> wide = std::vector<uint8_t>(2 * WS_SIZE);
    bool margins_drawn[2] = {};     // per wide buffer: something drew into the margins this frame

    Ide ide;
    adsp::Board* dcs = nullptr; // DCS sound board, if attached

    bool load_rom(const std::string& path);

    // CPU interrupt inputs as Cause.IP2+ bits: bit 0 = VBLANK (line 0), bit 1 = IDE (line 1)
    uint32_t irq_lines() const { return (in_vblank() ? 1u : 0u) | (ide.irq_out() ? 2u : 0u); }
    bool in_vblank() const;
    double frame_clock() const; // video frames since start (fraction = beam position)
    int64_t flip_frame = -1;    // frame in which the game last flipped the framebuffer

    // Returns host pointer for RAM/ROM, nullptr for IO or unmapped.
    uint8_t* host_ptr(uint32_t paddr) {
        if (paddr < RAM_BASE + RAM_SIZE) {
            if (widescreen) {
                for (uint32_t b = 0; b < 2; b++)
                    if (uint32_t o = paddr - (b ? FB1_OFFSET : FB0_OFFSET); o < SCREEN_W * SCREEN_H * 2)
                        return &wide[b * WS_SIZE + o / 640 * WS_PITCH + WS_M * 2 + o % 640];
            }
            return &ram[paddr - RAM_BASE];
        }
        if (widescreen && paddr - WS_BASE < 2 * WS_SIZE) {
            margins_drawn[(paddr - WS_BASE) / WS_SIZE] = true;
            return &wide[paddr - WS_BASE];
        }
        if (paddr - RAM2_BASE < RAM2_SIZE) return &ram2[paddr - RAM2_BASE];
        if (paddr - ROM_BASE < ROM_SIZE) return &rom[paddr - ROM_BASE];
        return nullptr;
    }

    // R4600 TLB, only what the game uses: tlbwi entries, matched on VPN2 (ASID ignored). The sprite
    // renderer reads its palette through a mapping at virtual 0 (lh rX, 0(pixel byte), 0x88010b94).
    struct TlbEntry { uint32_t hi = 0, lo[2] = {}, mask = 0; };
    TlbEntry tlb[48];
    void tlb_write(uint32_t index, uint32_t hi, uint32_t lo0, uint32_t lo1, uint32_t pagemask) {
        tlb[index % 48] = {hi & ~0x1fffu, {lo0, lo1}, pagemask | 0x1fffu};
    }
    // ponytail: linear scan and unmapped addresses fall through as identity (no TLB-miss
    // exception); add a micro-TLB / refill handling if profiling or the game needs it.
    uint32_t translate(uint32_t vaddr) const {
        if (vaddr - 0x80000000u < 0x40000000u) return vaddr & 0x1fffffffu;
        for (const TlbEntry& e : tlb) {
            if (((vaddr ^ e.hi) & ~e.mask) != 0) continue;
            uint32_t page = (e.mask >> 1) | 0xfffu, lo = e.lo[(vaddr & (page + 1)) ? 1 : 0];
            if (!(lo & 2)) continue; // V bit
            return (((lo >> 6) << 12) & ~page) | (vaddr & page);
        }
        return vaddr;
    }

    template <class T> T read(uint32_t vaddr) {
        uint32_t p = translate(vaddr);
        if (uint8_t* h = host_ptr(p)) { T v; std::memcpy(&v, h, sizeof v); return v; }
        return T(io_read(p & ~3u) >> ((p & 3) * 8));
    }
    template <class T> void write(uint32_t vaddr, T v) {
        uint32_t p = translate(vaddr);
        if (p - ROM_BASE < ROM_SIZE) return; // ROM is read-only
        if (uint8_t* h = host_ptr(p)) { std::memcpy(h, &v, sizeof v); return; }
        io_write(p & ~3u, uint32_t(v) << ((p & 3) * 8));
    }

    uint32_t io_read(uint32_t paddr);
    void io_write(uint32_t paddr, uint32_t data);

    const uint16_t* framebuffer() const {
        return reinterpret_cast<const uint16_t*>(&ram[(vram_control & 4) ? FB1_OFFSET : FB0_OFFSET]);
    }
    // The displayed picture: the framebuffer above, or with widescreen the whole wide buffer
    // (width WS_W, pitch WS_PITCH; margins_valid = something drew into its margins).
    bool back_has_margins() const { return widescreen && margins_drawn[(vram_control & 4) ? 0 : 1]; }
    struct Screen { const uint16_t* pixels; int width, pitch; bool margins_valid; };
    Screen screen() const {
        if (!widescreen) return {framebuffer(), SCREEN_W, SCREEN_W * 2, false};
        int b = (vram_control & 4) ? 1 : 0;
        return {reinterpret_cast<const uint16_t*>(&wide[b * WS_SIZE]), WS_W, WS_PITCH, margins_drawn[b]};
    }
};

} // namespace ki
