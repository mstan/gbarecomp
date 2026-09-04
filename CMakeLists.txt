cmake_minimum_required(VERSION 3.20)
project(gbarecomp C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Large generated corpora are expensive to rebuild in fresh build trees.
# Reuse identical objects automatically when ccache/sccache is available, while
# preserving an explicit launcher selected by the parent project or user.
set(GBARECOMP_COMPILER_CACHE "AUTO" CACHE STRING
    "Compiler cache: AUTO, OFF, or a ccache/sccache executable")
if(GBARECOMP_COMPILER_CACHE STREQUAL "OFF")
    if(GBARECOMP_MANAGED_COMPILER_CACHE_PROGRAM AND
       "${CMAKE_C_COMPILER_LAUNCHER}" STREQUAL
           "${GBARECOMP_MANAGED_COMPILER_CACHE_PROGRAM}")
        unset(CMAKE_C_COMPILER_LAUNCHER CACHE)
    endif()
    if(GBARECOMP_MANAGED_COMPILER_CACHE_PROGRAM AND
       "${CMAKE_CXX_COMPILER_LAUNCHER}" STREQUAL
           "${GBARECOMP_MANAGED_COMPILER_CACHE_PROGRAM}")
        unset(CMAKE_CXX_COMPILER_LAUNCHER CACHE)
    endif()
    unset(GBARECOMP_MANAGED_COMPILER_CACHE_PROGRAM CACHE)
elseif(NOT CMAKE_CXX_COMPILER_LAUNCHER)
    unset(GBARECOMP_COMPILER_CACHE_PROGRAM CACHE)
    if(GBARECOMP_COMPILER_CACHE STREQUAL "AUTO")
        find_program(GBARECOMP_COMPILER_CACHE_PROGRAM
            NAMES sccache ccache
            HINTS C:/msys64/mingw64/bin)
    elseif(EXISTS "${GBARECOMP_COMPILER_CACHE}")
        set(GBARECOMP_COMPILER_CACHE_PROGRAM
            "${GBARECOMP_COMPILER_CACHE}")
    else()
        find_program(GBARECOMP_COMPILER_CACHE_PROGRAM
            NAMES "${GBARECOMP_COMPILER_CACHE}")
    endif()
    if(GBARECOMP_COMPILER_CACHE_PROGRAM)
        set(CMAKE_C_COMPILER_LAUNCHER
            "${GBARECOMP_COMPILER_CACHE_PROGRAM}" CACHE STRING
            "Launcher for C compilation")
        set(CMAKE_CXX_COMPILER_LAUNCHER
            "${GBARECOMP_COMPILER_CACHE_PROGRAM}" CACHE STRING
            "Launcher for C++ compilation")
        set(GBARECOMP_MANAGED_COMPILER_CACHE_PROGRAM
            "${GBARECOMP_COMPILER_CACHE_PROGRAM}" CACHE INTERNAL
            "Compiler launcher selected by gbarecomp" FORCE)
        message(STATUS "gbarecomp: compiler cache enabled: "
            "${GBARECOMP_COMPILER_CACHE_PROGRAM}")
    else()
        message(STATUS "gbarecomp: compiler cache not found")
    endif()
endif()

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
option(GBARECOMP_SELFHEAL_RECOMPILE_DEFAULT
    "Enable runtime self-heal overlay compilation by default"
    ON)

# First-divergence co-simulation oracle (COSIM_ORACLE.md). When ON, compiles the
# cosim full-state hash + cycle-keyed park/step TCP engine and arms the runtime_tick
# checkpoint hook. Defined PUBLIC on gbarecomp_debug so it propagates through
# gbarecomp_runtime (runtime.cpp / runtime_bus_bridge.cpp) to the game exe. The
# force-interp backend (g_force_interp / GBARECOMP_FORCE_INTERP) is always compiled
# and independent of this option. OFF = byte-identical to a normal build.
option(GBA_COSIM
    "Build the first-divergence co-simulation oracle (state hash + TCP park/step)"
    OFF)

if(ANDROID AND TARGET SDL2::SDL2)
    # Android applications provide SDL from the parent Gradle/CMake project.
    # Reusing that target keeps SDLActivity's libSDL2.so as the single backend.
    set(GBARECOMP_SDL2_OK TRUE)
    set(SDL2_LIBRARIES SDL2::SDL2)
    get_target_property(SDL2_INCLUDE_DIRS SDL2::SDL2
                        INTERFACE_INCLUDE_DIRECTORIES)
    message(STATUS "gbarecomp: using parent-provided Android SDL2 target")
else()
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
# Parent game projects can opt into a double-clickable MinGW development
# directory without duplicating the same four-DLL staging recipe. Static
# release targets deliberately skip this because those dependencies are baked
# into the executable.
set(GBARECOMP_MINGW_RUNTIME_BIN "C:/msys64/mingw64/bin"
    CACHE PATH "Directory containing MinGW runtime DLLs for development builds")

function(gbarecomp_stage_mingw_runtime target_name)
    if(NOT TARGET "${target_name}")
        message(FATAL_ERROR
            "gbarecomp_stage_mingw_runtime: unknown target '${target_name}'")
    endif()
    if(NOT WIN32 OR NOT MINGW OR GBARECOMP_STATIC_RELEASE)
        return()
    endif()

    foreach(runtime_dll IN ITEMS
            SDL2.dll
            libgcc_s_seh-1.dll
            libstdc++-6.dll
            libwinpthread-1.dll)
        set(runtime_source "${GBARECOMP_MINGW_RUNTIME_BIN}/${runtime_dll}")
        if(EXISTS "${runtime_source}")
            add_custom_command(TARGET "${target_name}" POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${runtime_source}"
                    "$<TARGET_FILE_DIR:${target_name}>/${runtime_dll}"
                VERBATIM)
        else()
            message(WARNING
                "gbarecomp: development runtime DLL not found: ${runtime_source}")
        endif()
    endforeach()
endfunction()

set(ARM_RECOMP_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/arm-recomp-core")
include("${ARM_RECOMP_CORE_DIR}/cmake/ArmRecompCore.cmake")
arm_recomp_core_sources(GBA_ARM_RECOMP_CORE_SOURCES armv4t_gba)
arm_recomp_core_include_dirs(GBA_ARM_RECOMP_CORE_INCLUDE_DIRS armv4t_gba)

add_library(gbarecomp_armv4t STATIC
    ${GBA_ARM_RECOMP_CORE_SOURCES}
    src/armv4t/runtime_arm.cpp
    src/armv4t/symbol_lookup.cpp
)
target_include_directories(gbarecomp_armv4t PUBLIC
    src/armv4t
    ${GBA_ARM_RECOMP_CORE_INCLUDE_DIRS})
set_property(TARGET gbarecomp_armv4t PROPERTY POSITION_INDEPENDENT_CODE ON)

# ─────────────────────────────────────────────────────────────────────────────
# recompile_core — function discovery + per-function codegen, as a LIBRARY.
# Extracted from tools/gba_recompile so the SAME emitter serves both the
# offline corpus build (the tool) and the Stage-2 runtime self-healing
# recompiler (a dispatch miss → emit one function → gcc → DLL). Depends only
# on armv4t (decoder/IR/codegen); must NOT depend on src/gba or src/runtime.
# ─────────────────────────────────────────────────────────────────────────────
add_library(gbarecomp_recompile_core STATIC
    src/recompile/function_finder.cpp
    src/recompile/emit_function.cpp
    src/recompile/codegen_shards.cpp
    src/recompile/dispatch_table_entries.cpp
)
target_include_directories(gbarecomp_recompile_core PUBLIC src/recompile)
target_link_libraries(gbarecomp_recompile_core PUBLIC gbarecomp_armv4t)
set_property(TARGET gbarecomp_recompile_core PROPERTY POSITION_INDEPENDENT_CODE ON)

# ─────────────────────────────────────────────────────────────────────────────
# gba — GBA-specific bus, IO, IRQ, DMA, timers, PPU, audio, input, save, BIOS.
# Aggressively GBA-specific. Depends on armv4t for CPU types.
# ─────────────────────────────────────────────────────────────────────────────
add_library(gbarecomp_gba STATIC
    src/gba/gba_memory.cpp
    src/gba/gba_bus.cpp
    src/gba/gba_matrix_memory.cpp
    src/gba/gba_io.cpp
    src/gba/gba_irq.cpp
    src/gba/gba_dma.cpp
    src/gba/gba_timers.cpp
    src/gba/gba_ppu.cpp
    src/gba/gba_audio.cpp
    src/gba/mod_audio.cpp
    src/gba/gba_m4a.cpp
    src/gba/audio_shadow.cpp
    src/gba/mp2k_shadow.cpp
    src/gba/gba_input.cpp
    src/gba/gba_save.cpp
    src/gba/gba_gpio.cpp
    src/gba/gba_gyro.cpp
    src/gba/gba_solar.cpp
    src/gba/gba_rtc.cpp
    src/gba/gba_bios.cpp
    src/gba/gba_scheduler.cpp
    src/gba/gba_rom_header.cpp
    src/gba/sha1.cpp
    src/gba/crc32.cpp
)
target_include_directories(gbarecomp_gba PUBLIC src/gba)
# src/debug for snapshot.h — the save-state SnapshotWriter/Reader are
# header-only, so this is an include-path dependency only (gbarecomp_gba
# does NOT link gbarecomp_debug; that would invert the layering).
target_include_directories(gbarecomp_gba PRIVATE src/debug)
target_link_libraries(gbarecomp_gba PUBLIC gbarecomp_armv4t)
set_property(TARGET gbarecomp_gba PROPERTY POSITION_INDEPENDENT_CODE ON)

# ─────────────────────────────────────────────────────────────────────────────
# heal_gate — P6 differential-gate primitives (snapshot/journal/diff). PURE:
# depends only on gbarecomp_gba (GbaBus + ArmCpuState), no runtime globals, so
# it unit-tests standalone and the runtime gate state machine layers on top.
# ─────────────────────────────────────────────────────────────────────────────
add_library(gbarecomp_heal_gate STATIC src/runtime/heal_gate.cpp)
target_include_directories(gbarecomp_heal_gate PUBLIC src/runtime)
target_link_libraries(gbarecomp_heal_gate PUBLIC gbarecomp_gba)
set_property(TARGET gbarecomp_heal_gate PROPERTY POSITION_INDEPENDENT_CODE ON)

set(GBARECOMP_HOST_STACK_RESERVE_BYTES "16777216" CACHE STRING
    "Host stack reserve for Windows game executables that link gbarecomp_runtime")

function(gbarecomp_compiler_uses_msvc_linker out_var)
    if(MSVC OR
       CMAKE_C_SIMULATE_ID STREQUAL "MSVC" OR
       CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC" OR
       CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC" OR
       CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        set(${out_var} TRUE PARENT_SCOPE)
    else()
        set(${out_var} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(gbarecomp_target_link_host_stack target_name scope)
    gbarecomp_compiler_uses_msvc_linker(_gbarecomp_msvc_linker)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC" OR
       CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        target_link_options(${target_name} ${scope}
            "/STACK:${GBARECOMP_HOST_STACK_RESERVE_BYTES}")
    elseif(CMAKE_EXECUTABLE_SUFFIX STREQUAL ".exe" AND
           _gbarecomp_msvc_linker)
        # MSVC-style Clang/lld-link does not understand GNU --stack. Route
        # through CMake's linker wrapper so the compiler driver forwards the
        # Windows /STACK form to the linker.
        target_link_options(${target_name} ${scope}
            "LINKER:/STACK:${GBARECOMP_HOST_STACK_RESERVE_BYTES}")
    elseif(CMAKE_EXECUTABLE_SUFFIX STREQUAL ".exe" AND
           (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" OR
            CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
        target_link_options(${target_name} ${scope}
            "LINKER:--stack,${GBARECOMP_HOST_STACK_RESERVE_BYTES}")
    endif()
endfunction()

# ─────────────────────────────────────────────────────────────────────────────
# runtime — what game binaries link against. Dispatch, host glue.
# ─────────────────────────────────────────────────────────────────────────────
option(GBARECOMP_ENABLE_MODS
    "Build the GBA mod package loader and trusted-plugin runtime"
    OFF)

add_library(gbarecomp_runtime STATIC
    src/runtime/runtime.cpp
    src/runtime/save_config.cpp
    src/runtime/bios_hle.cpp
    src/runtime/generated_dispatch.cpp
    src/runtime/runtime_bus_bridge.cpp
    src/runtime/arm_cpu_bridge.cpp
    src/runtime/overlay_emit.cpp
    src/runtime/overlay_loader.cpp
    src/runtime/overlay_compile.cpp
    # Trusted function-entry hooks are always linked: opted-in generated code
    # must work even when the package catalog itself is disabled.
    src/runtime/mod_function_hooks.cpp
    src/runtime/host_platform.cpp
    src/runtime/host_window.cpp
    src/runtime/color_lut.cpp
    src/runtime/asset_picker.cpp
    # Production abort stubs for runtime_dispatch_miss +
    # runtime_unimplemented_op. Test binaries that link only
    # gbarecomp_armv4t (not gbarecomp_runtime) supply their own.
    src/runtime/runtime_arm_default_aborts.cpp
    src/runtime/self_heal_cluster.cpp
    # BIOS dispatch/body selection is handled below. Clean checkouts use a
    # tracked empty stub; locally generated BIOS output is selected as a unit.
)
gbarecomp_target_link_host_stack(gbarecomp_runtime INTERFACE)
if(GBARECOMP_ENABLE_MODS)
    target_sources(gbarecomp_runtime PRIVATE src/runtime/mod_runtime.cpp)
    target_compile_definitions(gbarecomp_runtime PUBLIC
        GBARECOMP_ENABLE_MODS=1)
    # recomp-ui retains its own compile-time gate in addition to requiring a
    # non-null provider. Only an explicitly opted-in game opens that gate.
    set(RECOMP_UI_ENABLE_MODS ON CACHE BOOL
        "Enable recomp-ui Mods view for this GBA mod-enabled game" FORCE)
endif()

# Keep private BIOS-derived output out of a framework checkout when a game
# supplies a build-local directory. The source-tree default preserves the
# existing standalone workflow.
set(GBARECOMP_GENERATED_BIOS_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/generated_bios"
    CACHE PATH "Directory containing locally generated BIOS recompilation sources")

# When the user has run `gba_recompile --bios`, the recompiled BIOS
# functions live in src/runtime/generated_bios/bios_recompiled.cpp.
# We can't list it unconditionally because the placeholder build
# doesn't have it. Pull it in if present.
if(EXISTS "${GBARECOMP_GENERATED_BIOS_DIR}/bios_recompiled.cpp")
    target_sources(gbarecomp_runtime PRIVATE
        "${GBARECOMP_GENERATED_BIOS_DIR}/bios_dispatch_table.cpp"
        "${GBARECOMP_GENERATED_BIOS_DIR}/bios_recompiled.cpp")
    target_compile_definitions(gbarecomp_runtime PUBLIC
        GBARECOMP_HAVE_BIOS_RECOMP=1)
    if(NOT WIN32)
        # The recompiled BIOS reset vector is named `_start` after its crt0
        # label. On ELF hosts that collides with the process entry symbol from
        # Scrt1.o, so rename the guest symbol consistently across the
        # generated BIOS translation units. PUBLIC keeps any dependent that
        # includes bios_recompiled.h in agreement. Windows entry symbols
        # differ, so the historical name is preserved there.
        target_compile_definitions(gbarecomp_runtime PUBLIC
            _start=gbarecomp_bios_reset_start)
    endif()
    message(STATUS "gbarecomp: BIOS recompiled output present — linking")
else()
    target_sources(gbarecomp_runtime PRIVATE
        src/runtime/generated_bios/bios_dispatch_stub.cpp)
    message(STATUS "gbarecomp: BIOS recompiled output absent — placeholder dispatch only. "
                   "Run `build/gba_recompile.exe --bios bios/gba_bios.bin` "
                   "to populate.")
endif()
target_include_directories(gbarecomp_runtime PUBLIC src/runtime)
if(GBARECOMP_RUNTIME_UI_ROOT AND
   EXISTS "${GBARECOMP_RUNTIME_UI_ROOT}/src/recomp_runtime_ui.h")
    target_compile_definitions(gbarecomp_runtime PUBLIC GBARECOMP_RUNTIME_UI=1)
    target_include_directories(gbarecomp_runtime PUBLIC
        "${GBARECOMP_RUNTIME_UI_ROOT}/src"
        "${GBARECOMP_RUNTIME_UI_ROOT}/src/common"
        "${GBARECOMP_RUNTIME_UI_ROOT}/src/third_party/imgui"
        "${GBARECOMP_RUNTIME_UI_ROOT}/src/third_party/imgui/backends")
    if(GBARECOMP_ENABLE_MODS AND
       EXISTS "${GBARECOMP_RUNTIME_UI_ROOT}/src/recomp_launcher.h")
        target_compile_definitions(gbarecomp_runtime PRIVATE
            GBARECOMP_MOD_UI=1)
    endif()
    if(GBARECOMP_SDL2_OK)
        # The runtime owns the SDL_Renderer2 adapter; the game's existing
        # game_launcher_ui target supplies the one shared ImGui core and SDL2
        # platform backend. The central helper is idempotent, preventing
        # duplicate renderer-backend symbols.
        set(RECOMP_UI_ROOT "${GBARECOMP_RUNTIME_UI_ROOT}")
        include("${GBARECOMP_RUNTIME_UI_ROOT}/recomp_ui.cmake")
        recomp_target_runtime_ui_sdlrenderer2(gbarecomp_runtime)
    endif()
endif()
# Bake the gbarecomp source root so the Stage-2 runtime compiler can find the
# overlay shim headers (src/runtime + src/armv4t) when it invokes g++ or the
# bundled tcc on a healed function. CMAKE_CURRENT_SOURCE_DIR is the gbarecomp
# worktree root even
# when a game project (MinishCapRecomp) pulls us in via add_subdirectory.
target_compile_definitions(gbarecomp_runtime PRIVATE
    GBARECOMP_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    GBARECOMP_SELFHEAL_RECOMPILE_DEFAULT=$<BOOL:${GBARECOMP_SELFHEAL_RECOMPILE_DEFAULT}>)
target_link_libraries(gbarecomp_runtime PUBLIC
    gbarecomp_gba gbarecomp_debug gbarecomp_recompile_core
    gbarecomp_heal_gate)
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
    # host_platform's FramePacer uses timeBeginPeriod/timeEndPeriod (winmm).
    target_link_libraries(gbarecomp_runtime PUBLIC comdlg32 user32 winmm)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# debug — TCP server + always-on ring buffers + snapshot. Always built.
# ─────────────────────────────────────────────────────────────────────────────
add_library(gbarecomp_debug STATIC
    src/debug/tcp_debug_server.cpp
    src/debug/trace_ring.cpp
    src/debug/snapshot.cpp
    src/debug/mod_state.cpp
    src/debug/ws_provenance.cpp
    src/debug/ws_sidecar.cpp
    # First-divergence co-sim engine + full-state hash. Empty TUs unless
    # GBA_COSIM is defined (guarded internally), so safe in the normal build.
    src/debug/cosim.cpp
    src/debug/cosim_state.cpp
)
target_include_directories(gbarecomp_debug PUBLIC src/debug)
# ws_provenance.cpp + cosim_state.cpp consult the active bus via runtime_bus_bridge.h.
target_include_directories(gbarecomp_debug PRIVATE src/runtime)
target_link_libraries(gbarecomp_debug PUBLIC gbarecomp_gba)
set_property(TARGET gbarecomp_debug PROPERTY POSITION_INDEPENDENT_CODE ON)
if(WIN32 OR MSYS)
    target_link_libraries(gbarecomp_debug PUBLIC ws2_32)
endif()
# Propagate GBA_COSIM PUBLIC so the cosim files here, the runtime_tick hook in
# gbarecomp_runtime, and runtime.cpp's cosim_init() all compile the oracle in.
if(GBA_COSIM)
    target_compile_definitions(gbarecomp_debug PUBLIC GBA_COSIM)
    message(STATUS "gbarecomp: GBA_COSIM ON — first-divergence oracle compiled in")
endif()

# ─────────────────────────────────────────────────────────────────────────────
# tools — small CLI utilities. Each is a separate executable.
# ─────────────────────────────────────────────────────────────────────────────
function(target_link_gbarecomp_runtime_stack target_name)
    # The four archives are mutually recursive, so a single-pass linker needs an
    # explicit group. The gate is the LINKER, not the platform: this used to test
    # MINGW OR MSYS, which left native Linux -- GNU ld, but neither of those --
    # taking the plain-list branch and failing to link the test executables:
    #
    #   runtime_arm.cpp: undefined reference to `overlay_try_dispatch'
    #   runtime_arm.cpp: undefined reference to `kBiosDispatchTable'
    #
    # Apple's ld64 and MSVC-style linkers resolve archive cycles without help.
    # ld64 and lld-link reject --start-group outright, so they take the plain
    # list. GNU-style linkers, including MinGW Clang's GNU frontend, need the
    # explicit group.
    gbarecomp_compiler_uses_msvc_linker(_gbarecomp_msvc_linker)
    if(APPLE OR _gbarecomp_msvc_linker)
        target_link_libraries(${target_name} PRIVATE
            gbarecomp_armv4t gbarecomp_gba gbarecomp_runtime gbarecomp_debug)
    else()
        target_link_libraries(${target_name} PRIVATE
            -Wl,--start-group
            gbarecomp_armv4t gbarecomp_gba gbarecomp_runtime gbarecomp_debug
            -Wl,--end-group)
    endif()
endfunction()

function(add_gbarecomp_runtime_stack_test target_name source_file)
    add_executable(${target_name}
        ${source_file}
        tools/bios_smoke/dispatch_stub.cpp)
    target_include_directories(${target_name} PRIVATE src/debug)
    target_link_gbarecomp_runtime_stack(${target_name})
    add_test(NAME ${target_name} COMMAND ${target_name})
endfunction()

add_executable(gba_scan tools/gba_scan/main.cpp)
target_link_libraries(gba_scan PRIVATE gbarecomp_armv4t gbarecomp_gba)

# A local toml++ checkout remains useful for offline development, but it is
# intentionally ignored.  Fresh clones/worktrees must still be able to build
# gba_recompile, so resolve the pinned upstream dependency into the build tree
# when the local single-header checkout is absent.
find_path(GBARECOMP_TOMLPP_INCLUDE_DIR
    NAMES toml.hpp
    HINTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tomlpp"
    NO_DEFAULT_PATH)
if(NOT GBARECOMP_TOMLPP_INCLUDE_DIR)
    include(FetchContent)
    set(TOMLPLUSPLUS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(TOMLPLUSPLUS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(tomlplusplus
        GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
        GIT_TAG 30172438cee64926dc41fdd9c11fb3ba5b2ba9de)
    FetchContent_MakeAvailable(tomlplusplus)
    set(GBARECOMP_TOMLPP_INCLUDE_DIR "${tomlplusplus_SOURCE_DIR}")
    message(STATUS
        "gbarecomp: fetched pinned toml++ v3.4.0 for gba_recompile")
else()
    message(STATUS
        "gbarecomp: using local toml++ at ${GBARECOMP_TOMLPP_INCLUDE_DIR}")
endif()

add_executable(gba_recompile
    tools/gba_recompile/main.cpp
    tools/gba_recompile/config.cpp)
target_include_directories(gba_recompile PRIVATE
    tools/gba_recompile
    src/armv4t
    "${GBARECOMP_TOMLPP_INCLUDE_DIR}")
target_link_libraries(gba_recompile PRIVATE
    gbarecomp_recompile_core gbarecomp_armv4t gbarecomp_gba)

# The decomp symbol importer is tools/symbol_import/import_decomp_symbols.py
# (a standalone Python tool that reads readelf/link-map text and writes the
# TSV + overlay TOML gba_recompile consumes). It replaced a C++ stub that only
# printed "stub", so there is no executable to build here. See
# docs/SYMBOL_OVERLAY.md.

# test_rom_runner — direct-boots a recompiled jsmolka gba-suite test
# ROM and reports r12 at exit. Two targets per ROM (ARM + THUMB):
# each ROM is recompiled at build time via gba_recompile and linked
# into its own runner exe. Output is the test_rom_runner main
# (shared) + the per-ROM generated body.

set(GBA_SUITE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/gba_suite)

# Recompile arm.gba at build time.
set(TEST_ROM_ARM_OUTDIR ${CMAKE_CURRENT_BINARY_DIR}/test_rom_arm_gen)
set(TEST_ROM_ARM_DISPATCH ${TEST_ROM_ARM_OUTDIR}/dispatch_table.cpp)
set(TEST_ROM_ARM_BODIES
    ${TEST_ROM_ARM_OUTDIR}/recompiled_000.cpp
    ${TEST_ROM_ARM_OUTDIR}/recompiled_001.cpp)
set(TEST_ROM_ARM_HEADER   ${TEST_ROM_ARM_OUTDIR}/recompiled.h)
file(MAKE_DIRECTORY ${TEST_ROM_ARM_OUTDIR})

if(EXISTS ${GBA_SUITE_ROOT}/arm/arm.gba)
    add_custom_command(
        OUTPUT  ${TEST_ROM_ARM_DISPATCH} ${TEST_ROM_ARM_BODIES}
                ${TEST_ROM_ARM_HEADER}
        COMMAND $<TARGET_FILE:gba_recompile>
                --rom ${GBA_SUITE_ROOT}/arm/arm.gba
                --entry 0x080000C0
                --codegen-shards 2
                --out ${TEST_ROM_ARM_OUTDIR}
        DEPENDS gba_recompile
                ${GBA_SUITE_ROOT}/arm/arm.gba
        COMMENT "Recompiling gba-suite arm.gba"
        VERBATIM
    )

    add_executable(test_rom_arm
        tools/test_rom_runner/main.cpp
        ${TEST_ROM_ARM_DISPATCH}
        ${TEST_ROM_ARM_BODIES}
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
set(TEST_ROM_THUMB_BODIES
    ${TEST_ROM_THUMB_OUTDIR}/recompiled_000.cpp
    ${TEST_ROM_THUMB_OUTDIR}/recompiled_001.cpp)
set(TEST_ROM_THUMB_HEADER   ${TEST_ROM_THUMB_OUTDIR}/recompiled.h)
file(MAKE_DIRECTORY ${TEST_ROM_THUMB_OUTDIR})

if(EXISTS ${GBA_SUITE_ROOT}/thumb/thumb.gba)
    add_custom_command(
        OUTPUT  ${TEST_ROM_THUMB_DISPATCH} ${TEST_ROM_THUMB_BODIES}
                ${TEST_ROM_THUMB_HEADER}
        COMMAND $<TARGET_FILE:gba_recompile>
                --rom ${GBA_SUITE_ROOT}/thumb/thumb.gba
                --entry 0x080000C0
                --codegen-shards 2
                --out ${TEST_ROM_THUMB_OUTDIR}
        DEPENDS gba_recompile
                ${GBA_SUITE_ROOT}/thumb/thumb.gba
        COMMENT "Recompiling gba-suite thumb.gba"
        VERBATIM
    )

    add_executable(test_rom_thumb
        tools/test_rom_runner/main.cpp
        ${TEST_ROM_THUMB_DISPATCH}
        ${TEST_ROM_THUMB_BODIES}
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
add_executable(bios_smoke tools/bios_smoke/main.cpp
                          tools/bios_smoke/dispatch_stub.cpp)
target_link_gbarecomp_runtime_stack(bios_smoke)
if(WIN32 OR MSYS)
    target_link_libraries(bios_smoke PRIVATE ws2_32)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# tests — per-subsystem. Run with ctest.
# ─────────────────────────────────────────────────────────────────────────────
enable_testing()

# Portable packaging generation is a Unix-only shell path. Register its smoke
# test when the small validation toolchain is available, while keeping ordinary
# engine builds independent of packaging-only host packages.
if(UNIX)
    find_program(GBARECOMP_BASH_EXECUTABLE bash)
    find_program(GBARECOMP_DESKTOP_FILE_VALIDATE desktop-file-validate)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(GBARECOMP_BASH_EXECUTABLE AND GBARECOMP_DESKTOP_FILE_VALIDATE
            AND Python3_Interpreter_FOUND)
        execute_process(
            COMMAND ${Python3_EXECUTABLE} -c "import yaml"
            RESULT_VARIABLE GBARECOMP_PYYAML_RESULT
            OUTPUT_QUIET ERROR_QUIET)
        if(GBARECOMP_PYYAML_RESULT EQUAL 0)
            add_test(NAME packaging_generation_checks
                COMMAND ${CMAKE_COMMAND} -E env
                    PYTHON=${Python3_EXECUTABLE}
                    ${GBARECOMP_BASH_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/packaging/tests/test_generation.sh)
        endif()
    endif()
endif()

if(GBARECOMP_ENABLE_MODS)
    add_executable(mod_runtime_tests
        tests/mod_runtime/test_main.cpp
        # The runtime's foreign-background publication API owns only the
        # pointer lifetime.  Keep this unit target independent of the full
        # PPU by providing its otherwise PPU-owned storage symbol.
        tests/mod_runtime/ppu_provider_stub.cpp
        src/runtime/mod_runtime.cpp
        src/runtime/mod_function_hooks.cpp
        src/runtime/asset_picker.cpp
        src/runtime/host_platform.cpp
        src/gba/mod_audio.cpp
        src/gba/crc32.cpp
        src/gba/sha1.cpp)
    target_include_directories(mod_runtime_tests PRIVATE
        src/runtime src/gba src/armv4t)
    if(WIN32 OR MSYS)
        target_link_libraries(mod_runtime_tests PRIVATE comdlg32 user32 winmm)
    endif()
    add_test(NAME mod_runtime_tests COMMAND mod_runtime_tests)
endif()

add_executable(mod_audio_tests
    tests/mod_audio/test_main.cpp
    src/gba/mod_audio.cpp)
target_include_directories(mod_audio_tests PRIVATE src/gba)
add_test(NAME mod_audio_tests COMMAND mod_audio_tests)

add_executable(mod_state_tests tests/mod_state/test_main.cpp)
target_include_directories(mod_state_tests PRIVATE src/debug)
target_link_libraries(mod_state_tests PRIVATE gbarecomp_debug)
add_test(NAME mod_state_tests COMMAND mod_state_tests)

add_test(NAME runtime_monolith_guard
    COMMAND ${CMAKE_COMMAND}
        -DGBARECOMP_RUNTIME_CMAKE=${CMAKE_CURRENT_BINARY_DIR}/runtime.cmake
        -DGBARECOMP_TEST_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/runtime_monolith_guard
        -DGBARECOMP_TEST_BINARY=${CMAKE_CURRENT_BINARY_DIR}/runtime_monolith_guard_test
        -DGBARECOMP_TEST_GENERATOR=${CMAKE_GENERATOR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/assert_runtime_monolith_guard.cmake)

add_executable(decoder_smoke tests/armv4t/decoder_smoke.cpp)
target_link_libraries(decoder_smoke PRIVATE gbarecomp_armv4t)
add_test(NAME decoder_smoke COMMAND decoder_smoke)

# Runtime address->name resolvers (function map + imported data-symbol map).
add_executable(symbol_lookup_smoke tests/armv4t/symbol_lookup_smoke.cpp)
target_link_libraries(symbol_lookup_smoke PRIVATE gbarecomp_armv4t)
add_test(NAME symbol_lookup_smoke COMMAND symbol_lookup_smoke)

# Overlay-config merge contract. Compiles config.cpp directly: it belongs to
# the gba_recompile tool rather than a library, and the test drives its public
# entry points (load_config / load_config_overlay) through real TOML files.
add_executable(config_overlay_smoke
    tests/recompile/config_overlay_smoke.cpp
    tools/gba_recompile/config.cpp)
target_include_directories(config_overlay_smoke PRIVATE
    tools/gba_recompile
    src/armv4t
    "${GBARECOMP_TOMLPP_INCLUDE_DIR}")
target_link_libraries(config_overlay_smoke PRIVATE
    gbarecomp_recompile_core gbarecomp_armv4t gbarecomp_gba)
add_test(NAME config_overlay_smoke COMMAND config_overlay_smoke)

# Decomp symbol importer: synthetic readelf + link-map fixtures through
# tools/symbol_import/import_decomp_symbols.py, compared against goldens.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME symbol_import_golden
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/symbol_import/run_golden.py")
else()
    message(STATUS
        "gbarecomp: no Python3 interpreter; skipping symbol_import_golden")
endif()

add_executable(function_finder_tests tests/recompile/function_finder_test.cpp)
target_link_libraries(function_finder_tests PRIVATE gbarecomp_recompile_core)
add_test(NAME function_finder_tests COMMAND function_finder_tests)

add_executable(codegen_shards_tests tests/recompile/codegen_shards_test.cpp)
target_link_libraries(codegen_shards_tests PRIVATE gbarecomp_recompile_core)
add_test(NAME codegen_shards_tests COMMAND codegen_shards_tests)

add_executable(recompile_config_tests
    tests/recompile/config_test.cpp
    tools/gba_recompile/config.cpp)
target_include_directories(recompile_config_tests PRIVATE
    tools/gba_recompile src/recompile src/armv4t
    "${GBARECOMP_TOMLPP_INCLUDE_DIR}")
target_link_libraries(recompile_config_tests PRIVATE gbarecomp_gba)
add_test(NAME recompile_config_tests COMMAND recompile_config_tests)

add_executable(audio_drc_tests tests/runtime/audio_drc_test.cpp)
target_include_directories(audio_drc_tests PRIVATE src/runtime)
add_test(NAME audio_drc_tests COMMAND audio_drc_tests)

# Self-heal proposal clustering — pure logic, links only the standalone
# clustering TU (no runtime/bus/overlay dependencies).
add_executable(selfheal_cluster_test
    tests/selfheal/cluster_test.cpp
    src/runtime/self_heal_cluster.cpp)
target_include_directories(selfheal_cluster_test PRIVATE src/runtime)
add_test(NAME selfheal_cluster_test COMMAND selfheal_cluster_test)

add_executable(interpreter_smoke tests/armv4t/interpreter_smoke.cpp)
target_link_libraries(interpreter_smoke PRIVATE gbarecomp_armv4t)
add_test(NAME interpreter_smoke COMMAND interpreter_smoke)

add_executable(armv4t_tests tests/armv4t/test_main.cpp)
target_link_libraries(armv4t_tests PRIVATE gbarecomp_armv4t)
add_test(NAME armv4t_tests COMMAND armv4t_tests)

add_executable(thumb_tests tests/thumb/test_main.cpp)
target_link_libraries(thumb_tests PRIVATE gbarecomp_armv4t)
add_test(NAME thumb_tests COMMAND thumb_tests)

add_gbarecomp_runtime_stack_test(bus_tests tests/bus/test_main.cpp)
add_gbarecomp_runtime_stack_test(dma_tests tests/dma/test_main.cpp)
add_gbarecomp_runtime_stack_test(timer_tests tests/timers/test_main.cpp)

add_executable(gpio_tests tests/gpio/test_main.cpp)
target_link_libraries(gpio_tests PRIVATE gbarecomp_gba)
add_test(NAME gpio_tests COMMAND gpio_tests)

add_executable(rtc_tests tests/rtc/test_main.cpp)
target_link_libraries(rtc_tests PRIVATE gbarecomp_gba)
add_test(NAME rtc_tests COMMAND rtc_tests)

add_executable(irq_tests tests/irq/test_main.cpp)
target_link_libraries(irq_tests PRIVATE gbarecomp_gba)
add_test(NAME irq_tests COMMAND irq_tests)

add_executable(ppu_smoke_tests tests/ppu_smoke/test_main.cpp)
target_include_directories(ppu_smoke_tests PRIVATE src/runtime src/debug)
target_link_libraries(ppu_smoke_tests PRIVATE gbarecomp_gba)
gbarecomp_target_link_host_stack(ppu_smoke_tests PRIVATE)
add_test(NAME ppu_smoke_tests COMMAND ppu_smoke_tests)

add_executable(save_config_tests
    tests/runtime/save_config_test.cpp
    src/runtime/save_config.cpp)
target_include_directories(save_config_tests PRIVATE src/runtime)
target_link_libraries(save_config_tests PRIVATE gbarecomp_gba)
add_test(NAME save_config_tests COMMAND save_config_tests)

add_executable(presentation_layout_tests tests/presentation_layout/test_main.cpp)
target_include_directories(presentation_layout_tests PRIVATE src/runtime)
add_test(NAME presentation_layout_tests COMMAND presentation_layout_tests)

# P6 differential-gate primitives (offline; full-RAM snapshot/restore/diff).
add_executable(heal_gate_tests
    tests/heal_gate/heal_gate_test.cpp
    tests/heal_gate/runtime_stubs.cpp)
target_link_libraries(heal_gate_tests PRIVATE gbarecomp_heal_gate)
add_test(NAME heal_gate_tests COMMAND heal_gate_tests)

# ─────────────────────────────────────────────────────────────────────
# codegen_tests — L1 per-IrOp diff harness.
#
# Architecture:
#   1. tests/codegen/test_cases.cpp lists per-IrOp test cases.
#   2. gen_codegen_tests.exe reads that list, invokes the same
#      ArmCodegen used by tools/gba_recompile, and emits one C
#      function per case into ${CMAKE_BINARY_DIR}/codegen_tests_generated/
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

# NOTE: must not collide with the codegen_tests TARGET name. On Windows the
# executable gains a .exe suffix so a same-named directory is harmless, but on
# macOS/Linux the linker output path and this directory are the same string and
# the link fails with "Is a directory" (errno 21).
set(CODEGEN_TESTS_OUTDIR
    ${CMAKE_CURRENT_BINARY_DIR}/codegen_tests_generated)
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
# selfheal_stage2_bios_identical — Stage-2 GFP1 byte-identity gate. Proves a
# healed-to-native BIOS function (0xBA4 exclude fixture) executes instruction-
# for-instruction identically to the fully-static corpus (static == bridge ==
# heal_cold == heal_warm). Heavy + needs the gitignored ROM/BIOS + a built
# MinishCapRecomp, so it is OPT-IN: pass -DGBARECOMP_SELFHEAL_MC_DIR=<MC dir>.
# Skips (ctest "Skipped") if the assets are absent.
# ─────────────────────────────────────────────────────────────────────────────
if(GBARECOMP_SELFHEAL_MC_DIR)
    find_program(BASH_EXE bash HINTS C:/msys64/usr/bin /usr/bin)
    if(BASH_EXE)
        add_test(NAME selfheal_stage2_bios_identical
            COMMAND ${BASH_EXE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/selfheal/stage2_bios_identical.sh)
        set_tests_properties(selfheal_stage2_bios_identical PROPERTIES
            ENVIRONMENT "GBARECOMP_ROOT=${CMAKE_CURRENT_SOURCE_DIR};MC_DIR=${GBARECOMP_SELFHEAL_MC_DIR}"
            SKIP_RETURN_CODE 77
            TIMEOUT 2400)
        # selfheal_coverage_sweep — drives the built MC from every savestate with
        # the self-heal recompiler on and asserts FULLY_STATIC for each scene
        # (the doctrine's "done"). Surfaces remaining finder gaps; doubles as a
        # coverage regression guard. Same opt-in / SKIP-77 contract.
        add_test(NAME selfheal_coverage_sweep
            COMMAND ${BASH_EXE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/selfheal/coverage_sweep.sh)
        set_tests_properties(selfheal_coverage_sweep PROPERTIES
            ENVIRONMENT "GBARECOMP_ROOT=${CMAKE_CURRENT_SOURCE_DIR};MC_DIR=${GBARECOMP_SELFHEAL_MC_DIR}"
            SKIP_RETURN_CODE 77
            TIMEOUT 2400)
    else()
        message(STATUS "gbarecomp: bash not found; skipping "
                       "selfheal_stage2_bios_identical + selfheal_coverage_sweep")
    endif()
endif()

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
