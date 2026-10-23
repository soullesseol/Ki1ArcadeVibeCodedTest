# Project Plan: Killer Instinct Arcade Hybrid Static Recompiler

Git repository (branch `main`). Not committed (`.gitignore`): `build*/`, `ki-rom/` (copyrighted ROMs), `mame-reference/`, `n64-recomp-reference/`, game data, dumps, `kinst.ini`.

## 1. Explicit Goal
The objective of this project is to build a native PC port (`.exe`) of the original *Killer Instinct* arcade version (Rare/Midway, 1994). This is **not** Wolf Unit hardware (`midwunit.cpp` is the TMS34010-based MK3/UMK3 board); KI runs on its own board driven by `mame-reference/src/mame/rare/kinst.cpp`.

To streamline development, we are targeting a **Hybrid Port Architecture**. You will use static recompilation to translate the game's 64-bit MIPS (R4600) CPU logic into native C++ code, and hook that recompiled logic into a hardware abstraction layer that mirrors the pathways used in MAME's `kinst.cpp`.

---

## 2. Hardware Facts (from `kinst.cpp`)
* **CPU:** MIPS R4600 @ 100MHz, **little-endian** (boot ROM word 0 = `0x0BF000E2` LE = `j 0xBFC00388`). 16KB I/D caches.
* **Code location:** The boot EPROM (`u98`, 512KB) only boots and loads the game; **the main game code and graphics live on the IDE hard drive** (`kinst.chd`, Seagate ST9150AG, C/H/S 419/13/47). The boot ROM checks the drive model string.
* **Memory map (physical; kseg0 `0x8xxxxxxx` and kseg1 `0xAxxxxxxx` alias it):**

  | Range | What |
  |---|---|
  | `0x00000000–0x0007FFFF` | 512KB RAM, contains both framebuffers |
  | `0x08000000–0x087FFFFF` | 8MB work RAM |
  | `0x10000080` | r: P1 inputs / w: vram control (bit 2 = framebuffer select) |
  | `0x10000088` | r: P2 inputs / w: DCS sound reset (bit 0) |
  | `0x10000090` | r: volume/service / w: sound control (rising bit 1 latches data) |
  | `0x10000098` | w: sound data |
  | `0x100000A0` | r: DIP switches |
  | `0x100000B0` | w: coin counter |
  | `0x10000100–0x1000013F` | IDE CS0 (ATA command block, reg = offset/8) |
  | `0x10000170` | IDE CS1 reg 6 (alt status / device control) |
  | `0x1FC00000–0x1FC7FFFF` | Boot EPROM, reset vector `0xBFC00000` |

  Unmapped reads return all-ones. KI2 shuffles the `0x100000xx` registers (`kinst2_map`).
* **Video:** No sprite/tile hardware. A plain 320×240 BGR555 framebuffer (R in bits 0–4, 640-byte rows) in RAM at `0x30000` or `0x58000` (selected by vram control bit 2). VBLANK ≈ 58.96Hz (`50MHz/8`, 406×261 total) drives CPU IRQ 0; IDE drives IRQ 1.
* **Audio:** Midway DCS (ADSP-2105) with sound ROMs `u10–u13`, `u33–u36` (loaded 16-bit byte-interleaved).

---

## 3. Technical Stack
* **Target Architecture:** Native Windows/Linux (x86_64)
* **Primary Language:** C++20
* **Build System:** CMake (SDL2 fetched via FetchContent)
* **Multimedia Framework:** SDL2 (Windowing, Input, 2D framebuffer upload via `SDL_PIXELFORMAT_XBGR1555`)
* **Graphics API:** SDL_Renderer (D3D11/OpenGL backend); raw OpenGL only if shaders become necessary
* **Audio API:** SDL audio (DCS output is a single mono stream)
* **Reference Source Materials (Located in Project Root):**
  * `./mame-reference/`: Full MAME source. Primary driver: `src/mame/rare/kinst.cpp`; sound: `src/mame/shared/dcs.cpp`; drive: `src/devices/bus/ata/`.
  * `./n64-recomp-reference/`: N64Recomp repository, the MIPS-to-C++ blueprint. It assumes big-endian N64 — byte/half/unaligned (`lwl/lwr`) access semantics must be flipped for KI.
  * `./ki-rom/`: `kinst.zip` (boot ROM `u98-l14` = v1.4, CRC `afedb75f`, plus sound ROMs) and `kinst.chd` hard drive image. The build and `kinst` read the ROMs **extracted** next to the zip (`u98-l14`, `u10-l1` … `u36-l1`). The build reads them to generate code; the shipped exe never needs them after first run (see Install below).

