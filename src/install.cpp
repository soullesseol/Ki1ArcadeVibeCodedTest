#include "install.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <libchdr/chd.h>
#include <miniz.h>

namespace fs = std::filesystem;

namespace ki {

namespace {

constexpr uint32_t ROM_BYTES = 0x80000;
// The exe is recompiled from v1.4 (ki-l14.u98 / u98-l14), so only that boot ROM works.
constexpr uint32_t BOOT_CRC = 0xafedb75f;
struct Known { uint32_t crc; const char* name; };
constexpr Known OTHER_BOOTS[] = {{0x7b65ca3d, "v1.5d"}, {0x65f7ea31, "v1.3"}, {0x05e67bcb, "proto v4.7"}, {0x230f55fb, "v1.5d AnyIDE"}};
// Sound ROMs in sound-region order (kinst.cpp ROM_LOAD16_BYTE slots 0x000000, 0x200000, ...).
constexpr Known SOUND_ROMS[8] = {{0xb6cc155f, "u10-l1"}, {0x0b5e05df, "u11-l1"}, {0xd05ce6ad, "u12-l1"}, {0x7d0954ea, "u13-l1"},
                                 {0x8bbe4f0c, "u33-l1"}, {0xb2e73603, "u34-l1"}, {0x0aaef4fc, "u35-l1"}, {0x0577bb60, "u36-l1"}};
// SHA1 of kinst.chd's raw sectors. MAME lists the combined raw+metadata SHA1 (81d83323...); the
// raw one is the same in every CHD version (v3 stores it as `sha1`, v4/v5 as `rawsha1`).
constexpr uint8_t DISK_SHA1[20] = {0xa3, 0x7a, 0x2c, 0x5e, 0x52, 0xea, 0x93, 0x6a, 0x71, 0x52,
                                   0x10, 0xd2, 0x37, 0x87, 0x4d, 0xd5, 0x73, 0xbb, 0x00, 0x2f};
constexpr char BOOT_MAGIC[8] = {'K', 'I', 'B', 'O', 'O', 'T', 0, 1};
constexpr size_t SOUND_BYTES = 0x800000 * 2;

struct Found {
    std::vector<uint8_t> boot;
    std::array<std::vector<uint8_t>, 8> sound;
    std::string chd;
    std::vector<std::string> other_boots;
};

// Where a 512KB ROM with this CRC goes, or nullptr if it isn't one we still need.
std::vector<uint8_t>* slot_for(Found& f, uint32_t crc) {
    if (crc == BOOT_CRC) return f.boot.empty() ? &f.boot : nullptr;
    for (int k = 0; k < 8; k++)
        if (crc == SOUND_ROMS[k].crc) return f.sound[k].empty() ? &f.sound[k] : nullptr;
    for (const Known& o : OTHER_BOOTS)
        if (crc == o.crc) f.other_boots.push_back(o.name);
    return nullptr;
}

bool chd_is_kinst(const fs::path& p) {
    chd_file* chd = nullptr;
    if (chd_open(p.string().c_str(), CHD_OPEN_READ, nullptr, &chd) != CHDERR_NONE) return false;
    const chd_header* h = chd_get_header(chd);
    bool ok = std::memcmp(h->version <= 3 ? h->sha1 : h->rawsha1, DISK_SHA1, 20) == 0 && h->logicalbytes == Ide::BYTES;
    chd_close(chd);
    return ok;
}

void scan_file(Found& f, const fs::path& p) {
    std::string ext = p.extension().string();
    for (char& ch : ext) ch = char(std::tolower(uint8_t(ch)));
    std::error_code ec;
    if (ext == ".chd") {
        if (f.chd.empty() && chd_is_kinst(p)) f.chd = p.string();
    } else if (ext == ".zip") {
        mz_zip_archive z{};
        if (!mz_zip_reader_init_file(&z, p.string().c_str(), 0)) return;
        for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&z); i++) {
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&z, i, &st) || st.m_uncomp_size != ROM_BYTES) continue;
            if (std::vector<uint8_t>* v = slot_for(f, st.m_crc32)) {
                v->resize(ROM_BYTES); // extraction verifies the CRC
                if (!mz_zip_reader_extract_to_mem(&z, i, v->data(), ROM_BYTES, 0)) v->clear();
            }
        }
        mz_zip_reader_end(&z);
    } else if (fs::file_size(p, ec) == ROM_BYTES) {
        std::vector<uint8_t> data(ROM_BYTES);
        std::ifstream(p, std::ios::binary).read(reinterpret_cast<char*>(data.data()), ROM_BYTES);
        if (std::vector<uint8_t>* v = slot_for(f, uint32_t(mz_crc32(MZ_CRC32_INIT, data.data(), ROM_BYTES)))) *v = std::move(data);
    }
}

// ponytail: a whole MAME roms folder works but opens every zip's directory; fine for a one-time step.
void scan(Found& f, const fs::path& p) {
    std::error_code ec;
    if (!fs::is_directory(p, ec)) { scan_file(f, p); return; }
    for (auto it = fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec)) scan_file(f, it->path());
}

void progress(int pct) { std::printf("progress %d\n", pct); std::fflush(stdout); }
bool fail(const std::string& msg) { std::printf("error: %s\n", msg.c_str()); std::fflush(stdout); return false; }

// Write to name.tmp and rename, so a half-written file never looks finished.
bool write_file(const fs::path& dir, const char* name, const std::vector<std::pair<const void*, size_t>>& parts) {
    fs::path tmp = dir / (std::string(name) + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary);
        for (auto [p, n] : parts) out.write(static_cast<const char*>(p), std::streamsize(n));
        if (!out) return false;
    }
    std::error_code ec;
    fs::rename(tmp, dir / name, ec);
    return !ec;
}

