#pragma once
// Minimal ATA hard disk backed by the raw drive image that `kinst --install` unpacks from the CHD. Behaviour mirrors
// mame-reference/src/devices/machine/atastorage.cpp + atahle.cpp, minus timing:
// every command completes instantly.
#include <array>
#include <fstream>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ki {

struct Ide {
    enum : uint8_t { ERR = 0x01, DRQ = 0x08, DSC = 0x10, DRDY = 0x40, BSY = 0x80 };

    // KI's Seagate ST9150AG (the boot ROM checks the model string).
    static constexpr int CYLS = 419, HEADS = 13, SPT = 47;
    static constexpr uint64_t BYTES = uint64_t(CYLS) * HEADS * SPT * 512;

    bool open(const std::string& image_path);
    bool present() const { return img.is_open(); }

    uint32_t read(int reg);            // CS0 register 0-7
    void write(int reg, uint32_t v);
    uint32_t alt_status() const { return status; }       // CS1 reg 6 read
    void control(uint32_t v);                            // CS1 reg 6 write
    bool irq_out() const { return irq && !(devctl & 2); } // nIEN masks INTRQ

    // Task file
    uint8_t feature = 0, count = 0, sector = 0, cyl_lo = 0, cyl_hi = 0, head = 0, command = 0, devctl = 0;
    uint8_t status = DRDY | DSC, error = 0;
    bool irq = false;

private:
    uint32_t lba() const;
    void next_sector();
    void load_sector();
    bool read_sector(uint32_t lba, uint8_t* out);
    void finish_irq() { irq = true; }

    std::ifstream img;
    uint32_t total_sectors = 0;
    // ponytail: sector writes live in memory only and are lost on exit; persist to a sidecar
    // file when audits/high scores need to survive restarts.
    std::unordered_map<uint32_t, std::array<uint8_t, 512>> written;

    int heads = 0, spt = 0;          // current logical geometry (SET_CONFIG changes it)
    uint16_t identify[256]{};
    uint8_t buf[512]{};
    int pos = 512, remaining = 0;
    bool writing = false;
};

} // namespace ki
