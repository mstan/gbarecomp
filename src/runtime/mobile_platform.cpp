// mobile_platform.cpp — see mobile_platform.h.

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE  // pthread_getattr_np for the desktop stack guard
#endif

#include "mobile_platform.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#if defined(__linux__)
#include <pthread.h>
#endif

#if defined(__ANDROID__)
#include <pthread.h>
#include <unistd.h>

#include <SDL.h>
#include <SDL_system.h>
#endif

namespace gbarecomp {

// Host-stack budget guard state (see mobile_stack_over_budget). File-local;
// armed per thread by run_stacked (mobile) or
// host_stack_guard_arm_current (desktop finite-stack threads).
static thread_local std::uintptr_t g_stack_guard_lo = 0;
// Critical tier: fires even inside a live IRQ handler (which normally gates the
// guard off) when the stack is so low that the handler cannot possibly complete
// anyway. Covers the stuck-nest-depth state (g_irq_nest_depth never returns to
// 0 after a guest-return/IRQ interleave), where the primary gate would
// otherwise leave the nesting unbounded.
static thread_local std::uintptr_t g_stack_guard_crit_lo = 0;

#if defined(__ANDROID__)
namespace {
struct StackedCall {
    int (*entry)(int, char**) = nullptr;
    int argc = 0;
    char** argv = nullptr;
    std::size_t stack_bytes = 0;
    int result = 1;
};
void* run_stacked(void* arg) {
    auto* call = static_cast<StackedCall*>(arg);
    // Arm this thread's budget: keep kGuardMargin of headroom for the
    // runtime's own transient frames between two instruction-boundary
    // checks, then unwind through the standard yield path.
    constexpr std::size_t kGuardMargin = 32u * 1024u * 1024u;
    host_stack_guard_arm_current(kGuardMargin);
    call->result = call->entry(call->argc, call->argv);
    g_stack_guard_lo = 0;
    g_stack_guard_crit_lo = 0;
    return nullptr;
}
}  // namespace
#endif

int mobile_run_with_stack(int (*entry)(int, char**), int argc, char** argv,
                          std::size_t stack_bytes) {
#if defined(__ANDROID__)
    StackedCall call;
    call.entry = entry;
    call.argc = argc;
    call.argv = argv;
    call.stack_bytes = stack_bytes;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    // Reserved address space only; pages are committed as the stack grows.
    const bool sized = pthread_attr_setstacksize(&attr, stack_bytes) == 0;
    pthread_t thread;
    if (pthread_create(&thread, &attr, run_stacked, &call) != 0) {
        pthread_attr_destroy(&attr);
        std::fprintf(stderr, "mobile: could not create the %zu-byte game thread; "
                     "running on the SDL thread\n", stack_bytes);
        return entry(argc, argv);
    }
    pthread_attr_destroy(&attr);
    std::fprintf(stderr, "mobile: game thread stack=%zu bytes%s\n", stack_bytes,
                 sized ? "" : " (size request rejected)");
    pthread_join(thread, nullptr);
    return call.result;
#else
    (void)stack_bytes;
    return entry(argc, argv);
#endif
}

bool mobile_stack_over_budget(const void* probe) {
    if (g_stack_guard_lo == 0) return false;
    return reinterpret_cast<std::uintptr_t>(probe) < g_stack_guard_lo;
}

bool host_stack_critically_low(const void* probe) {
    if (g_stack_guard_crit_lo == 0) return false;
    return reinterpret_cast<std::uintptr_t>(probe) < g_stack_guard_crit_lo;
}

// Desktop (and any pthread) arming: arm the guard for the CALLING thread
// from its real stack bounds, unwinding once the probe comes within
// `reserve_bytes` of the stack bottom. Used for the finite-stack TCP game
// thread, where the split vblank-wait loop's per-iteration host calls can
// nest past the default 8 MiB (see runtime_bus_bridge.cpp guard block).
void host_stack_guard_arm_current(std::size_t reserve_bytes) {
    g_stack_guard_lo = 0;
    g_stack_guard_crit_lo = 0;
#if defined(__linux__)
    pthread_attr_t attr;
    void*  base = nullptr;
    std::size_t size = 0;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        if (pthread_attr_getstack(&attr, &base, &size) == 0 && base != nullptr) {
            const std::uintptr_t lo = reinterpret_cast<std::uintptr_t>(base);
            const std::size_t margin =
                reserve_bytes < size ? reserve_bytes : size / 2u;
            g_stack_guard_lo = lo + margin;
            const std::size_t crit =
                (size / 16u) < 512u * 1024u ? size / 16u : 512u * 1024u;
            g_stack_guard_crit_lo = lo + crit;
        }
        pthread_attr_destroy(&attr);
    }
#else
    (void)reserve_bytes;
#endif
}

bool mobile_prepare_process(std::vector<std::string>& args,
                            const MobileProcessOptions& options) {
#if defined(__ANDROID__)
    if (const char* storage = SDL_AndroidGetInternalStoragePath())
        (void)chdir(storage);
    const char* log = options.log_file ? options.log_file : "android-runtime.log";
    // One open file description for both streams: separate freopen()s would
    // give stderr and stdout independent offsets, and each would overwrite
    // the other's lines in the shared log.
    std::freopen(log, "w", stderr);
    std::fflush(stdout);
    dup2(fileno(stderr), fileno(stdout));
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // No compiler on the device: misses bridge loudly and are reported for
    // the offline heal loop instead of attempting an on-device rebuild.
    setenv("GBARECOMP_SELFHEAL_RECOMPILE", "0", 0);
    std::fprintf(stderr,
                 "mobile: storage=%s selfheal=report-only "
                 "(dispatch misses bridge loudly -> recomp_master_misses.toml.frag)\n",
                 SDL_AndroidGetInternalStoragePath()
                     ? SDL_AndroidGetInternalStoragePath() : "(unknown)");
    if (args.empty()) args.emplace_back("main");
    if (options.program_name) args[0] = options.program_name;
    if (options.game_config) args.emplace_back(options.game_config);
    args.emplace_back("--no-launcher");
    // Validation hook: extra runtime arguments (e.g. "--tcp-observe 19892")
    // from an app-private file only adb run-as can create. Absent in normal
    // installs; every argument taken is logged.
    if (std::FILE* f = std::fopen("debug-args.txt", "r")) {
        char word[256];
        while (std::fscanf(f, "%255s", word) == 1) {
            args.emplace_back(word);
            std::fprintf(stderr, "mobile: debug arg %s\n", word);
        }
        std::fclose(f);
    }
    return true;
#else
    (void)args;
    (void)options;
    return false;
#endif
}

}  // namespace gbarecomp
