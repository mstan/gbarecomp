# oracle/CMakeLists.txt — mGBA-backed reference emulator.
#
# This subtree is NEVER pulled into the main build by default; gate is
# `GBARECOMP_BUILD_ORACLE`. The oracle binary embeds libmgba (MPL-2.0)
# and stays a separate process — see ../PRINCIPLES.md "zero copyleft
# emulator dependencies in the native build".

set(MGBA_ROOT ${CMAKE_SOURCE_DIR}/third_party/mgba)
set(MGBA_BUILD ${MGBA_ROOT}/build)
set(MGBA_LIB   ${MGBA_BUILD}/libmgba.a)

if(NOT EXISTS ${MGBA_LIB})
    message(FATAL_ERROR
        "GBARECOMP_BUILD_ORACLE=ON but libmgba.a missing at ${MGBA_LIB}. "
        "Configure + build third_party/mgba/ first.")
endif()

add_executable(gbarecomp_oracle main.cpp)
target_include_directories(gbarecomp_oracle PRIVATE
    ${MGBA_ROOT}/include
    ${MGBA_BUILD}/include
)
target_link_libraries(gbarecomp_oracle PRIVATE
    ${MGBA_LIB}
)
if(WIN32 OR MSYS)
    # mGBA's config.c pulls SHGetKnownFolderPath + CoTaskMemFree for
    # locating %APPDATA%, so we need ole32 + uuid on top of the
    # basic socket libs.
    target_link_libraries(gbarecomp_oracle PRIVATE
        ws2_32 shlwapi winmm ole32 uuid)
endif()
target_compile_definitions(gbarecomp_oracle PRIVATE
    M_CORE_GBA=1
)
