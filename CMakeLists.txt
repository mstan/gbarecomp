cmake_minimum_required(VERSION 3.20)
project(gbarecomp C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W3 /permissive- /Zc:__cplusplus)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# SDL2 — soft dependency. When present we build host_window.cpp with the
# real SDL backend; otherwise it compiles as a no-op stub. CI and headless
# environments stay buildable either way.
#
# Avoid `find_package(SDL2 CONFIG)` on MSYS2: that target's
# INTERFACE_INCLUDE_DIRECTORIES is `-isystem /mingw64/include`, which
# breaks libstdc++'s `#include_next <stdlib.h>` chain.
# Avoid `C:/...` HINTS too: CMake-on-MSYS records them but mangles the
# emitted -I flag with the build dir prefix.
# ─────────────────────────────────────────────────────────────────────────────
set(GBARECOMP_SDL2_OK FALSE)
set(GBARECOMP_MINGW_PREFIX_UNIX "/c/msys64/mingw64" CACHE PATH
    "MSYS2 mingw64 install root, in Unix form, used for finding SDL2")
find_path(SDL2_INCLUDE_DIR
    NAMES SDL.h
    PATH_SUFFIXES SDL2 SDL2/include
    HINTS ${GBARECOMP_MINGW_PREFIX_UNIX}/include
)
find_library(SDL2_LIBRARY
    NAMES SDL2
    HINTS ${GBARECOMP_MINGW_PREFIX_UNIX}/lib
)
if(SDL2_INCLUDE_DIR AND SDL2_LIBRARY)
    set(GBARECOMP_SDL2_OK TRUE)
    set(SDL2_INCLUDE_DIRS ${SDL2_INCLUDE_DIR})
    set(SDL2_LIBRARIES   ${SDL2_LIBRARY})
    message(STATUS "gbarecomp: SDL2 found — inc=${SDL2_INCLUDE_DIR} lib=${SDL2_LIBRARY}")
else()
    message(STATUS "gbarecomp: SDL2 NOT found — host_window will stub out (inc=${SDL2_INCLUDE_DIR} lib=${SDL2_LIBRARY})")
endif()

# ─────────────────────────────────────────────────────────────────────────────
# armv4t — portable ARM7TDMI decoder, IR, codegen. Reusable for ARM9 later.
# Must NOT depend on src/gba or src/runtime.
# ─────────────────────────────────────────────────────────────────────────────
add_library(gbarecomp_armv4t STATIC
    src/armv4t/arm_decode.cpp
    src/armv4t/thumb_decode.cpp
    src/armv4t/arm_ir.cpp
    src/armv4t/arm_codegen.cpp
    src/armv4t/runtime_arm.cpp
    src/armv4t/interpreter.cpp
)
target_include_directories(gbarecomp_armv4t PUBLIC src/armv4t)
set_property(TARGET gbarecomp_armv4t PROPERTY POSITION_INDEPENDENT_CODE ON)

# ─────────────────────────────────────────────────────────────────────────────
# gba — GBA-specific bus, IO, IRQ, DMA, timers, PPU, audio, input, save, BIOS.
# Aggressively GBA-specific. Depends on armv4t for CPU types.
# ─────────────────────────────────────────────────────────────────────────────
add_library(gbarecomp_gba STATIC
    src/gba/gba_memory.cpp
    src/gba/gba_bus.cpp
    src/gba/gba_io.cpp
    src/gba/gba_irq.cpp
    src/gba/gba_dma.cpp
    src/gba/gba_timers.cpp
    src/gba/gba_ppu.cpp
    src/gba/gba_audio.cpp
    src/gba/gba_input.cpp
    src/gba/gba_save.cpp
    src/gba/gba_bios.cpp
    src/gba/gba_scheduler.cpp
    src/gba/gba_rom_header.cpp
    src/gba/sha1.cpp
)
target_include_directories(gbarecomp_gba PUBLIC src/gba)
target_link_libraries(gbarecomp_gba PUBLIC gbarecomp_armv4t)
set_property(TARGET gbarecomp_gba PROPERTY POSITION_INDEPENDENT_CODE ON)

# ─────────────────────────────────────────────────────────────────────────────
# runtime — what game binaries link against. Dispatch, host glue.
# ─────────────────────────────────────────────────────────────────────────────
add_library(gbarecomp_runtime STATIC
    src/runtime/runtime.cpp
    src/runtime/generated_dispatch.cpp
    src/runtime/runtime_bus_bridge.cpp
    src/runtime/host_platform.cpp
    src/runtime/host_window.cpp
)
target_include_directories(gbarecomp_runtime PUBLIC src/runtime)
target_link_libraries(gbarecomp_runtime PUBLIC gbarecomp_gba gbarecomp_debug)
set_property(TARGET gbarecomp_runtime PROPERTY POSITION_INDEPENDENT_CODE ON)
if(GBARECOMP_SDL2_OK)
    target_compile_definitions(gbarecomp_runtime PUBLIC GBARECOMP_HAVE_SDL2)
    # Use plain -I (not -isystem) for SDL2 headers. -isystem on a path
    # that overlaps libstdc++'s search ordering breaks
    # `#include_next <stdlib.h>`.
    target_include_directories(gbarecomp_runtime PUBLIC ${SDL2_INCLUDE_DIRS})
    target_link_libraries(gbarecomp_runtime PUBLIC ${SDL2_LIBRARIES})
