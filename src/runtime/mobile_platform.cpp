// mobile_platform.cpp — see mobile_platform.h.

#include "mobile_platform.h"

#include <cstdio>
#include <cstdlib>

#if defined(__ANDROID__)
#include <pthread.h>
#include <unistd.h>

#include <SDL.h>
#include <SDL_system.h>
#endif

namespace gbarecomp {

#if defined(__ANDROID__)
namespace {
struct StackedCall {
    int (*entry)(int, char**) = nullptr;
    int argc = 0;
    char** argv = nullptr;
    int result = 1;
};
void* run_stacked(void* arg) {
    auto* call = static_cast<StackedCall*>(arg);
    call->result = call->entry(call->argc, call->argv);
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

bool mobile_prepare_process(std::vector<std::string>& args,
                            const MobileProcessOptions& options) {
#if defined(__ANDROID__)
    if (const char* storage = SDL_AndroidGetInternalStoragePath())
        (void)chdir(storage);
    const char* log = options.log_file ? options.log_file : "android-runtime.log";
    std::freopen(log, "w", stderr);
    std::freopen(log, "a", stdout);
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
