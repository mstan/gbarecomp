#include "hq2x_filter.h"
#include <SDL.h>
#include <atomic>
#include <cstdio>
#include <limits>
#include <mutex>
#include <string>

// Matches upstream hqx.h's ABI, including 32-bit MinGW.
#if defined(__GNUC__) && defined(__MINGW32__)
#define GBA_HQX_CALL __stdcall
#else
#define GBA_HQX_CALL
#endif

namespace gbarecomp {
namespace {
std::atomic<bool> enabled{false};
struct Library {
    using Init = void (GBA_HQX_CALL*)();
    using Scale = void (GBA_HQX_CALL*)(const std::uint32_t*, std::uint32_t*, int, int);
    void* handle = nullptr;
    Scale scale = nullptr;
    Library() {
        char* base = SDL_GetBasePath();
        if (!base) return;
        const std::string path = std::string(base) + GBARECOMP_HQ2X_MODULE_NAME;
        SDL_free(base);
        handle = SDL_LoadObject(path.c_str());
        if (handle) {
            auto init = reinterpret_cast<Init>(SDL_LoadFunction(handle,"hqxInit"));
            scale = reinterpret_cast<Scale>(SDL_LoadFunction(handle,"hq2x_32"));
            if (init && scale) { init(); return; }
            scale = nullptr;
            SDL_UnloadObject(handle); handle = nullptr;
        }
        std::fprintf(stderr,"HQ2x: cannot load %s: %s\n",path.c_str(),SDL_GetError());
    }
    ~Library() { if (handle) SDL_UnloadObject(handle); }
};
}
bool hq2x_enabled() noexcept { return enabled.load(std::memory_order_relaxed); }
void set_hq2x_enabled(bool value) noexcept { enabled.store(value,std::memory_order_relaxed); }

const std::uint8_t* Hq2xFilter::apply(const std::uint8_t* rgb, unsigned width, unsigned height) {
    const std::uint64_t count = std::uint64_t(width)*height;
    if (!rgb || !width || !height || width > unsigned(std::numeric_limits<int>::max()/2) ||
        height > unsigned(std::numeric_limits<int>::max()/2) || count > unsigned(std::numeric_limits<int>::max()/4))
        return nullptr;
    static Library library;
    if (!library.scale) return nullptr;
    input_.resize(std::size_t(count)); output_.resize(std::size_t(count)*4);
    rgb_.resize(std::size_t(count)*12);
    for (std::size_t i=0;i<count;++i)
        input_[i]=0xff000000u | (std::uint32_t(rgb[i*3])<<16) |
            (std::uint32_t(rgb[i*3+1])<<8) | rgb[i*3+2];
    // Upstream uses global scratch YUV values. Serialize independent host
    // windows' filter calls while leaving their image buffers independent.
    static std::mutex scale_mutex;
    {
        const std::lock_guard<std::mutex> lock(scale_mutex);
        library.scale(input_.data(),output_.data(),int(width),int(height));
    }
    for (std::size_t i=0;i<count*4;++i) {
        rgb_[i*3]=std::uint8_t(output_[i]>>16);
        rgb_[i*3+1]=std::uint8_t(output_[i]>>8);
        rgb_[i*3+2]=std::uint8_t(output_[i]);
    }
    return rgb_.data();
}
}

#if defined(GBARECOMP_HQ2X_MOD_PLUGIN)
#include "mod_runtime.h"
namespace {
void reset_hq2x() { gbarecomp::set_hq2x_enabled(false); }
void activate_hq2x() { gbarecomp::set_hq2x_enabled(true); }
}
// Keep registration in the same archive object the presentation code uses.
GBA_MOD_CONSTRUCTOR(register_hq2x_plugin) {
    (void)gba_mod_register_reset_callback(reset_hq2x);
    (void)gba_mod_register_activation_plugin("gba.presentation.hq2x",activate_hq2x);
}
#endif
