# Killer Instinct — native PC port

A native Windows and Linux port of the 1994 arcade **Killer Instinct** (Rare/Midway, v1.4), made by
**static recompilation**: the game's MIPS R4600 code and its ADSP-2105 sound code are translated to
C++ at build time and compiled into a normal executable. There is no CPU emulator loop; the
hardware around the CPU (framebuffers, inputs, IDE drive, DCS sound board) is a small
model based on MAME's `kinst` driver.

**No game data is included.** You need your own legal copy of the Killer Instinct v1.4 arcade ROM
set (`kinst.zip`) and hard drive image (`kinst.chd`), both to build and to play.

## Features

- Full game: attract mode, all fighters and stages, music and sound effects
- OpenGL renderer with effects: `sharp`, `scanlines`, `crt`, or your own `.glsl` shader (F9 cycles)
- Widescreen (~16:9) on the 3D stages, with real extra picture rather than stretching; other screens
  get black, blurred or mirrored side bars
- Smooth frame pacing locked to a 60 Hz display
- Fullscreen (Alt+Enter / F11), keyboard and gamepad remapping
- Operator settings, audits and high scores are saved between runs

## Building

You need the ROM set at build time because the code is generated from it. Put `kinst.zip` and
`kinst.chd` in `ki-rom/`, and extract the zip into that same folder (`u98-l14`, `u10-l1` …
`u36-l1`). The `ki-rom/` folder is gitignored, so the ROMs never get committed.

### Windows

Requires Visual Studio 2022 or later (Desktop C++ workload), which includes CMake and Ninja. From
a *x64 Native Tools* command prompt:

```
cmake -S . -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel
ctest --test-dir build-rel
```

SDL2 and libchdr are downloaded automatically by CMake.

### Linux (via Docker)

```
docker build -t kinst-linux tools/linux
docker run --rm -v "$PWD:/src" -v kinst-linux-build:/build kinst-linux
```

This builds in Ubuntu 22.04, runs the tests and writes `build-linux/kinst`. The binary only needs
glibc 2.35 or newer (Ubuntu 22.04+, SteamOS 3.5+, other distros from 2022 on).

## Playing

The first time, install the game data from your ROM set (a folder holding `kinst.zip` and
`kinst.chd`; zips and subfolders are searched):

```
kinst --install <folder with your ROMs>
```

This writes a `data/` folder next to the executable. After that, just run `kinst`. The ROMs are no
longer needed.

The default settings need **two credits** to start a game.

| Action | Player 1 | Player 2 |
|---|---|---|
| Move | Arrow keys | Numpad 8/4/6/2 |
| Quick / Medium / Fierce punch | Q / W / E | Numpad 7 / 9 / `/` |
| Quick / Medium / Fierce kick | A / S / D | Numpad 1 / 3 / `*` |
| Start | 1 | 2 |
| Coin | 5 | 6 |

The first two gamepads also work (punches X / Y / RB, kicks A / B / RT, Back = coin). Other keys: F2 opens test (service) mode, F9 cycles effects,
Alt+Enter or F11 toggles fullscreen, and `=` / `-` change the volume.

## Settings

Copy `kinst.ini.example` to `kinst.ini` next to the executable. It documents every option:
renderer, effects, widescreen and its side-bar fill, fullscreen, frame pacing, FPS counter and
key/button remapping. Command-line overrides: `--renderer`, `--fx`, `--widescreen`, `--config`,
`--data`.

To restore factory operator settings and high scores, delete `data/disk.img.changes`.

## How it works

`CLAUDE.md` is the detailed technical log: the boot chain, the recompilers (`tools/kirecomp.cpp`
for MIPS, `tools/adsprecomp.cpp` for the DSP), the hardware model, and the widescreen patches
(`tools/widescreen.patches`).

## License

The source code is under the BSD 3-Clause license (see `LICENSE`). Parts are ported from MAME,
which is under the same license. Killer Instinct itself is the property of its owners. This
project is not affiliated with or endorsed by them.
