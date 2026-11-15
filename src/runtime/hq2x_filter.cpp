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
std::atomic<unsigned> selected_scale{0};
struct Library {
    using Init = void (GBA_HQX_CALL*)();
    using Scale = void (GBA_HQX_CALL*)(const std::uint32_t*, std::uint32_t*, int, int);
    void* handle = nullptr;
    Scale scales[3]{};
    std::once_flag missing_scale[3];
    Library() {
        char* base = SDL_GetBasePath();
        if (!base) return;
        const std::string path = std::string(base) + GBARECOMP_HQ2X_MODULE_NAME;
        SDL_free(base);
        handle = SDL_LoadObject(path.c_str());
        if (handle) {
            auto init = reinterpret_cast<Init>(SDL_LoadFunction(handle,"hqxInit"));
            scales[0] = reinterpret_cast<Scale>(SDL_LoadFunction(handle,"hq2x_32"));
            scales[1] = reinterpret_cast<Scale>(SDL_LoadFunction(handle,"hq3x_32"));
            scales[2] = reinterpret_cast<Scale>(SDL_LoadFunction(handle,"hq4x_32"));
            if (init && scales[0]) { init(); return; }
            for (auto& scale : scales) scale = nullptr;
            SDL_UnloadObject(handle); handle = nullptr;
        }
        std::fprintf(stderr,"HQ2x: cannot load %s: %s\n",path.c_str(),SDL_GetError());
    }
    ~Library() { if (handle) SDL_UnloadObject(handle); }
};
}
unsigned hqx_scale() noexcept { return selected_scale.load(std::memory_order_relaxed); }
bool set_hqx_scale(unsigned scale) noexcept {
    if (scale != 0 && scale != 2 && scale != 3 && scale != 4) return false;
    selected_scale.store(scale, std::memory_order_relaxed);
    return true;
}
bool hq2x_enabled() noexcept { return hqx_scale() == 2; }
void set_hq2x_enabled(bool value) noexcept { (void)set_hqx_scale(value ? 2 : 0); }

const std::uint8_t* Hq2xFilter::apply(const std::uint8_t* rgb, unsigned width, unsigned height,
                                    unsigned scale) {
    if (scale < 2 || scale > 4) return nullptr;
    const std::uint64_t count = std::uint64_t(width)*height;
    const std::uint64_t factor = std::uint64_t(scale)*scale;
    // Bound both upstream's signed row arithmetic and our allocation products,
    // including 32-bit hosts, before allocating or reading any source pixels.
    if (!rgb || !width || !height ||
        count > std::uint64_t(std::numeric_limits<int>::max())/(4*factor) ||
        count > std::uint64_t(std::numeric_limits<std::size_t>::max())/(4*factor))
        return nullptr;
    static Library library;
    const auto scaler = library.scales[scale-2];
    if (!scaler) {
        std::call_once(library.missing_scale[scale-2], [scale] {
            std::fprintf(stderr,"Pixel smoothing: HQ%ux is unavailable; using normal pixels.\n",scale);
        });
        return nullptr;
    }
    input_.resize(std::size_t(count)); output_.resize(std::size_t(count*factor));
    rgb_.resize(std::size_t(count*factor)*3);
    for (std::size_t i=0;i<count;++i)
        input_[i]=0xff000000u | (std::uint32_t(rgb[i*3])<<16) |
            (std::uint32_t(rgb[i*3+1])<<8) | rgb[i*3+2];
    // Upstream uses global scratch YUV values. Serialize independent host
    // windows' filter calls while leaving their image buffers independent.
    static std::mutex scale_mutex;
    {
        const std::lock_guard<std::mutex> lock(scale_mutex);
        scaler(input_.data(),output_.data(),int(width),int(height));
    }
    for (std::size_t i=0;i<count*factor;++i) {
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
