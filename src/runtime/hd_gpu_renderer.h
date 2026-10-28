#pragma once
#include "gba_high_resolution.h"
#include <memory>
#include <vector>
struct SDL_Renderer;
struct SDL_Texture;
namespace gbarecomp {
namespace runtime { class ColorLut; }

// One optional presentation consumer. All GL symbols are loaded from SDL and
// unsupported contexts retain the portable DrawFrame renderer. No guest state
// or live guest memory is read here. Destroy before the owning SDL renderer.
class HdGpuRenderer {
public:
    HdGpuRenderer();
    ~HdGpuRenderer();
    bool initialize(SDL_Renderer*, SDL_Texture* activation_texture);
    bool available() const;
    SDL_Texture* render(const gba::GbaHighResolutionFrame::DrawFrame&,
                        const runtime::ColorLut*);
    bool readback(std::vector<uint8_t>& rgb, unsigned& width, unsigned& height);
    unsigned width() const;
    unsigned height() const;
    void reset(bool context_lost = false);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace gbarecomp
