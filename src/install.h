#pragma once
// First-run install: turns the user's own MAME ROM set (kinst.zip or loose ROMs + kinst.chd) into
// the data directory the game runs from, so the ROMs are never needed again:
//   boot.bin   work RAM + CPU state right after the boot ROM stub has unpacked the game program
//   disk.img   the hard drive as raw sectors (unpacked from the CHD)
//   sound.bin  the DCS sound ROM region (8 ROMs byte-interleaved, as ROM_LOAD16_BYTE builds it)
#include <string>
#include <vector>
#include "recomp.h"

namespace ki {

constexpr const char* BOOT_FILE = "boot.bin";
constexpr const char* DISK_FILE = "disk.img";
constexpr const char* SOUND_FILE = "sound.bin";

// sources: files or folders to search (searched recursively; ROMs are matched by CRC, so file
// names don't matter). Prints "progress N" and "error: ..." lines on stdout for a launcher.
bool install(const std::vector<std::string>& sources, const std::string& data_dir,
             const FuncEntry* boot_stub, size_t boot_stub_count);
bool data_ready(const std::string& data_dir);
bool load_boot(const std::string& data_dir, Ctx& c, Bus& bus);

} // namespace ki
