// mobile_platform.h — process setup shared by gbarecomp mobile (Android) games.
//
// The Android shell (platform/android) installs the game's payload into the
// app's private files directory and starts SDL's native main with no
// arguments. mobile_prepare_process() turns that into the same relative
// layout the desktop runtime uses, so games need no Android path plumbing:
//
//   * chdir() to the internal storage root,
//   * redirect stdout/stderr to android-runtime.log there (logcat does not
//     reliably carry native stdio),
//   * append the staged per-game TOML and --no-launcher to the arguments,
//   * default GBARECOMP_SELFHEAL_RECOMPILE=0: a device has no toolchain, so
//     dispatch misses are bridged by the interpreter LOUDLY, logged to the
//     miss list/coverage report in the files directory, and fed back offline.
//     The statically recompiled code remains the execution path.
//
// On every other platform it does nothing and returns false.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace gbarecomp {

struct MobileProcessOptions {
    const char* game_config = nullptr;   // e.g. "variants/emerald/game.toml"
    const char* program_name = nullptr;  // argv[0] stand-in, e.g. "./EmeraldRecomp"
    const char* log_file = "android-runtime.log";
};

bool mobile_prepare_process(std::vector<std::string>& args,
                            const MobileProcessOptions& options);

// Run `entry(argc, argv)` on a thread with a `stack_bytes` stack and return
// its result. Recompiled guest code maps guest calls (and some tail-call
// loops) onto the host call stack, so game executables reserve a large stack
// (desktop links 256 MiB). SDL's Android main thread is a default Java thread
// (~1 MiB) — far too small — so mobile entry points run the game on a native
// thread sized like the desktop reservation. Elsewhere this calls directly.
constexpr std::size_t kMobileGameStackBytes = 256u * 1024u * 1024u;
int mobile_run_with_stack(int (*entry)(int, char**), int argc, char** argv,
                          std::size_t stack_bytes = kMobileGameStackBytes);

// True when built for a mobile platform (compile-time).
constexpr bool mobile_platform() {
#if defined(__ANDROID__)
    return true;
#else
    return false;
#endif
}

}  // namespace gbarecomp
