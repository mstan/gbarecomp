# Original CPU timing fixture: assemble it, recompile it through the
# production pipeline, and link only the generated code into the runner.
# Needs devkitARM (or a prebuilt ROM whose hash the recompiler verifies).
find_program(GBA_TIMING_ARM_GCC arm-none-eabi-gcc HINTS C:/devkitPro/devkitARM/bin)
find_program(GBA_TIMING_ARM_OBJCOPY arm-none-eabi-objcopy HINTS C:/devkitPro/devkitARM/bin)
set(GBA_TIMING_FIXTURE_ROM "" CACHE FILEPATH "Already assembled timing_fixture.S ROM (hash verified by recompiler)")
if(GBA_TIMING_FIXTURE_ROM OR (GBA_TIMING_ARM_GCC AND GBA_TIMING_ARM_OBJCOPY))
    set(timing_gen ${CMAKE_CURRENT_BINARY_DIR}/generated)
    file(MAKE_DIRECTORY ${timing_gen})
    if(GBA_TIMING_FIXTURE_ROM)
        set(timing_rom ${GBA_TIMING_FIXTURE_ROM})
    else()
        set(timing_rom ${timing_gen}/timing_fixture.gba)
        add_custom_command(OUTPUT ${timing_rom}
            COMMAND ${GBA_TIMING_ARM_GCC} -nostdlib -Wl,-Ttext=0x08000000
                -Wl,--entry=_start ${CMAKE_CURRENT_SOURCE_DIR}/timing_fixture.S
                -o ${timing_gen}/timing_fixture.elf
            COMMAND ${GBA_TIMING_ARM_OBJCOPY} -O binary ${timing_gen}/timing_fixture.elf ${timing_rom}
            DEPENDS timing_fixture.S VERBATIM)
    endif()
    add_custom_command(OUTPUT ${timing_gen}/dispatch_table.cpp ${timing_gen}/recompiled_000.cpp
                              ${timing_gen}/recompiled_001.cpp ${timing_gen}/recompiled.h
        COMMAND $<TARGET_FILE:gba_recompile> --rom ${timing_rom} --entry 0x08000000
            --config ${CMAKE_CURRENT_SOURCE_DIR}/timing_fixture.toml
            --codegen-shards 2 --out ${timing_gen}
        DEPENDS gba_recompile ${timing_rom} timing_fixture.toml VERBATIM)
    add_executable(timing_rom_tests timing_test.cpp ${timing_gen}/dispatch_table.cpp
        ${timing_gen}/recompiled_000.cpp ${timing_gen}/recompiled_001.cpp)
    target_include_directories(timing_rom_tests PRIVATE ${timing_gen}
        ${PROJECT_SOURCE_DIR}/src/armv4t ${PROJECT_SOURCE_DIR}/src/runtime)
    target_link_gbarecomp_runtime_stack(timing_rom_tests)
    add_test(NAME timing_rom_tests COMMAND timing_rom_tests ${timing_rom})
    set_tests_properties(timing_rom_tests PROPERTIES TIMEOUT 120)
    set(GBARECOMP_TIMING_ORACLE_EXE "" CACHE FILEPATH "Separate pinned mGBA timing reference executable (oracle/timing)")
    if(GBARECOMP_TIMING_ORACLE_EXE)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_test(NAME timing_mgba_differential COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/oracle/timing/compare.py $<TARGET_FILE:timing_rom_tests>
            ${GBARECOMP_TIMING_ORACLE_EXE} ${timing_rom})
        set_tests_properties(timing_mgba_differential PROPERTIES TIMEOUT 240)
    endif()
endif()
