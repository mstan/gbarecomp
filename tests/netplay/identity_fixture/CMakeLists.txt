cmake_minimum_required(VERSION 3.20)
project(optimization_identity CXX)
add_executable(identity_probe src/main.cpp "${CMAKE_CURRENT_SOURCE_DIR}/generated/recompiled_000.cpp")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/generated/recompiled_000.cpp"
    PROPERTIES COMPILE_OPTIONS "-${GENERATED_OPT};-g0")
set_property(TARGET identity_probe PROPERTY INTERPROCEDURAL_OPTIMIZATION ${USE_IPO})
include("${IDENTITY_MODULE}")
gbarecomp_target_netplay_identity(identity_probe "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/generated")
