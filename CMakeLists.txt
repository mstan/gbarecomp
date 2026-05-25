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

# Release builds bake SDL2 + libstdc++ + libgcc + libwinpthread into
# the executable so the shipped .exe has no DLL dependencies on a
# vanilla Windows install. Dev / debug builds keep the dynamic SDL2
# link so iteration is fast.
option(GBARECOMP_STATIC_RELEASE
    "Statically link SDL2 + the mingw C++ runtime into release exes"
    OFF)

find_path(SDL2_INCLUDE_DIR
    NAMES SDL.h
    PATH_SUFFIXES SDL2 SDL2/include
    HINTS ${GBARECOMP_MINGW_PREFIX_UNIX}/include
)
if(GBARECOMP_STATIC_RELEASE)
    # Pin the static archives directly. CMake's find_library would
    # prefer libSDL2.dll.a on a default MSYS2 install. Try the Unix
    # form first (works under msys2 shells), fall back to the native
    # Windows path (works under PowerShell / cmd).
    set(SDL2_STATIC_LIB_CANDIDATES
        ${GBARECOMP_MINGW_PREFIX_UNIX}/lib/libSDL2.a
        C:/msys64/mingw64/lib/libSDL2.a)
    set(SDL2_LIBRARY "")
    foreach(_cand IN LISTS SDL2_STATIC_LIB_CANDIDATES)
        if(EXISTS "${_cand}")
            set(SDL2_LIBRARY      "${_cand}")
            string(REPLACE "libSDL2.a" "libSDL2main.a"
                   SDL2_MAIN_LIBRARY "${_cand}")
            break()
        endif()
    endforeach()
    if(SDL2_LIBRARY STREQUAL "" OR NOT EXISTS "${SDL2_MAIN_LIBRARY}")
        message(FATAL_ERROR
            "GBARECOMP_STATIC_RELEASE=ON but libSDL2.a / libSDL2main.a "
            "not found. Searched: ${SDL2_STATIC_LIB_CANDIDATES}")
    endif()
    # SDL2 static depends on a list of Windows system libs that
    # libSDL2.dll already pulls in transitively when dynamic.
    # (From `pkg-config --static --libs sdl2`.)
    # Use full paths to libSDL2.a + libSDL2main.a — `-lSDL2` would let
    # the linker prefer libSDL2.dll.a (dynamic import) over libSDL2.a.
    set(SDL2_STATIC_SYSDEPS
        -lmingw32
        ${SDL2_MAIN_LIBRARY}
        ${SDL2_LIBRARY}
        -lm
        -lkernel32 -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32
        -lversion -luuid -ladvapi32 -lsetupapi -lshell32 -ldinput8
        -mwindows)
else()
    find_library(SDL2_LIBRARY
        NAMES SDL2
        HINTS ${GBARECOMP_MINGW_PREFIX_UNIX}/lib
    )
endif()

if(SDL2_INCLUDE_DIR AND SDL2_LIBRARY)
    set(GBARECOMP_SDL2_OK TRUE)
    set(SDL2_INCLUDE_DIRS ${SDL2_INCLUDE_DIR})
    if(GBARECOMP_STATIC_RELEASE)
        set(SDL2_LIBRARIES ${SDL2_STATIC_SYSDEPS})
    else()
        set(SDL2_LIBRARIES ${SDL2_LIBRARY})
    endif()
    message(STATUS "gbarecomp: SDL2 found — inc=${SDL2_INCLUDE_DIR} lib=${SDL2_LIBRARY} static=${GBARECOMP_STATIC_RELEASE}")
else()
    message(STATUS "gbarecomp: SDL2 NOT found — host_window will stub out (inc=${SDL2_INCLUDE_DIR} lib=${SDL2_LIBRARY})")
endif()

if(GBARECOMP_STATIC_RELEASE)
    # NOTE: -static / -static-libgcc / -static-libstdc++ MUST be set
    # via -DCMAKE_EXE_LINKER_FLAGS at configure time (see package_release.ps1).
    # add_link_options() here would only apply to targets defined in
    # gbarecomp's subdir, not to a parent project that includes us via
    # add_subdirectory() (e.g. MinishCapRecomp). The cache var works
    # across the whole tree.
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
    src/gba/crc32.cpp
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
    src/runtime/asset_picker.cpp
    # Production abort stubs for runtime_dispatch_miss +
    # runtime_unimplemented_op. Test binaries that link only
    # gbarecomp_armv4t (not gbarecomp_runtime) supply their own.
    src/runtime/runtime_arm_default_aborts.cpp
    # BIOS dispatch table — placeholder until `gba_recompile --bios`
    # is run, real bios_recompiled.cpp is gitignored and linked
    # opt-in via GBARECOMP_HAVE_BIOS_RECOMP below.
    src/runtime/generated_bios/bios_dispatch_table.cpp
)