endif()

# ─────────────────────────────────────────────────────────────────────────────
# debug — TCP server + always-on ring buffers + snapshot. Always built.
# ─────────────────────────────────────────────────────────────────────────────
add_library(gbarecomp_debug STATIC
    src/debug/tcp_debug_server.cpp
    src/debug/trace_ring.cpp
    src/debug/snapshot.cpp
)
target_include_directories(gbarecomp_debug PUBLIC src/debug)
target_link_libraries(gbarecomp_debug PUBLIC gbarecomp_gba)
set_property(TARGET gbarecomp_debug PROPERTY POSITION_INDEPENDENT_CODE ON)
if(WIN32 OR MSYS)
    target_link_libraries(gbarecomp_debug PUBLIC ws2_32)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# tools — small CLI utilities. Each is a separate executable.
# ─────────────────────────────────────────────────────────────────────────────
add_executable(gba_scan tools/gba_scan/main.cpp)
target_link_libraries(gba_scan PRIVATE gbarecomp_armv4t gbarecomp_gba)

add_executable(gba_recompile
    tools/gba_recompile/main.cpp
    tools/gba_recompile/function_finder.cpp)
target_include_directories(gba_recompile PRIVATE
    tools/gba_recompile
    src/armv4t)
target_link_libraries(gba_recompile PRIVATE gbarecomp_armv4t gbarecomp_gba)

add_executable(symbol_import tools/symbol_import/main.cpp)
target_link_libraries(symbol_import PRIVATE gbarecomp_armv4t)

add_executable(test_rom_runner tools/test_rom_runner/main.cpp)
target_link_libraries(test_rom_runner PRIVATE
    gbarecomp_armv4t gbarecomp_gba gbarecomp_runtime gbarecomp_debug)

# bios_smoke — Phase 2.1 canary. Loads bios/gba_bios.bin, runs the
# interpreter from PC=0 for N instructions, prints a trace.
add_executable(bios_smoke tools/bios_smoke/main.cpp)
target_link_libraries(bios_smoke PRIVATE
    gbarecomp_armv4t gbarecomp_gba gbarecomp_runtime gbarecomp_debug)
if(WIN32 OR MSYS)
    target_link_libraries(bios_smoke PRIVATE ws2_32)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# tests — per-subsystem. Run with ctest.
# ─────────────────────────────────────────────────────────────────────────────
enable_testing()

add_executable(decoder_smoke tests/armv4t/decoder_smoke.cpp)
target_link_libraries(decoder_smoke PRIVATE gbarecomp_armv4t)
add_test(NAME decoder_smoke COMMAND decoder_smoke)

add_executable(interpreter_smoke tests/armv4t/interpreter_smoke.cpp)
target_link_libraries(interpreter_smoke PRIVATE gbarecomp_armv4t)
add_test(NAME interpreter_smoke COMMAND interpreter_smoke)

add_executable(armv4t_tests tests/armv4t/test_main.cpp)
target_link_libraries(armv4t_tests PRIVATE gbarecomp_armv4t)
add_test(NAME armv4t_tests COMMAND armv4t_tests)

add_executable(thumb_tests tests/thumb/test_main.cpp)
target_link_libraries(thumb_tests PRIVATE gbarecomp_armv4t)
add_test(NAME thumb_tests COMMAND thumb_tests)

add_executable(bus_tests tests/bus/test_main.cpp)
target_link_libraries(bus_tests PRIVATE gbarecomp_gba)
add_test(NAME bus_tests COMMAND bus_tests)

add_executable(dma_tests tests/dma/test_main.cpp)
target_link_libraries(dma_tests PRIVATE gbarecomp_gba)
add_test(NAME dma_tests COMMAND dma_tests)

add_executable(timer_tests tests/timers/test_main.cpp)
target_link_libraries(timer_tests PRIVATE gbarecomp_gba)
add_test(NAME timer_tests COMMAND timer_tests)

add_executable(irq_tests tests/irq/test_main.cpp)
target_link_libraries(irq_tests PRIVATE gbarecomp_gba)
add_test(NAME irq_tests COMMAND irq_tests)

add_executable(ppu_smoke_tests tests/ppu_smoke/test_main.cpp)
target_link_libraries(ppu_smoke_tests PRIVATE gbarecomp_gba)
add_test(NAME ppu_smoke_tests COMMAND ppu_smoke_tests)

# ─────────────────────────────────────────────────────────────────────────────
# Oracle binary — opt-in (-DGBARECOMP_BUILD_ORACLE=ON). Embeds libmgba
# (MPL-2.0). Per PRINCIPLES.md the native build has zero copyleft deps;
# the oracle binary is the ONLY place libmgba may be linked, and it
# runs as a SEPARATE PROCESS over TCP.
# ─────────────────────────────────────────────────────────────────────────────
option(GBARECOMP_BUILD_ORACLE
       "Build the mGBA-backed oracle (requires third_party/mgba/build/libmgba.a)"
       OFF)
if(GBARECOMP_BUILD_ORACLE)
    add_subdirectory(oracle)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# runtime.cmake — game projects (e.g. MinishCapRecomp) include this to add
# a runnable target that links generated C against the runtime libs.
# ─────────────────────────────────────────────────────────────────────────────
configure_file(
    cmake/runtime.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/runtime.cmake
    @ONLY
)
