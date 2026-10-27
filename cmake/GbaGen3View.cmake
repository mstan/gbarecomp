# Shared opt-in Gen III presentation adapter. Kept outside the GBA hardware
# libraries; each title supplies its reviewed profile and mod authorization.
enable_testing()
function(gbarecomp_target_gen3_view target name)
    set(gen3_dir "${GBARECOMP_ROOT}/src/presentation/gen3")
    target_sources(${target} PRIVATE
        "${gen3_dir}/field_view.cpp" "${gen3_dir}/object_view.cpp"
        "${gen3_dir}/ui_view.cpp" "${gen3_dir}/adaptive_view_plugin.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gen3_profile.cpp")
    target_include_directories(${target} PRIVATE "${gen3_dir}")
    add_executable(${target}_view_tests
        "${GBARECOMP_ROOT}/tests/gen3_view/test_main.cpp"
        "${gen3_dir}/field_view.cpp" "${gen3_dir}/object_view.cpp"
        "${gen3_dir}/ui_view.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/gen3_profile.cpp")
    target_include_directories(${target}_view_tests PRIVATE "${gen3_dir}" "${GBARECOMP_ROOT}/src/gba")
    string(TOUPPER "${name}" variant_name)
    target_compile_definitions(${target}_view_tests PRIVATE GEN3_VARIANT_${variant_name}=1)
    target_link_libraries(${target}_view_tests PRIVATE gbarecomp_gba)
    add_test(NAME ${target}_view_tests COMMAND ${target}_view_tests)
    add_executable(${target}_view_capture_check
        "${GBARECOMP_ROOT}/tools/gen3_view_capture_check.cpp"
        "${gen3_dir}/field_view.cpp" "${gen3_dir}/object_view.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gen3_profile.cpp")
    target_include_directories(${target}_view_capture_check PRIVATE "${gen3_dir}" "${GBARECOMP_ROOT}/src/gba")
    target_compile_definitions(${target}_view_capture_check PRIVATE GEN3_VARIANT_${variant_name}=1)
    target_link_libraries(${target}_view_capture_check PRIVATE gbarecomp_gba)
endfunction()