---

## 4. Core Development Principles & Constraints
1. **Incremental Compilation:** Do not attempt to recompile everything at once. Start with the boot ROM, ensuring memory maps hook up correctly, then the code it loads from the CHD.
2. **Abstracted Hardware:** All guest memory access goes through the bus (`src/memmap.h`). Video is presented by uploading the selected framebuffer each frame; IO register writes are intercepted on the bus.
3. **No MIPS Interpreter Emulation:** The ultimate goal is static recompilation. CPU instructions should be transformed into native C++ functions, not run inside an instruction loop.

---

## 5. Phased Execution Roadmap

### Phase 1: Environment and Memory Mapping (done)
* CMake setup linking SDL2.
* SDL2 window at 4:3 (320×240 ×3) presenting the guest framebuffer.
* Virtual memory map from `kinst_map` (`src/memmap.h`, tested by `tests/memmap_test.cpp`).

### Phase 2: Parser Modification (MIPS-to-C++)
* ~~Write a C++ recompiler for **little-endian** MIPS III~~ — done: `tools/kirecomp.cpp` (own decoder, no symbols; N64Recomp-style output). Runtime in `src/recomp.h` / `src/runtime.cpp`.
* Boot chain (all generated at build time into `build/gen/`):
  1. ROM `0xBFC00000–0xBFC00E48` is a decompressor stub → `boot_stub.cpp`.
  2. `kiunpack` runs the recompiled stub; it unpacks the boot program (~530KB) to `0x88000000` → `boot_main.bin` → `boot_main.cpp`.
  3. `kinst` runs both natively from reset on a guest thread; the SDL window presents the framebuffer.
* The "boot program" in u98 **is the game engine** (824 functions); the CHD holds assets only. Attract mode (title, character bios) runs fully recompiled.
* Control-flow quirks handled: branch in delay slot, `jr $aN` returns (saved ra), non-local `jr` to an ancestor's return address (`Unwind`), pointer harvesting for dispatch tables, in-function `switch` for computed jumps into unrolled loops.
* TLB: the sprite renderer reads each character's palette through a TLB mapping at virtual 0 (`lh rX, 0(pixel byte)`, 0x88010b94; mapped by `tlbwi` in 0x8800b044). `Bus::translate` applies `tlbwi` entries to kuseg/kseg2 data accesses; without it fighters render dark blue/green.
* Hardware for Phase 2: IDE drive over the CHD (`src/ide.cpp`, libchdr; writes persist in `data/disk.img.changes`, see below), live COP0 Cause (VBLANK = IP2, IDE = IP3) and Count (50MHz) from wall-clock time.
* Debugging: `KI_HEADLESS=<sec>` runs without a window and dumps RAM + registers; `KI_IDE_TRACE=1` logs drive commands; crashes print the last 64 functions entered.
* Tests: `memmap` (bus + unaligned LE helpers) and `boot_smoke` (20s headless boot must stay in recompiled code).

### Install / distribution (no ROMs after first run)
* The exe contains all recompiled code (MIPS + DSP). Game **data** comes from the user's ROM set once:
  `kinst --install <folder or files...> [--data DIR]` scans recursively (loose files and any `.zip`, matched by CRC; `.chd` by raw-data SHA1 `a37a2c5e...`), checks it's v1.4 (`u98` CRC `afedb75f`), and writes `DIR` (default `data/` next to the exe): `boot.bin` (RAM + CPU state after running the recompiled boot stub), `disk.img` (raw 131MB drive), `sound.bin` (DCS region). Prints `progress N` / `error: ...` / `done: ...` on stdout; exit 0 = success. ~4s.
