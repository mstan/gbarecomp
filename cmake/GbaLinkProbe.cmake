# Opt-in headless qualification using a game's existing generated native code.
# Never fetches images or writes cartridge saves.
set(GBARECOMP_LINK_PROBE_GENERATED "" CACHE PATH "Generated game sources for the local-link probe")
set(GBARECOMP_LINK_PROBE_SHA1 "" CACHE STRING "Expected ROM SHA-1 for those generated sources")
set(GBARECOMP_LINK_PROBE_PROGRAM "" CACHE STRING "Program/build identity for the link probe")
set(GBARECOMP_LINK_PROBE_SETUP_SOURCE "" CACHE FILEPATH "Optional game-specific instance setup for the probe")
set(GBARECOMP_LINK_PROBE_OPTIMIZATION "0" CACHE STRING "Generated probe optimization level (0 for quick builds; 1 matches game builds)")
if(NOT GBARECOMP_LINK_PROBE_OPTIMIZATION MATCHES "^[0123]$")
    message(FATAL_ERROR "Link probe optimization must be 0, 1, 2 or 3")
endif()
if(GBARECOMP_LINK_PROBE_GENERATED)
    string(LENGTH "${GBARECOMP_LINK_PROBE_SHA1}" probe_hash_length)
    if(NOT probe_hash_length EQUAL 40 OR NOT GBARECOMP_LINK_PROBE_SHA1 MATCHES "^[0-9a-f]+$"
            OR NOT GBARECOMP_LINK_PROBE_PROGRAM)
        message(FATAL_ERROR "Link probe requires a lowercase ROM SHA-1 and program/build identity")
    endif()
    if(NOT EXISTS "${GBARECOMP_LINK_PROBE_GENERATED}/dispatch_table.cpp")
        message(FATAL_ERROR "Link probe needs generated dispatch_table.cpp")
    endif()
    file(GLOB probe_generated CONFIGURE_DEPENDS "${GBARECOMP_LINK_PROBE_GENERATED}/*.cpp")
    add_executable(gba_link_probe tools/link_session/main.cpp ${probe_generated})
    if(GBARECOMP_LINK_PROBE_SETUP_SOURCE)
        target_sources(gba_link_probe PRIVATE ${GBARECOMP_LINK_PROBE_SETUP_SOURCE})
        target_compile_definitions(gba_link_probe PRIVATE GBA_LINK_PROBE_SETUP=1)
    endif()
    target_compile_definitions(gba_link_probe PRIVATE
        GBA_LINK_PROBE_SHA1="${GBARECOMP_LINK_PROBE_SHA1}"
        GBA_LINK_PROBE_PROGRAM="${GBARECOMP_LINK_PROBE_PROGRAM}")
    # Qualification builds favor compile time; the runtime itself retains the
    # selected build-type optimization. Do not change game project source flags.
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        set_source_files_properties(${probe_generated} PROPERTIES COMPILE_OPTIONS "-O${GBARECOMP_LINK_PROBE_OPTIMIZATION};-g0;-w")
    endif()
    target_link_gbarecomp_runtime_stack(gba_link_probe)
endif()
