#include "ide.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace ki {

// ATA strings are stored as big-endian character pairs per word.
static void ata_string(uint16_t* w, int words, const char* s) {
    for (int i = 0; i < words * 2; i++) {
        char ch = *s ? *s++ : ' ';
        w[i / 2] = (i & 1) ? uint16_t((w[i / 2] & 0xff00) | uint8_t(ch)) : uint16_t(uint8_t(ch) << 8);
    }
}

bool Ide::open(const std::string& path) {
    img.open(path, std::ios::binary | std::ios::ate);
    if (!img || uint64_t(img.tellg()) != BYTES) {
        std::fprintf(stderr, "bad drive image %s\n", path.c_str());
        img.close();
        return false;
    }
    int cyls = CYLS;
    heads = HEADS;
    spt = SPT;
    total_sectors = uint32_t(BYTES / 512);

    // Replay the change log (a torn last record is dropped), rewrite it compacted via a temp file so a
    // crash mid-rewrite can't lose it, then keep appending to it.
    const std::string log = path + ".changes", tmp = log + ".tmp";
    written.clear();
    if (std::ifstream in(log, std::ios::binary); in) {
        uint32_t l = 0;
        std::array<uint8_t, 512> s;
        while (in.read(reinterpret_cast<char*>(&l), 4) && in.read(reinterpret_cast<char*>(s.data()), 512))
            if (l < total_sectors) written[l] = s;
    }
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        for (auto& [l, s] : written) {
            out.write(reinterpret_cast<const char*>(&l), 4);
            out.write(reinterpret_cast<const char*>(s.data()), 512);
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, log, ec);
    if (ec) std::fprintf(stderr, "can't update %s: %s\n", log.c_str(), ec.message().c_str());
    changes.open(log, std::ios::binary | std::ios::app);

    // ide_build_identify_device(), with the model/serial patch from kinst_state::machine_reset()
    identify[0] = 0x045a;
    identify[1] = uint16_t(cyls);
    identify[3] = uint16_t(heads);
    identify[6] = uint16_t(spt);
    ata_string(&identify[10], 10, "00000000000000000000");
    identify[22] = 4;
    ata_string(&identify[23], 4, "1.0");
    ata_string(&identify[27], 20, "ST9150AG");   // the boot ROM refuses any other model
    identify[47] = 0x8010;
    identify[49] = 0x0f03;
    identify[51] = 2;
    identify[52] = 2;
    identify[53] = 3;
    identify[54] = uint16_t(cyls);
    identify[55] = uint16_t(heads);
    identify[56] = uint16_t(spt);
    identify[57] = uint16_t(total_sectors);
    identify[58] = uint16_t(total_sectors >> 16);
    identify[60] = uint16_t(total_sectors);
    identify[61] = uint16_t(total_sectors >> 16);
    return true;
}

uint32_t Ide::lba() const {
    if (head & 0x40) return (uint32_t(head & 15) << 24) | (cyl_hi << 16) | (cyl_lo << 8) | sector;
    return (((cyl_hi << 8 | cyl_lo) * heads + (head & 15)) * spt) + sector - 1;
}

void Ide::next_sector() {
    if (head & 0x40) {
        uint32_t l = lba() + 1;
        sector = uint8_t(l); cyl_lo = uint8_t(l >> 8); cyl_hi = uint8_t(l >> 16);
        head = uint8_t((head & 0xf0) | ((l >> 24) & 15));
        return;
    }
    if (++sector > spt) {
        sector = 1;
        int h = (head & 15) + 1;
        if (h >= heads) { h = 0; if (++cyl_lo == 0) cyl_hi++; }
        head = uint8_t((head & 0xf0) | h);
    }
}

bool Ide::read_sector(uint32_t l, uint8_t* out) {
    if (l >= total_sectors) return false;
    if (auto it = written.find(l); it != written.end()) { std::memcpy(out, it->second.data(), 512); return true; }
    img.seekg(std::streamoff(l) * 512);
    return bool(img.read(reinterpret_cast<char*>(out), 512));
}

void Ide::load_sector() {
    pos = 0;
    if (read_sector(lba(), buf)) status = DRDY | DSC | DRQ;
    else { status = DRDY | DSC | ERR; error = 0x10; } // IDNF
    finish_irq();
}

uint32_t Ide::read(int reg) {
    if (!present()) return ~0u;
    switch (reg) {
    case 0: {
        if (!(status & DRQ) || writing) return 0;
        uint16_t v = uint16_t(buf[pos] | buf[pos + 1] << 8);
        if ((pos += 2) == 512) {
            if (--remaining > 0) { next_sector(); load_sector(); }
            else status &= ~DRQ;
        }
        return v;
    }
    case 1: return error;
    case 2: return count;
    case 3: return sector;
    case 4: return cyl_lo;
    case 5: return cyl_hi;
    case 6: return head;
    case 7: irq = false; return status;  // reading status acknowledges INTRQ
    }
    return 0;
}

void Ide::write(int reg, uint32_t v) {
    if (!present()) return;
    uint8_t b = uint8_t(v);
    switch (reg) {
    case 0:
        if (!(status & DRQ) || !writing) return;
        buf[pos] = uint8_t(v); buf[pos + 1] = uint8_t(v >> 8);
        if ((pos += 2) == 512) {
            uint32_t l = lba();
            if (l < total_sectors) {
                std::memcpy(written[l].data(), buf, 512);
                changes.write(reinterpret_cast<const char*>(&l), 4);
                changes.write(reinterpret_cast<const char*>(buf), 512);
                changes.flush();
            }
            finish_irq();
            pos = 0;
            if (--remaining > 0) next_sector();
            else { status &= ~DRQ; writing = false; }
        }
        return;
    case 1: feature = b; return;
    case 2: count = b; return;
    case 3: sector = b; return;
    case 4: cyl_lo = b; return;
    case 5: cyl_hi = b; return;
    case 6: head = b; return;
    case 7: break;
    default: return;
    }

    command = b;
    static const bool trace = std::getenv("KI_IDE_TRACE") != nullptr;
    if (trace) std::fprintf(stderr, "ide: cmd %02x lba %u count %u\n", command, lba(), count ? count : 256);
    error = 0;
    status = DRDY | DSC;
    irq = false;
    writing = false;
    switch (command) {
    case 0x20: case 0x21: case 0xc4: // read sectors / read multiple
        remaining = count ? count : 256;
        load_sector();
        return;
    case 0x30: case 0x31: case 0xc5: // write sectors / write multiple
        remaining = count ? count : 256;
        writing = true; pos = 0; status |= DRQ;
        return;
    case 0xec: // identify device
        std::memcpy(buf, identify, 512);
        remaining = 1; pos = 0; status |= DRQ;
        finish_irq();
        return;
    case 0x91: // set config: logical geometry
        spt = count ? count : 256;
        heads = (head & 15) + 1;
        finish_irq();
        return;
    case 0x90: error = 1; finish_irq(); return; // diagnostic: passed
    case 0x08: case 0x70: case 0xc6: case 0xef: case 0xe0: case 0xe1: case 0xe2: case 0xe3: case 0xe5: case 0xe7:
        finish_irq();
        return;
    }
    if ((command & 0xf0) == 0x10) { finish_irq(); return; } // recalibrate
    std::fprintf(stderr, "ide: unsupported command %02x\n", command);
    status |= ERR; error = 0x04; // ABRT
    finish_irq();
}

void Ide::control(uint32_t v) {
    if ((devctl & 4) && !(v & 4)) { status = DRDY | DSC; error = 1; irq = false; } // end of soft reset
    devctl = uint8_t(v);
}

} // namespace ki