* Windows: `kinst.exe` is a GUI-subsystem app (`WIN32_EXECUTABLE`, `/ENTRY:mainCRTStartup` keeps `main`), so no console window opens. Pipes (launcher, ctest) get stdout as usual; started from a terminal without redirection it attaches to that console (the prompt returns before the output, as with any GUI exe).
* `kinst --check [--data DIR]`: exit 0 when data is ready (for a launcher's Play button). `kinst [--data DIR]`: play; starts at `0x88000000` from `boot.bin` and never opens a ROM (verified: game code never reads the boot ROM after the stub).
* Code: `src/install.cpp` (miniz from libchdr's deps, with `MINIZ_ARCHIVE_APIS`, reads zips). Tests: `install` fixture builds `build*/data` from `ki-rom/`, then `boot_smoke` plays from it.
* ponytail: the service-mode ROM test (if it checksums the boot ROM) would fail, since the ROM isn't mapped at runtime.

### Phase 3: Video/Audio/Interrupt Bridge
* VBLANK speed fix: the main loop (`0x88001408`) waits on the VBLANK *level*; natively a frame can finish inside the 1.36ms VBLANK and run again (fights ran ~1.65x fast, ~98 flips/s). A framebuffer flip during VBLANK now ends that VBLANK for the game (`Bus::flip_frame`, `in_vblank`): 59 flips/s, matching the board.
* ~~Interrupts~~ — done. The game uses only the COP0 Count/Compare timer (IM7, 60Hz, handler `0x88026388` via the stub it copies from `0x88026344` to `0x80000180`): coin debounce, credit and clock counters. VBLANK/IDE are polled via Cause. Recompiled code checks `POLL(c)` at function entries and loop back-edges; a 1kHz host thread sets the flag. `eret` returns natively.
* ~~Input~~ — done. Keyboard (P1: arrows, QWE high / ASD low, 1 start, 5 coin, F2 test; P2: numpad, 2 start, 6 coin), cabinet volume `=`/`-` (the game steps the DCS volume itself), and first two SDL gamepads. Default settings need **2 credits** to start. Remapping: `kinst.ini` `p1_<action>`/`p2_<action>` = SDL key names, `pad_<action>` = SDL controller names (both pads, then `p1_pad_`/`p2_pad_` override one; Xbox and PlayStation aliases like `cross`, `r1`, `l2`, triggers allowed); comma lists, `none` unbinds, a set action replaces its defaults. `kinst --list-controls` prints the bindings in effect as ini lines; `--config FILE` picks another ini. Reference: `kinst.ini.example`. Test: `controls`. The game ignores switches held ≥12 timer ticks (stuck-switch check), so scripted presses are 100ms.
* Coin → start → versus → fight → continue → attract all run recompiled (`boot_smoke` plays into the first match).
* Build both `build/` (Debug) and `build-rel/` (Release); timing is wall-clock so both run full speed.
* Linux build (Docker, no local toolchain needed): `docker build -t kinst-linux tools/linux` then `docker run --rm -v "<repo>:/src" -v kinst-linux-build:/build kinst-linux` -> builds Release in the volume, runs all tests (OpenGL on Xvfb + Mesa, `SDL_AUDIODRIVER=dummy`), copies the binary to `build-linux/kinst`. Ubuntu 22.04 + gcc 13 (toolchain PPA), libstdc++/libgcc static: needs only glibc >= 2.35 (SDL dlopens X11/Wayland/GL/ALSA/Pulse at runtime). Linux crash report = SIGSEGV/SIGBUS handler on an alternate stack (guest thread). `Bus::vsync` uses a mutex, not `std::atomic<Vsync>` (would need libatomic).
* ~~DCS sound~~ — done, **statically recompiled** (user's choice over porting MAME's interpreter):
  * `tools/adsprecomp.cpp`: ADSP-2105 → C++. One state-machine function per code image; every instruction is a label; jumps are gotos; calls/RTS/RTI/DO-UNTIL/interrupts use the real guest PC/loop/status stacks (the DSP code manipulates them). Data-path ops are decoded at recompile time into direct calls to helpers in `src/adsp.h`, ported from MAME `2100ops.hxx` (bit-exact; note PX is 8-bit).
  * `src/dcs.cpp`: board model (`dcs_2k` maps, ROM banking, host latch with auto-ack/IRQ2, SPORT1 autobuffer → 31.25kHz ring + IRQ1, ADSP timer). Runs on its own thread; host side only touches atomics. SDL plays the ring.
  * Code images, all compiled straight from sound ROM u10 at build time: `dcs_boot` (bank-0 loader page), `dcs_os` (bank-1 page it reboots into via SYSCONTROL bit 9), `dcs_ovl_a`/`dcs_ovl_b` (overlays the OS loader at 0x03e8 copies to PM 0x0800, 3 bytes/word). The board picks the image compiled from the word PM holds at the target pc; words that changed since compilation are "dirty" and code exits when it reaches one (self-modifying-code safety). Runtime-patched interrupt vectors (single jump/RTI) are resolved as trampolines.
  * MIPS side: `0x88` sound reset, `0x98` data + rising bit 1 of `0x90` latches it, `0x90` read bit 1 = latch empty. That status read yields while the latch is full: the game's send loop has an iteration-count timeout that natively expires in ~2ms and resets the sound board.
  * Debug: `KI_DCS_TRACE=1` (resets, reboots, host bytes, autobuffer), `KI_DCS_TRACE_RANGE=lo-hi` (per-instruction register trace), `KI_WAV=file.wav` (headless audio capture).
  * Levels verified against MAME 0.120 `-wavwrite` (`D:\Disco160_B\mame + KI`): attract music really is quiet (rms ~300, peaks ~1000), same as ours.
  * Known: occasionally the first seconds of attract audio are silent (timing-dependent around DSP startup); everything after recovers.
* Drive writes (operator settings, audits, high scores; the game writes sectors 12-18) persist in `<data>/disk.img.changes`: a log of (lba u32, 512 bytes) records, replayed and compacted (temp file + rename) by `Ide::open`, appended + flushed on every sector write. `disk.img` stays as installed; delete the log for factory settings. `--install` doesn't touch it. Test: `ide`.
* Debug: `KI_SHOTS=1` (headless) dumps the displayed picture to `fb_NNN.bin` once a second (320x240 BGR555, 432x240 with widescreen); `KI_INPUT` port 3 = VOLUME port.

### Phase 4: Native renderer (in progress)
* Renderers are pluggable: `src/Renderer.hpp` (interface: `init`, `draw(Frame)`, `window`) + table in `src/Renderer.cpp`. A new backend = subclass in its own file + one table line + add it to `RENDERERS` in CMakeLists. Selected by `kinst.ini` next to the exe (`renderer = opengl|sdl`; `key = value`, `#`/`;` comments) or `--renderer NAME`; `--list-renderers` prints `name<TAB>description`. Unknown or failing renderer falls back to `sdl`. Default is the first table entry (`opengl`).
  * `opengl` (`src/NativeRenderer.*`): SDL2 + OpenGL 3.3 core (Windows, Linux, Steam Deck); GL entry points via `SDL_GL_GetProcAddress`, no opengl32/libGL link. Uploads BGR555 as `GL_RGBA`/`UNSIGNED_SHORT_1_5_5_5_REV` (bit 15 ignored), letterboxed 4:3, nearest.
  * `sdl` (`src/SdlRenderer.*`): the original SDL_Renderer XBGR1555 streaming-texture blit.
  * Test `renderer`: GL pixel readback, then every registered renderer must init and draw in a hidden window.
* Fullscreen: `kinst.ini` `fullscreen = on`, Alt+Enter / F11 toggle (desktop fullscreen, cursor hidden).
* Frame pacing (`frame_pacing = display`, default; `original` = board clock): the presenter measures its swap interval; if it (or every k-th) is 55-62Hz it reports each refresh with `Bus::display_vsync`, and `Bus::frame_clock` then counts those refreshes (VBLANK = last 8% before each), so each game frame is shown exactly once. Reports older than 100ms (window dragged, no vsync, headless) fall back to wall clock x 58.96Hz. Measured in a fight at 60Hz: 0 repeated frames vs ~1/s with `original`. Game runs 60/58.96 = 1.7% fast.
* Window title shows the renderer in use (`Killer Instinct [opengl]`). `kinst.ini` `show_fps = on|off|1|0`: yellow game-FPS counter, top right (3x5 font `stamp_text` in `Renderer.cpp`, stamped into a copy of the frame so every renderer shows it). It counts framebuffer flips (`Bus::flips`), not host refreshes; headless runs print `video: N frames`.
* Effects: `kinst.ini` `fx = none|sharp|scanlines|crt|FILE.glsl` (or `--fx`), F9 cycles presets, title shows it (`[opengl, crt]`). `Renderer::set_fx`/`fx_presets` (base accepts only `none`). In `NativeRenderer.cpp`: each effect is a single-pass fragment shader with `FX_HEADER` prepended (`uv`, `layer` bilinear, `source_size`, `output_size`, `pixel()` nearest); a failed compile keeps the previous effect and reports the GLSL log. Multi-pass (xBR etc.) would need render-to-texture. `renderer` test compiles every preset, runs and rejects a `.glsl`.
* Widescreen (`kinst.ini` `widescreen = on`, or `--widescreen`; default off): 432x240 (~16:9, M = 56 margin columns each side). Real extra picture on the **3D stages** (Cinder, Orchid's rooftop, ... incl. the round fly-in); pre-rendered stages, menus and attract stay 4:3 with black bars. Off = the original code path (every patch is `if (widescreen) alt else original`).
  * Design: one wide buffer per framebuffer (`Bus::wide`, physical `WS_BASE` = 0x400000, 1024-byte rows). View A = the game's FB0/FB1 addresses (640-byte rows): `Bus::host_ptr` redirects them into the centre, so unpatched drawing (HUD, text, pre-rendered stages) works unchanged. View B = the wide buffer itself; patched routines draw there with x = -56..375. `Bus::margins_drawn[buf]` (set by any view-B access, cleared at the flip that makes it the back buffer) decides whether the margins show or stay black (`Bus::screen`, `screen_frame` in main.cpp). Renderers letterbox to the frame's own aspect.
  * Patches: `tools/widescreen.patches` (kirecomp `--patches`; kinds `word` = alternative instruction, `code` = C++ instead, `after` = original then C++; plain instructions only; unused/branch addresses are build errors). Helpers in `recomp.h` (`ws_row0`, `ws_origin`, `ws_raster_enter`, `ws_sprite_enter`).
    * Background layers `0x8801854c` (list at `[0x8808de3c]`: `{type, y, rows, width, parallax divisor, data}`): type 0 panoramas (960 px wide, wrapping) via blitter `0x880189b8(a0 src x, a1 y, a2 width, a3 rows, v1 src, v0 fb)` -> 432 columns from src x - 56; type 1 solid fill `0x880188d8`; black-above-first-layer `0x88018954`.
    * Polygons (3D floor/objects, BSP walk `0x88014990`, vertices projected by `0x88019c64` to 16.16 screen x/y around centre `[0x88035e48]` = 160): triangle families A `0x88019fa0/a044/a0e8`, B `0x8801ade0/ae84`, C `0x8801bbd0/bc74/bd18` (6 span routines each, via `[0x88035e50]`/`[0x88035e80]`/`[0x88035ea0]`) and line drawer `0x8801ca60`. They clip x with sign tests, so on entry (after they save all registers) x += 56 and v0 -> wide row 0; limits 320 -> 432 (0x140/0x13f/0x27f/0x27e), strides 0x280 / y<<7 -> 1024. Patch lines generated from those rules (gen script was a scratch tool; the rules are in the file's comments). A widened call with a non-framebuffer target is counted (`ws_foreign_targets`, printed headless) — so far always 0.
    * Full-screen colour passes (linear 0x4b00-dword sweeps of the back buffer): darken `0x88001530..` (the dim/red blink before a finisher), brighten/fade `0x880015dc`, colour flash `0x880186b4`, white fill `0x88018668`. On frames with drawn margins they sweep the whole wide buffer.
    * Sprite blitter `0x880104e8` (args: s2 = row pitch, sign = flip; k0..k1 x clip; gp..fp y clip; computes the back buffer itself from byte `[0x88086200]`): on frames with drawn margins, calls clipped to the full screen are widened at entry (pitch +-0x400, clip -56..376) and start at the wide origin. Covers fighters, stage objects (`0x88015ff8`), 3D-scene sprites.
  * Not widened (pillarboxed), and can't be: pre-rendered stages (gym, Riptor, Sabrewulf, Jago, Thunder, Glacius...). Decoder `0x8800cb00 -> 0x88030320` (k1 = camera x / table[state], k0 = back buffer): the image header at `[0x885b1004]` (+8 = 99 big-endian) is the number of **camera steps**, not columns. Each 8-px camera step has its own separately encoded 320-px frame (`[0x885b1008 + 4*step]`; bitstream decoded per block row by `0x880308ec` into tiles at `0x8002ce00`, exactly 80 x 4 px), and the fraction k1&7 shifts columns by a per-column depth table (s7 bytes). So pixels outside the 320 columns were never rendered. Tried decoding the neighbouring camera steps into the margins: the perspective differs, so seams and repeated scenery (removed). Options left: black bars (current) or a fake fill (blurred/mirrored edges) in the presenter.
  * `widescreen_fill = black|blur|mirror` (kinst.ini): what the empty bars show (`fill_margins` in Renderer.cpp, applied in `screen_frame` when no view-B drawing happened that frame; blur = mirrored edge, 2x two-pass 17-tap box blur, half brightness; mirror = 5/8 brightness). Checked in the `renderer` test.
  * Test: `widescreen_smoke` (boot_smoke with `--widescreen`; stage is random). Verified by screenshots on Cinder and Orchid stages, fly-in, and that off-mode output is unchanged.
  * ponytail: width is fixed at 432 (constants in the patch file and `WS_W/WS_M`); 384 (16:10, Steam Deck) would need a second set of patch constants.
* The game renders in software. Draw paths found by profiling framebuffer writes: `0x8800c2fc` full-screen RLE background decoder (240 rows; called from `0x8802b740`/`0x8802be80`); polygon rasterizer `0x88014990`→`0x88014a40`→`0x88014b2c` (quad records with 4 vertex pointers at +16..+28, span routines via `0x8801c1a4`, blend helpers `0x88010ac0..b0c`); text/HUD `0x8802286c`; 8×8 block writer `0x8803052c`. These are the hook points for per-layer GPU drawing and texture mods.

### Future features (not started)
* **Higher internal resolution / upscaling:** little gain from rendering at 2x — sprites and backgrounds are fixed-resolution bitmaps drawn 1:1, only the polygon floor would get sharper. Better: multi-pass upscale shaders (xBR, ScaleFX) — the `fx` system is single-pass, needs render-to-texture passes.
* **HD replacement packs:** dump each sprite/background bitmap from the hooked draw routines (sprite blitter `0x880104e8`, panorama blitter `0x880189b8`, block decoder `0x8803052c`), upscale offline (AI or by hand), and draw the replacements on the GPU. Biggest job; shares the hooks with widescreen.
* **Widescreen 16:10 width** (Steam Deck).
