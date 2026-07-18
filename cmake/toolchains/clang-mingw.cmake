# Clang frontend with the MSYS2 MinGW headers, libraries, and GNU ABI.
# Configure a game from PowerShell with:
#   cmake -S . -B build-clang -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=<gbarecomp>/cmake/toolchains/clang-mingw.cmake

set(CMAKE_SYSTEM_NAME Windows)

set(GBARECOMP_LLVM_ROOT "C:/Program Files/LLVM" CACHE PATH
    "Root of the LLVM for Windows installation")
set(GBARECOMP_MINGW_ROOT "C:/msys64/mingw64" CACHE PATH
    "Root of the MSYS2 MinGW64 installation")

set(CMAKE_C_COMPILER "${GBARECOMP_LLVM_ROOT}/bin/clang.exe" CACHE FILEPATH "")
set(CMAKE_CXX_COMPILER "${GBARECOMP_LLVM_ROOT}/bin/clang++.exe" CACHE FILEPATH "")
set(CMAKE_C_COMPILER_TARGET x86_64-w64-windows-gnu CACHE STRING "")
set(CMAKE_CXX_COMPILER_TARGET x86_64-w64-windows-gnu CACHE STRING "")
set(CMAKE_RC_COMPILER "${GBARECOMP_MINGW_ROOT}/bin/windres.exe" CACHE FILEPATH "")
# LLVM's standalone Windows package finds MinGW headers/libraries for the GNU
# target, but does not add MinGW's POSIX thread runtime automatically.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-pthread")

if(NOT EXISTS "${CMAKE_CXX_COMPILER}")
    message(FATAL_ERROR
        "Clang not found at ${CMAKE_CXX_COMPILER}. Install LLVM for Windows "
        "or set GBARECOMP_LLVM_ROOT before configuring.")
endif()
if(NOT EXISTS "${GBARECOMP_MINGW_ROOT}/lib")
    message(FATAL_ERROR
        "MSYS2 MinGW64 not found at ${GBARECOMP_MINGW_ROOT}. Set "
        "GBARECOMP_MINGW_ROOT before configuring.")
endif()
