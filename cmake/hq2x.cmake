option(GBARECOMP_ENABLE_HQ2X "Build the optional replaceable HQ2x presentation library" OFF)
if(GBARECOMP_ENABLE_HQ2X AND GBARECOMP_SDL2_OK)
    add_subdirectory(third_party/hqx)
    target_sources(gbarecomp_runtime PRIVATE src/runtime/hq2x_filter.cpp)
    if(GBARECOMP_ENABLE_MODS)
        target_compile_definitions(gbarecomp_runtime PRIVATE GBARECOMP_HQ2X_MOD_PLUGIN=1)
    endif()
    target_compile_definitions(gbarecomp_runtime PRIVATE GBARECOMP_ENABLE_HQ2X=1
        "GBARECOMP_HQ2X_MODULE_NAME=\"$<TARGET_FILE_NAME:gbarecomp_hqx>\"")
endif()

function(gbarecomp_stage_hq2x target)
    if(NOT TARGET gbarecomp_hqx)
        return()
    endif()
    add_dependencies(${target} gbarecomp_hqx)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:gbarecomp_hqx>" "$<TARGET_FILE_DIR:${target}>"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../third_party/hqx" "$<TARGET_FILE_DIR:${target}>/third-party/hqx"
        VERBATIM)
endfunction()

if(TARGET gbarecomp_hqx)
    add_executable(hq2x_filter_tests tests/host_input/hq2x_filter_test.cpp src/runtime/hq2x_filter.cpp)
    target_include_directories(hq2x_filter_tests PRIVATE src/runtime ${SDL2_INCLUDE_DIRS})
    target_link_libraries(hq2x_filter_tests PRIVATE ${SDL2_LIBRARIES})
    target_compile_definitions(hq2x_filter_tests PRIVATE SDL_MAIN_HANDLED
        "GBARECOMP_HQ2X_MODULE_NAME=\"$<TARGET_FILE_NAME:gbarecomp_hqx>\"")
    gbarecomp_stage_hq2x(hq2x_filter_tests)
    add_test(NAME hq2x_filter COMMAND hq2x_filter_tests)
endif()
