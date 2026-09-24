# Shared native root for gbarecomp Android games (driven by Gradle's
# externalNativeBuild, see gbarecomp-app.gradle). Builds SDL2 as a shared
# library, then the game's own CMake project, whose Android target must be
# named `main` (libmain.so is what SDLActivity loads).
cmake_minimum_required(VERSION 3.22)
project(GbarecompAndroidApp C CXX)

if(NOT GBA_GAME_ROOT OR NOT EXISTS "${GBA_GAME_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "GBA_GAME_ROOT must name the game repository root")
endif()

set(GBARECOMP_ANDROID_SDL_ROOT "${CMAKE_CURRENT_LIST_DIR}/../third_party/SDL")
if(NOT EXISTS "${GBARECOMP_ANDROID_SDL_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "SDL submodule missing: git submodule update --init platform/android/third_party/SDL")
endif()

set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${GBARECOMP_ANDROID_SDL_ROOT}" sdl_build)

add_subdirectory("${GBA_GAME_ROOT}" game_build)

if(NOT TARGET main)
    message(FATAL_ERROR "The game project must define an Android target named 'main'")
endif()
