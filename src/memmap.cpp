#include "memmap.h"
#include "dcs.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <thread>

namespace ki {

// ponytail: derived from wall-clock time; switch to a guest-cycle clock if the game's speed
// needs to be decoupled from the host (fast-forward, frame stepping).
double Bus::seconds() const {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

double Bus::frame_clock() const {
    double now = seconds();
    Vsync v;
    {
        std::lock_guard g(vsync_lock);
        v = vsync;
    }
    if (now - v.t < 0.1) return double(v.n) + (now - v.t) / v.period;
    return now * VBLANK_HZ;
}

// VBLANK is asserted for lines 240-260 of each 261-line frame (screen.set_raw in kinst.cpp).
// The main loop (0x88001408) waits for the VBLANK *level*, flips, and runs a frame. On the real
// 100MHz CPU a frame takes longer than the 1.36ms VBLANK; natively it can finish inside it and
// run again, so the game ran up to ~1.7x too fast. Once the game has flipped during a VBLANK,
// that VBLANK reads as over.
bool Bus::in_vblank() const {
    double frame = frame_clock();
    return frame - std::floor(frame) >= 240.0 / 261.0 && int64_t(frame) != flip_frame;
}

bool Bus::load_rom(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.read(reinterpret_cast<char*>(rom.data()), ROM_SIZE)) {
        std::fprintf(stderr, "failed to read %u bytes from %s\n", ROM_SIZE, path.c_str());
        return false;
    }
    return true;
}

// ponytail: sub-word IO writes overwrite the whole register (MAME uses COMBINE_DATA with a mask);
// pass a mask through if the game turns out to do byte stores to these.
uint32_t Bus::io_read(uint32_t paddr) {
    switch (paddr) {
    case REG_P1_VRAMCTL:   return p1;
    case REG_P2_SNDRESET:  return p2;
    case REG_VOL_SNDCTL: { // bit 1 = DCS input latch empty (sound_status_r); "ready" without a board
        if (!dcs) return volume;
        uint32_t empty = (dcs->control_r() >> 11) & 1u;
        // The game polls this with an iteration-count timeout (0x16e360 loops at 0x88003f40) sized
        // for the real CPU; natively that's ~2ms and it resets the sound board when the DSP thread
        // is briefly busy. Yielding while the latch is full gives each poll real-hardware-ish cost.
        if (!empty) std::this_thread::yield();
        return (volume & ~2u) | (empty << 1);
    }
    case REG_UNUSED_SNDDT: return ~0u;
    case REG_DSW:          return dsw;
    case IDE_CS1_ALTSTAT:  return ide.present() ? ide.alt_status() : ~0u;
    }
    if (paddr >= IDE_CS0_BASE && paddr <= IDE_CS0_END) return ide.read(int(paddr - IDE_CS0_BASE) / 8);
    return ~0u; // unmapped reads float high (unmap_value_high)
}

void Bus::io_write(uint32_t paddr, uint32_t data) {
    switch (paddr) {
    case REG_P1_VRAMCTL:
        if ((vram_control ^ data) & 4) { // a flip during VBLANK ends it for the game (see in_vblank)
            flips++;
            if (in_vblank()) flip_frame = int64_t(frame_clock());
            margins_drawn[(data & 4) ? 0 : 1] = false; // the new back buffer starts a fresh frame
        }
        vram_control = data;
        return;
    case REG_P2_SNDRESET:
        sound_reset = data;
        if (dcs) dcs->reset_w(data & 1);
        return;
    case REG_VOL_SNDCTL: { // rising edge of bit 1 latches the sound data into the DCS
        uint32_t prev = sound_control;
        sound_control = data;
        if (dcs && !(prev & 2) && (data & 2)) dcs->data_w(uint16_t(sound_data));
        return;
    }
    case REG_UNUSED_SNDDT: sound_data = data; return;
    case REG_COIN:         coin_control = data; return;
    case IDE_CS1_ALTSTAT:  ide.control(data); return;
    }
    if (paddr >= IDE_CS0_BASE && paddr <= IDE_CS0_END) ide.write(int(paddr - IDE_CS0_BASE) / 8, data);
}

} // namespace ki
