cmake_minimum_required(VERSION 3.20)
project(gbarecomp_runtime_monolith_guard CXX)

if(NOT GBARECOMP_RUNTIME_CMAKE)
    message(FATAL_ERROR "GBARECOMP_RUNTIME_CMAKE is required")
endif()
include("${GBARECOMP_RUNTIME_CMAKE}")

gbarecomp_add_runtime_target(monolith_guard_fixture
    GAME_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated")
