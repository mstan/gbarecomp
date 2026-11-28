# Shared by add_subdirectory consumers and the exported runtime.cmake.
include_guard(GLOBAL)

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
    set(_reserve "${GBARECOMP_HOST_STACK_RESERVE_BYTES}")
    if(ARGC GREATER 2)
        set(_reserve "${ARGV2}")
    endif()
    gbarecomp_compiler_uses_msvc_linker(_gbarecomp_msvc_linker)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC" OR
       CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        target_link_options(${target_name} ${scope}
            "/STACK:${_reserve}")
    elseif(CMAKE_EXECUTABLE_SUFFIX STREQUAL ".exe" AND
           _gbarecomp_msvc_linker)
        # MSVC-style Clang/lld-link does not understand GNU --stack. Route
        # through CMake's linker wrapper so the compiler driver forwards the
        # Windows /STACK form to the linker.
        target_link_options(${target_name} ${scope}
            "LINKER:/STACK:${_reserve}")
    elseif(CMAKE_EXECUTABLE_SUFFIX STREQUAL ".exe" AND
           (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" OR
            CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
        target_link_options(${target_name} ${scope}
            "LINKER:--stack,${_reserve}")
    endif()
endfunction()

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
            ${ARGN} gbarecomp_armv4t gbarecomp_gba gbarecomp_runtime gbarecomp_debug)
    else()
        target_link_libraries(${target_name} PRIVATE
            -Wl,--start-group
            ${ARGN} gbarecomp_armv4t gbarecomp_gba gbarecomp_runtime gbarecomp_debug
            -Wl,--end-group)
    endif()
endfunction()

# Retain all generated support units, but reject the obsolete cartridge monolith.
# Title-owned shard optimization constraints remain with the consuming target.
function(gbarecomp_collect_generated_sources out_var)
    cmake_parse_arguments(GG "" "DIRECTORY" "" ${ARGN})
    if(NOT GG_DIRECTORY)
        message(FATAL_ERROR "gbarecomp_collect_generated_sources requires DIRECTORY")
    endif()
    if(EXISTS "${GG_DIRECTORY}/recompiled.cpp")
        message(FATAL_ERROR "stale monolithic ${GG_DIRECTORY}/recompiled.cpp found; cartridge monoliths are no longer supported. Regenerate with gba_recompile.")
    endif()
    file(GLOB _generated CONFIGURE_DEPENDS
        "${GG_DIRECTORY}/*.c" "${GG_DIRECTORY}/*.cpp")
    file(GLOB _shards CONFIGURE_DEPENDS "${GG_DIRECTORY}/recompiled_[0-9]*.cpp")
    list(LENGTH _shards _count)
    if(_count LESS 2 OR NOT EXISTS "${GG_DIRECTORY}/dispatch_table.cpp")
        message(FATAL_ERROR "Expected sharded cartridge and dispatch_table.cpp in ${GG_DIRECTORY}; regenerate with gba_recompile.")
    endif()
    set(${out_var} ${_generated} PARENT_SCOPE)
endfunction()

# Apply common runtime includes and archive-cycle handling to an existing target.
# The caller still owns executable/shared-library kind, custom sources and flags.
function(gbarecomp_target_runtime target_name)
    if(GBARECOMP_CORE_DIR)
        set(_core "${GBARECOMP_CORE_DIR}")
    else()
        set(_core "${GBARECOMP_ROOT}")
    endif()
    target_include_directories(${target_name} PRIVATE
        "${_core}/src/armv4t" "${_core}/src/gba"
        "${_core}/src/runtime" "${_core}/src/debug")
    target_link_gbarecomp_runtime_stack(${target_name} ${ARGN})
endfunction()