# When the user has run `gba_recompile --bios`, the recompiled BIOS
# functions live in src/runtime/generated_bios/bios_recompiled.cpp.
# We can't list it unconditionally because the placeholder build
# doesn't have it. Pull it in if present.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/generated_bios/bios_recompiled.cpp")
    target_sources(gbarecomp_runtime PRIVATE
        src/runtime/generated_bios/bios_recompiled.cpp)
    target_compile_definitions(gbarecomp_runtime PUBLIC
        GBARECOMP_HAVE_BIOS_RECOMP=1)
    message(STATUS "gbarecomp: BIOS recompiled output present — linking")
else()
    message(STATUS "gbarecomp: BIOS recompiled output absent — placeholder dispatch only. "
                   "Run `build/gba_recompile.exe --bios bios/gba_bios.bin` "
                   "to populate.")
endif()
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
if(WIN32 OR MSYS)
    # asset_picker uses GetOpenFileNameA (comdlg32) and MessageBoxA (user32).
    target_link_libraries(gbarecomp_runtime PUBLIC comdlg32 user32)
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
function(target_link_gbarecomp_runtime_stack target_name)
    if(MINGW OR MSYS)
        target_link_libraries(${target_name} PRIVATE
            -Wl,--start-group
            gbarecomp_armv4t gbarecomp_gba gbarecomp_runtime gbarecomp_debug
            -Wl,--end-group)
    else()
        target_link_libraries(${target_name} PRIVATE
            gbarecomp_armv4t gbarecomp_gba gbarecomp_runtime gbarecomp_debug)
    endif()
endfunction()

add_executable(gba_scan tools/gba_scan/main.cpp)
target_link_libraries(gba_scan PRIVATE gbarecomp_armv4t gbarecomp_gba)

add_executable(gba_recompile
    tools/gba_recompile/main.cpp
    tools/gba_recompile/function_finder.cpp
    tools/gba_recompile/config.cpp)
target_include_directories(gba_recompile PRIVATE
    tools/gba_recompile
    src/armv4t
    third_party/tomlpp)
target_link_libraries(gba_recompile PRIVATE gbarecomp_armv4t gbarecomp_gba)

add_executable(symbol_import tools/symbol_import/main.cpp)
target_link_libraries(symbol_import PRIVATE gbarecomp_armv4t)

# test_rom_runner — direct-boots a recompiled jsmolka gba-suite test
# ROM and reports r12 at exit. Two targets per ROM (ARM + THUMB):
# each ROM is recompiled at build time via gba_recompile and linked
# into its own runner exe. Output is the test_rom_runner main
# (shared) + the per-ROM generated body.

set(GBA_SUITE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/gba_suite)

# Recompile arm.gba at build time.
set(TEST_ROM_ARM_OUTDIR ${CMAKE_CURRENT_BINARY_DIR}/test_rom_arm_gen)
set(TEST_ROM_ARM_DISPATCH ${TEST_ROM_ARM_OUTDIR}/dispatch_table.cpp)
set(TEST_ROM_ARM_BODY     ${TEST_ROM_ARM_OUTDIR}/recompiled.cpp)
set(TEST_ROM_ARM_HEADER   ${TEST_ROM_ARM_OUTDIR}/recompiled.h)
file(MAKE_DIRECTORY ${TEST_ROM_ARM_OUTDIR})

if(EXISTS ${GBA_SUITE_ROOT}/arm/arm.gba)
    add_custom_command(
        OUTPUT  ${TEST_ROM_ARM_DISPATCH} ${TEST_ROM_ARM_BODY}
                ${TEST_ROM_ARM_HEADER}
        COMMAND $<TARGET_FILE:gba_recompile>
                --rom ${GBA_SUITE_ROOT}/arm/arm.gba
                --entry 0x080000C0
                --out ${TEST_ROM_ARM_OUTDIR}
        DEPENDS gba_recompile
                ${GBA_SUITE_ROOT}/arm/arm.gba
        COMMENT "Recompiling gba-suite arm.gba"
        VERBATIM
    )

    add_executable(test_rom_arm
        tools/test_rom_runner/main.cpp
        ${TEST_ROM_ARM_DISPATCH}
        ${TEST_ROM_ARM_BODY}
    )
    target_include_directories(test_rom_arm PRIVATE
        ${TEST_ROM_ARM_OUTDIR}
        src/armv4t)
    target_link_gbarecomp_runtime_stack(test_rom_arm)
    add_test(NAME test_rom_arm COMMAND test_rom_arm
        ${GBA_SUITE_ROOT}/arm/arm.gba)
else()
    message(STATUS "gbarecomp: gba-suite arm.gba not found; "
                   "skipping test_rom_arm target")
endif()

