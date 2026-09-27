# Optional original ARM fixture: build a real ROM, recompile it through the
# production pipeline, and link only the generated code into the native test.
find_program(GBA_LINK_ARM_GCC arm-none-eabi-gcc HINTS C:/devkitPro/devkitARM/bin)
find_program(GBA_LINK_ARM_OBJCOPY arm-none-eabi-objcopy HINTS C:/devkitPro/devkitARM/bin)
set(GBA_LINK_FIXTURE_ROM "" CACHE FILEPATH "Already assembled original link_fixture.S ROM (hash verified by recompiler)")
if(GBA_LINK_FIXTURE_ROM OR (GBA_LINK_ARM_GCC AND GBA_LINK_ARM_OBJCOPY))
    set(link_gen ${CMAKE_CURRENT_BINARY_DIR}/generated)
    file(MAKE_DIRECTORY ${link_gen})
    if(GBA_LINK_FIXTURE_ROM)
        set(link_rom ${GBA_LINK_FIXTURE_ROM})
    else()
        set(link_rom ${link_gen}/link_fixture.gba)
        add_custom_command(OUTPUT ${link_rom}
            COMMAND ${GBA_LINK_ARM_GCC} -nostdlib -Wl,-Ttext=0x08000000
                -Wl,--entry=_start ${CMAKE_CURRENT_SOURCE_DIR}/link_fixture.S
                -o ${link_gen}/link_fixture.elf
            COMMAND ${GBA_LINK_ARM_OBJCOPY} -O binary ${link_gen}/link_fixture.elf ${link_rom}
            DEPENDS link_fixture.S VERBATIM)
    endif()
    add_custom_command(OUTPUT ${link_gen}/dispatch_table.cpp ${link_gen}/recompiled_000.cpp
                              ${link_gen}/recompiled_001.cpp ${link_gen}/recompiled.h
        COMMAND $<TARGET_FILE:gba_recompile> --rom ${link_rom} --entry 0x08000000
            --config ${CMAKE_CURRENT_SOURCE_DIR}/link_fixture.toml
            --codegen-shards 2 --out ${link_gen}
        DEPENDS gba_recompile ${link_rom} link_fixture.toml VERBATIM)
    add_executable(link_rom_tests rom_test.cpp ${link_gen}/dispatch_table.cpp
        ${link_gen}/recompiled_000.cpp ${link_gen}/recompiled_001.cpp)
    target_include_directories(link_rom_tests PRIVATE ${link_gen} ${PROJECT_SOURCE_DIR}/src/armv4t)
    target_link_gbarecomp_runtime_stack(link_rom_tests)
    add_test(NAME link_rom_tests COMMAND link_rom_tests ${link_rom})
    set_tests_properties(link_rom_tests PROPERTIES TIMEOUT 30)
    set(GBARECOMP_LINK_ORACLE_EXE "" CACHE FILEPATH "Separate pinned mGBA link reference executable")
    if(GBARECOMP_LINK_ORACLE_EXE)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_test(NAME link_mgba_differential COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/oracle/link/compare.py $<TARGET_FILE:link_rom_tests>
            ${GBARECOMP_LINK_ORACLE_EXE} ${link_rom})
        set_tests_properties(link_mgba_differential PROPERTIES TIMEOUT 45)
    endif()
endif()
