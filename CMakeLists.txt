cmake_minimum_required(VERSION 3.20)
project(ki_native_port CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-2.30.9
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(SDL2)

# libchdr reads the MAME CHD hard disk image (kinst.chd is CHD v3, zlib).
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CHDR_WANT_TESTS OFF CACHE BOOL "" FORCE)
# The bundled miniz also reads the user's kinst.zip during `kinst --install`.
set(MINIZ_ARCHIVE_APIS ON CACHE BOOL "" FORCE)
set(MINIZ_STDIO ON CACHE BOOL "" FORCE)
FetchContent_Declare(libchdr
    GIT_REPOSITORY https://github.com/rtissera/libchdr.git
    GIT_TAG 47d794151b2b64f6aba60304e6480f4f5a2bc24b)
FetchContent_MakeAvailable(libchdr)

add_library(kicore STATIC src/memmap.cpp src/runtime.cpp src/ide.cpp src/dcs.cpp)
target_include_directories(kicore PUBLIC src)

# Static recompiler (host tool), run at build time against the user's ROM.
add_executable(kirecomp tools/kirecomp.cpp)

set(KI_BOOT_ROM ${CMAKE_SOURCE_DIR}/ki-rom/u98-l14 CACHE FILEPATH "Killer Instinct v1.4 boot ROM (u98)")
set(GEN ${CMAKE_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${GEN})
# Decompressor stub: ROM 0xbfc00000-0xbfc00e48 (the rest of the ROM is its compressed payload).
add_custom_command(
    OUTPUT ${GEN}/boot_stub.cpp
    COMMAND kirecomp ${KI_BOOT_ROM} 0 bfc00000 bfc00e48 ${GEN}/boot_stub.cpp boot_stub bfc00000
    DEPENDS kirecomp ${KI_BOOT_ROM})

# Stage 2: run the recompiled stub to unpack the boot program into work RAM, then recompile it.
add_executable(kiunpack tools/kiunpack.cpp ${GEN}/boot_stub.cpp)
target_link_libraries(kiunpack PRIVATE kicore)
add_custom_command(
    OUTPUT ${GEN}/boot_main.bin
    COMMAND kiunpack ${KI_BOOT_ROM} ${GEN}/boot_main.bin
    DEPENDS kiunpack ${KI_BOOT_ROM})
add_custom_command(
    OUTPUT ${GEN}/boot_main.cpp
    COMMAND kirecomp ${GEN}/boot_main.bin 0 88000000 88033000 ${GEN}/boot_main.cpp boot_main
            --patches ${CMAKE_SOURCE_DIR}/tools/widescreen.patches 88000000 88026344 88026388
    DEPENDS kirecomp ${GEN}/boot_main.bin ${CMAKE_SOURCE_DIR}/tools/widescreen.patches)

# DCS sound board: every ADSP-2105 code image is recompiled straight from sound ROM u10 — the
# bank-0 loader page, the bank-1 OS page it reboots into, and the two overlays the OS loads.
add_executable(adsprecomp tools/adsprecomp.cpp)
set(KI_SOUND_ROMS u10-l1 u11-l1 u12-l1 u13-l1 u33-l1 u34-l1 u35-l1 u36-l1)
list(TRANSFORM KI_SOUND_ROMS PREPEND ${CMAKE_SOURCE_DIR}/ki-rom/)
list(GET KI_SOUND_ROMS 0 KI_SOUND_U10)
add_custom_command(
    OUTPUT ${GEN}/dcs_boot.cpp
    COMMAND adsprecomp boot0 ${KI_SOUND_U10} ${GEN}/dcs_boot.cpp dcs_boot
    DEPENDS adsprecomp ${KI_SOUND_U10})
# The bank-0 loader reboots the DSP (SYSCONTROL bit 9) into the sound OS on bank 1's boot page.
add_custom_command(
    OUTPUT ${GEN}/dcs_os.cpp
    COMMAND adsprecomp boot1 ${KI_SOUND_U10} ${GEN}/dcs_os.cpp dcs_os
    DEPENDS adsprecomp ${KI_SOUND_U10})
# The OS's loader (0x03e8) copies two overlays from ROM to external PM 0x0800 at init:
# A = bank 0 +0x800, 0x200 words (runs once), B = bank 2 +0, 0x300 words (stays resident).
add_custom_command(
    OUTPUT ${GEN}/dcs_ovl_a.cpp
    COMMAND adsprecomp ovl:0:800:200:800 ${KI_SOUND_U10} ${GEN}/dcs_ovl_a.cpp dcs_ovl_a
    DEPENDS adsprecomp ${KI_SOUND_U10})
add_custom_command(
    OUTPUT ${GEN}/dcs_ovl_b.cpp
    COMMAND adsprecomp ovl:2:0:300:800 ${KI_SOUND_U10} ${GEN}/dcs_ovl_b.cpp dcs_ovl_b
    DEPENDS adsprecomp ${KI_SOUND_U10})

# Video backends, selected by kinst.ini / --renderer (src/Renderer.hpp).
set(RENDERERS src/Renderer.cpp src/SdlRenderer.cpp src/NativeRenderer.cpp)

# The shipped exe needs no ROMs at build time beyond the code above: on first run
# `kinst --install <rom folder>` writes data/ (src/install.cpp) and the game runs from that.
add_executable(kinst src/main.cpp src/install.cpp ${RENDERERS} ${GEN}/boot_stub.cpp ${GEN}/boot_main.cpp ${GEN}/dcs_boot.cpp ${GEN}/dcs_os.cpp ${GEN}/dcs_ovl_a.cpp ${GEN}/dcs_ovl_b.cpp)
if(MSVC)
    set_source_files_properties(${GEN}/boot_main.cpp PROPERTIES COMPILE_OPTIONS /bigobj)
endif()
target_link_libraries(kinst PRIVATE kicore SDL2::SDL2-static chdr-static miniz)
target_include_directories(kinst PRIVATE ${libchdr_SOURCE_DIR}/deps/miniz-3.1.2)

enable_testing()
add_executable(memmap_test tests/memmap_test.cpp)
target_link_libraries(memmap_test PRIVATE kicore)
add_test(NAME memmap COMMAND memmap_test)

# Drive writes (settings, audits, high scores) persist in <image>.changes across runs.
add_executable(ide_test tests/ide_test.cpp)
target_link_libraries(ide_test PRIVATE kicore)
add_test(NAME ide COMMAND ide_test WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# First-run install from the user's ROM set (kinst.zip contents + kinst.chd in ki-rom/), then boot
# from the generated data headless, insert two coins, press start and play into the first match
# (KI_INPUT = ms:port:hexbits): must never leave recompiled MIPS or DSP code, and the DCS must
# produce audio.
set(KI_ROM_DIR ${CMAKE_SOURCE_DIR}/ki-rom CACHE PATH "Folder holding the user's Killer Instinct ROM set")
add_test(NAME install COMMAND kinst --install ${KI_ROM_DIR} --data ${CMAKE_BINARY_DIR}/data)
set_tests_properties(install PROPERTIES FIXTURES_SETUP game_data PASS_REGULAR_EXPRESSION "progress 100" TIMEOUT 300)
add_test(NAME boot_smoke COMMAND ${CMAKE_COMMAND} -E env KI_HEADLESS=30
    "KI_INPUT=6000:1:800,7000:1:800,10000:1:400,20000:1:1,22000:1:8,24000:1:20"
    $<TARGET_FILE:kinst> --data ${CMAKE_BINARY_DIR}/data)
set_tests_properties(boot_smoke PROPERTIES
    FIXTURES_REQUIRED game_data
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    PASS_REGULAR_EXPRESSION "last function entered"
    FAIL_REGULAR_EXPRESSION "unrecompiled|unimplemented|crash|no recompiled DSP code|rms 0[.]0|not found"
    TIMEOUT 90)
# Same run in widescreen (tools/widescreen.patches active): must stay in recompiled code and the
# widened rasterizers must only ever draw into the framebuffers. The stage is random, so this
# doesn't always reach a 3D stage.
add_test(NAME widescreen_smoke COMMAND ${CMAKE_COMMAND} -E env KI_HEADLESS=30
    "KI_INPUT=6000:1:800,7000:1:800,10000:1:400,20000:1:1,22000:1:8,24000:1:20"
    $<TARGET_FILE:kinst> --data ${CMAKE_BINARY_DIR}/data --widescreen)
set_tests_properties(widescreen_smoke PROPERTIES
    FIXTURES_REQUIRED game_data
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    PASS_REGULAR_EXPRESSION "widescreen: 0 rasterizer calls on other targets"
    FAIL_REGULAR_EXPRESSION "unrecompiled|unimplemented|crash|no recompiled DSP code|rms 0[.]0|not found"
    TIMEOUT 90)

# Renderers: every registered one must start; the OpenGL one draws a BGR555 frame through a hidden
# window and reads the pixels back. Needs a desktop GL driver.
add_executable(renderer_test tests/renderer_test.cpp ${RENDERERS})
target_link_libraries(renderer_test PRIVATE SDL2::SDL2-static)
add_test(NAME renderer COMMAND renderer_test)

# Button remapping from kinst.ini: keys replace an action's defaults, PlayStation names map to SDL's,
# pad_ applies to both controllers and p2_pad_ overrides controller 2 only.
add_test(NAME controls COMMAND kinst --config ${CMAKE_SOURCE_DIR}/tests/controls_test.ini --list-controls)
set_tests_properties(controls PROPERTIES PASS_REGULAR_EXPRESSION
    "p1_up = Up\n.*p1_high_quick = J, K\n.*p2_up = none\n.*p1_pad_high_quick = x\n.*p1_pad_high_fierce = b\n.*p1_pad_low_fierce = rightshoulder, lefttrigger\n.*p2_pad_high_quick = x, y\n.*p2_pad_high_fierce = b\n")
