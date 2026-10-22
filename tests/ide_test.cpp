// Drive writes persist: a sector written through the ATA registers comes back after reopening the
// image, the image file itself is untouched, and the change log is compacted on open.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include "ide.h"

#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

using ki::Ide;

static void select_lba(Ide& d, uint32_t lba) {
    d.write(2, 1);                                 // one sector
    d.write(3, lba & 0xff);
    d.write(4, (lba >> 8) & 0xff);
    d.write(5, (lba >> 16) & 0xff);
    d.write(6, 0x40 | ((lba >> 24) & 15));         // LBA mode
}

static void write_sector(Ide& d, uint32_t lba, uint16_t seed) {
    select_lba(d, lba);
    d.write(7, 0x30);
    for (int i = 0; i < 256; i++) d.write(0, uint16_t(seed + i));
}

int main() {
    const std::string img = "ide_test.img", log = img + ".changes";
    std::filesystem::remove(log);
    { std::ofstream f(img, std::ios::binary | std::ios::trunc); f.seekp(Ide::BYTES - 1); f.put(0); }

    {
        Ide d;
        CHECK(d.open(img));
        write_sector(d, 1000, 7);
        write_sector(d, 1000, 100); // rewrite: the log holds two records until the next open
    }
    CHECK(std::filesystem::file_size(log) == 2 * 516);

    {
        Ide e;
        CHECK(e.open(img));
        CHECK(std::filesystem::file_size(log) == 516);
        select_lba(e, 1000);
        e.write(7, 0x20);
        for (int i = 0; i < 256; i++) CHECK(e.read(0) == uint16_t(100 + i));
    }

    std::ifstream raw(img, std::ios::binary);
    raw.seekg(1000 * 512);
    CHECK(raw.get() == 0); // the installed image stays pristine
    raw.close();
    std::filesystem::remove(img);
    std::filesystem::remove(log);
    std::printf("ok\n");
    return 0;
}
