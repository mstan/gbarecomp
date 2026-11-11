#include "gba_ppu.h"
#include "gba_ui_layers.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace {
unsigned seed = 0x143ac951;
unsigned random_word() { seed ^= seed << 13; seed ^= seed >> 17; return seed ^= seed << 5; }
unsigned u16(const unsigned char* p) { return p[0] | (unsigned(p[1]) << 8); }
void put(unsigned char* p, unsigned v) { p[0] = v; p[1] = v >> 8; }
void check(bool pass, const char* message, unsigned scene = 0, unsigned pixel = 0) {
    if (!pass) { std::fprintf(stderr, "UI layers: %s scene=%u pixel=%u\n", message, scene, pixel); std::exit(1); }
}
constexpr unsigned width = 240, height = 160, pixels = width * height;
}

int main() {
    std::array<unsigned char, 0x400> io{}, palette{}, oam{};
    std::vector<unsigned char> vram(0x18000), reference_ui(pixels * 3), reference_world(pixels * 3);
    std::vector<unsigned char> ui(pixels * 3), world(pixels * 3), opacity(pixels);
    auto reference = std::make_unique<gba::GbaPpu>();
    const std::array<gba::GbaUiLayerJob::Rect, 3> clipped{{{-7, -3, 93, 72}, {50, 40, 114, 93}, {197, 101, 43, 59}}};
    const std::array<gba::GbaUiLayerJob::Rect, 1> full{{{0, 0, 240, 160}}};
    for (unsigned scene = 0; scene < 96; ++scene) {
        for (auto& b : io) b = random_word();
        for (auto& b : vram) b = random_word();
        for (auto& b : palette) b = random_word();
        for (auto& b : oam) b = random_word();
        const unsigned display = 0x1f40 | ((scene & 7) << 13);
        put(io.data(), display);
        put(io.data() + 8, random_word() & ~0xc080u); // native 4bpp/32x32 BG0
        // Explicit normal, semitransparent, OBJ-window and affine cursor pieces.
        for (unsigned object = 0; object < 8; ++object) {
            const unsigned a0 = 40 + object * 7 | ((object & 3) << 10) |
                ((scene & 1) ? 0x100 : 0) | ((scene & 2) ? 0x200 : 0);
            put(oam.data() + object * 8, a0);
            put(oam.data() + object * 8 + 2, (40 + object * 13) | ((object & 3) << 14));
            put(oam.data() + object * 8 + 4, (object * 6) | ((object & 3) << 10));
        }
        const auto io_before = io, palette_before = palette, oam_before = oam;
        const auto vram_before = vram;
        const auto regions = scene & 1 ? std::span<const gba::GbaUiLayerJob::Rect>(clipped) :
                                        std::span<const gba::GbaUiLayerJob::Rect>(full);
        gba::GbaUiLayerJob job{io, vram, oam, palette, regions};
        job.world_hidden_layers = 1;
        job.world_obj_window = false;
        for (auto& disabled : job.world_disable) disabled = (random_word() & 3) == 0;
        for (unsigned object = 0; object < 8; ++object) job.world_disable[object] = true;
        std::fill(ui.begin(), ui.end(), 0xa5);
        std::fill(world.begin(), world.end(), 0xa5);
        std::fill(opacity.begin(), opacity.end(), 0xa5);
        check(gba::render_ui_layers(job, {ui, world, opacity}), "admitted input declined", scene);
        reference->render(reference_ui.data(), display, io.data(), vram.data(), oam.data(), palette.data());
        auto world_oam = oam;
        for (unsigned object = 0; object < 128; ++object)
            if (job.world_disable[object]) world_oam[object * 8 + 1] |= 2;
        reference->render(reference_world.data(), display & ~0x8100u,
            io.data(), vram.data(), world_oam.data(), palette.data());
        const unsigned cnt = u16(io.data() + 8), screen = ((cnt >> 8) & 31) * 0x800;
        const unsigned chars = ((cnt >> 2) & 3) * 0x4000;
        for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
            const unsigned pixel = y * width + x;
            bool needed = false;
            for (const auto& r : regions)
                needed |= int(x) >= r.x && int(x) < r.x + r.width && int(y) >= r.y && int(y) < r.y + r.height;
            if (!needed) {
                check(opacity[pixel] == 0xa5 && ui[pixel * 3] == 0xa5 && world[pixel * 3] == 0xa5,
                      "outside-region output modified", scene, pixel);
                continue;
            }
            check(std::equal(ui.data() + pixel * 3, ui.data() + pixel * 3 + 3, reference_ui.data() + pixel * 3),
                  "UI pixels differ", scene, pixel);
            check(std::equal(world.data() + pixel * 3, world.data() + pixel * 3 + 3, reference_world.data() + pixel * 3),
                  "world pixels differ", scene, pixel);
            const unsigned tx = (x + u16(io.data() + 0x10)) & 255, ty = (y + u16(io.data() + 0x12)) & 255;
            const unsigned entry = u16(vram.data() + screen + ((ty / 8) * 32 + tx / 8) * 2);
            const unsigned px = entry & 0x400 ? 7 - (tx & 7) : tx & 7;
            const unsigned py = entry & 0x800 ? 7 - (ty & 7) : ty & 7;
            const unsigned offset = chars + (entry & 1023) * 32 + py * 4 + px / 2;
            const bool opaque = offset < 0x10000 && ((vram[offset] >> ((px & 1) * 4)) & 15);
            check(bool(opacity[pixel]) == opaque, "BG0 opacity differs", scene, pixel);
        }
        check(io == io_before && vram == vram_before && oam == oam_before && palette == palette_before,
              "source memory changed", scene);
    }
    gba::GbaUiLayerJob invalid{io, vram, oam, palette, full};
    put(io.data(), 0x80); // Refuse unsupported snapshots before touching output.
    const auto saved = ui;
    check(!gba::render_ui_layers(invalid, {ui, world, opacity}) && ui == saved, "invalid input mutated output");
    put(io.data(), 0x9f40);
    put(oam.data(), 0x0800); // Changed OBJ-window needs independent mask preparation.
    invalid.world_disable[0] = true;
    check(!gba::render_ui_layers(invalid, {ui, world, opacity}) && ui == saved,
          "unsupported OBJ-window variant mutated output");
    std::puts("PASS: 96 shared UI/world jobs, windows/cursor variants, clipping, mutation and opacity");
}