bool write_disk(const std::string& chd_path, const fs::path& dir) {
    chd_file* chd = nullptr;
    if (chd_open(chd_path.c_str(), CHD_OPEN_READ, nullptr, &chd) != CHDERR_NONE) return false;
    const chd_header* h = chd_get_header(chd);
    std::vector<uint8_t> hunk(h->hunkbytes);
    fs::path tmp = dir / (std::string(DISK_FILE) + ".tmp");
    bool ok = true;
    {
        std::ofstream out(tmp, std::ios::binary);
        uint64_t left = Ide::BYTES;
        for (uint32_t i = 0; ok && left; i++) {
            ok = chd_read(chd, i, hunk.data()) == CHDERR_NONE;
            size_t n = size_t(std::min<uint64_t>(left, hunk.size()));
            out.write(reinterpret_cast<const char*>(hunk.data()), std::streamsize(n));
            left -= n;
            if (i % 512 == 0) progress(10 + int(85 * (Ide::BYTES - left) / Ide::BYTES));
        }
        ok = ok && bool(out);
    }
    chd_close(chd);
    std::error_code ec;
    if (ok) fs::rename(tmp, dir / DISK_FILE, ec);
    return ok && !ec;
}

// Register image of boot.bin, in file order.
std::vector<std::pair<void*, size_t>> boot_parts(Ctx& c, Bus& bus) {
    return {{c.r, sizeof c.r}, {&c.hi, sizeof c.hi}, {&c.lo, sizeof c.lo}, {c.f, sizeof c.f}, {c.fcr, sizeof c.fcr},
            {c.cop0, sizeof c.cop0}, {bus.ram.data(), bus.ram.size()}, {bus.ram2.data(), bus.ram2.size()}};
}

} // namespace

bool install(const std::vector<std::string>& sources, const std::string& data_dir, const FuncEntry* stub, size_t stub_count) {
    progress(0);
    Found f;
    for (const std::string& s : sources) scan(f, s);
    std::string missing;
    if (f.boot.empty()) missing += " u98-l14 (boot ROM v1.4, CRC afedb75f)";
    for (int k = 0; k < 8; k++)
        if (f.sound[k].empty()) missing += std::string(" ") + SOUND_ROMS[k].name;
    if (f.chd.empty()) missing += " kinst.chd";
    if (!missing.empty()) {
        std::string msg = "missing Killer Instinct files:" + missing;
        if (f.boot.empty() && !f.other_boots.empty())
            msg += " (found boot ROM " + f.other_boots[0] + "; this build needs v1.4)";
        return fail(msg);
    }
    progress(5);

    std::error_code ec;
    fs::path dir(data_dir);
    fs::create_directories(dir, ec);
    fs::remove(dir / BOOT_FILE, ec); // boot.bin is written last: its presence marks a finished install

    // Sound region: each 512KB ROM fills the low bytes of a 0x100000-word slot (high bytes 0xff).
    std::vector<uint16_t> sound(SOUND_BYTES / 2, 0xffff);
    for (size_t k = 0; k < 8; k++)
        for (size_t i = 0; i < ROM_BYTES; i++) sound[k * 0x100000 + i] = uint16_t(0xff00 | f.sound[k][i]);
    if (!write_file(dir, SOUND_FILE, {{sound.data(), SOUND_BYTES}})) return fail("can't write " + (dir / SOUND_FILE).string());
    progress(10);

    if (!write_disk(f.chd, dir)) return fail("can't unpack " + f.chd + " to " + (dir / DISK_FILE).string());

    // Run the recompiled boot ROM stub: it unpacks the game program into work RAM, then jumps to it.
    auto bus = std::make_unique<Bus>();
    std::memcpy(bus->rom.data(), f.boot.data(), ROM_BYTES);
    Ctx c;
    c.bus = bus.get();
    c.cop0[15] = 0x2020; // PRId: R4600
    register_funcs(stub, stub_count);
    try {
        run(&c, 0xbfc00000);
    } catch (const MissingFunc& m) {
        if (m.vaddr != 0x88000000) return fail("boot stub exited to unexpected address");
    } catch (const Unimplemented&) {
        return fail("boot stub hit an unimplemented instruction");
    }
    std::vector<std::pair<const void*, size_t>> parts = {{BOOT_MAGIC, sizeof BOOT_MAGIC}};
    for (auto [p, n] : boot_parts(c, *bus)) parts.push_back({p, n});
    if (!write_file(dir, BOOT_FILE, parts)) return fail("can't write " + (dir / BOOT_FILE).string());
    progress(100);
    std::printf("done: game data written to %s\n", fs::absolute(dir, ec).string().c_str());
    std::fflush(stdout);
    return true;
}

bool data_ready(const std::string& data_dir) {
    std::error_code ec;
    fs::path dir(data_dir);
    char magic[8] = {};
    std::ifstream(dir / BOOT_FILE, std::ios::binary).read(magic, 8);
    return std::memcmp(magic, BOOT_MAGIC, 8) == 0 && fs::file_size(dir / DISK_FILE, ec) == Ide::BYTES &&
           fs::file_size(dir / SOUND_FILE, ec) == SOUND_BYTES;
}

bool load_boot(const std::string& data_dir, Ctx& c, Bus& bus) {
    std::ifstream in(fs::path(data_dir) / BOOT_FILE, std::ios::binary);
    char magic[8] = {};
    in.read(magic, 8);
    if (std::memcmp(magic, BOOT_MAGIC, 8) != 0) return false;
    for (auto [p, n] : boot_parts(c, bus)) in.read(static_cast<char*>(p), std::streamsize(n));
    return bool(in);
}

} // namespace ki
