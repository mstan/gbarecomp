#include "gba_high_resolution.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>

// This contract must never allocate a full extreme-resolution image. Refuse
// such a request before malloc, so an accidental eager-raster regression is
// a small test failure rather than a multi-gigabyte allocation.
void* operator new(std::size_t bytes) {
    if (bytes > 8u * 1024u * 1024u) throw std::bad_alloc();
    if (void* result = std::malloc(bytes ? bytes : 1)) return result;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
using Frame = gba::GbaHighResolutionFrame;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void deferred(unsigned scale) {
    Frame frame(scale);
    frame.set_deferred_rendering(true);
    frame.configure(240, 160);
    require(frame.width() == 240 * scale && frame.height() == 160 * scale,
            "requested dimensions changed");
    std::array<uint8_t, 240 * 3> row{};
    for (unsigned x = 0; x < row.size(); ++x) row[x] = uint8_t(x * 73);
    for (unsigned y = 0; y < 160; ++y) frame.upscale_line(y, row.data());
    frame.latch();
    require(frame.complete(), "deferred frame incomplete");
    require(frame.cpu_materializations() == 0, "deferred frame materialized CPU pixels");
    auto jobs = frame.draw_frame();
    require(jobs && jobs->scale == scale && jobs->width == 240 &&
            jobs->height == 160 && jobs->lines.size() == 160,
            "deferred descriptor mismatch");
    require(jobs->lines.front().native_rgb.size() == row.size() &&
            jobs->lines.back().native_rgb == jobs->lines.front().native_rgb,
            "deferred native rows changed");
}
}

int main() {
    try {
        static_assert(Frame::kMaxScale == 128);
        for (unsigned scale : {2u, 3u, 4u, 8u, 16u, 32u, 64u, 128u}) deferred(scale);
        for (unsigned invalid : {0u, 1u, 129u, ~0u}) {
            bool rejected = false;
            try { Frame frame(invalid); }
            catch (const std::invalid_argument&) { rejected = true; }
            require(rejected, "invalid scale accepted");
        }
        Frame maximum(128);
        maximum.set_deferred_rendering(true);
        maximum.configure(1280, 864);
        require(maximum.width() == 163840 && maximum.height() == 110592,
                "maximum metadata dimensions overflowed");
        std::puts("HD limits: 2x..128x deferred capture, requested dimensions and invalid scale rejection pass; allocations capped at 8 MiB");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "HD limits: %s\n", error.what());
        return 1;
    }
}