# Recompile thumb.gba at build time.
set(TEST_ROM_THUMB_OUTDIR ${CMAKE_CURRENT_BINARY_DIR}/test_rom_thumb_gen)
set(TEST_ROM_THUMB_DISPATCH ${TEST_ROM_THUMB_OUTDIR}/dispatch_table.cpp)
set(TEST_ROM_THUMB_BODY     ${TEST_ROM_THUMB_OUTDIR}/recompiled.cpp)
set(TEST_ROM_THUMB_HEADER   ${TEST_ROM_THUMB_OUTDIR}/recompiled.h)
file(MAKE_DIRECTORY ${TEST_ROM_THUMB_OUTDIR})

if(EXISTS ${GBA_SUITE_ROOT}/thumb/thumb.gba)
    add_custom_command(
        OUTPUT  ${TEST_ROM_THUMB_DISPATCH} ${TEST_ROM_THUMB_BODY}
                ${TEST_ROM_THUMB_HEADER}
        COMMAND $<TARGET_FILE:gba_recompile>
                --rom ${GBA_SUITE_ROOT}/thumb/thumb.gba
                --entry 0x080000C0
                --out ${TEST_ROM_THUMB_OUTDIR}
        DEPENDS gba_recompile
                ${GBA_SUITE_ROOT}/thumb/thumb.gba
        COMMENT "Recompiling gba-suite thumb.gba"
        VERBATIM
    )

    add_executable(test_rom_thumb
        tools/test_rom_runner/main.cpp
        ${TEST_ROM_THUMB_DISPATCH}
        ${TEST_ROM_THUMB_BODY}
    )
    target_include_directories(test_rom_thumb PRIVATE
        ${TEST_ROM_THUMB_OUTDIR}
        src/armv4t)
    target_link_gbarecomp_runtime_stack(test_rom_thumb)
    add_test(NAME test_rom_thumb COMMAND test_rom_thumb
        ${GBA_SUITE_ROOT}/thumb/thumb.gba)
else()
    message(STATUS "gbarecomp: gba-suite thumb.gba not found; "
                   "skipping test_rom_thumb target")
endif()

# bios_smoke — Phase 2.1 canary. Loads bios/gba_bios.bin, runs the
# interpreter from PC=0 for N instructions, prints a trace.
add_executable(bios_smoke tools/bios_smoke/main.cpp)
target_link_gbarecomp_runtime_stack(bios_smoke)
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

# ─────────────────────────────────────────────────────────────────────
# codegen_tests — L1 per-IrOp diff harness.
#
# Architecture:
#   1. tests/codegen/test_cases.cpp lists per-IrOp test cases.
#   2. gen_codegen_tests.exe reads that list, invokes the same
#      ArmCodegen used by tools/gba_recompile, and emits one C
#      function per case into ${CMAKE_BINARY_DIR}/codegen_tests/
#      test_funcs.cpp.
#   3. codegen_tests.exe links test_main.cpp + test_cases.cpp +
#      stubs.cpp + the generated test_funcs.cpp against
#      gbarecomp_armv4t. It runs both the interpreter and the
#      generated function per case and diffs the resulting state.
#
# Per PRINCIPLES.md "Interpreter is informative, never load-bearing":
# the interpreter is the diff-oracle, never a runtime fallback. Any
# divergence is a real codegen bug to fix in arm_codegen.cpp.
# ─────────────────────────────────────────────────────────────────────
add_executable(gen_codegen_tests
    tests/codegen/gen_codegen_tests.cpp
    tests/codegen/test_cases.cpp
)
target_include_directories(gen_codegen_tests PRIVATE
    tests/codegen
    src/armv4t)
target_link_libraries(gen_codegen_tests PRIVATE gbarecomp_armv4t)

set(CODEGEN_TESTS_OUTDIR
    ${CMAKE_CURRENT_BINARY_DIR}/codegen_tests)
set(CODEGEN_TESTS_GENFILE
    ${CODEGEN_TESTS_OUTDIR}/test_funcs.cpp)

file(MAKE_DIRECTORY ${CODEGEN_TESTS_OUTDIR})

add_custom_command(
    OUTPUT  ${CODEGEN_TESTS_GENFILE}
    COMMAND gen_codegen_tests ${CODEGEN_TESTS_GENFILE}
    DEPENDS gen_codegen_tests
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/codegen/test_cases.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/codegen/test_cases.h
    COMMENT "Emitting L1 codegen test functions"
    VERBATIM
)

add_executable(codegen_tests
    tests/codegen/test_main.cpp
    tests/codegen/test_cases.cpp
    tests/codegen/stubs.cpp
    ${CODEGEN_TESTS_GENFILE}
)
target_include_directories(codegen_tests PRIVATE
    tests/codegen
    src/armv4t)
target_link_libraries(codegen_tests PRIVATE gbarecomp_armv4t)
add_test(NAME codegen_tests COMMAND codegen_tests)

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
