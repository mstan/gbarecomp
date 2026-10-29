// A portable, deterministic input corpus for separate original/candidate
// compositor executables. Emit full canonical bytes at partial rows and the
// latched boundary; the host runner performs exact comparisons, not a second
// implementation of pixel ordering. This target is correctness-only.
#include "gba_ppu.h"
#include "gba_bus.h"
#include "gba_simulation_state.h"
#include "foreign_presentation_internal.h"
#include "snapshot.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <vector>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#if defined(GBARECOMP_ORDERED_SCANLINE_EXPERIMENT) && defined(GBARECOMP_ORDERED_DRAW_TEST_COVERAGE)
extern "C" uint64_t gba_ordered_draw_test_rows();
#endif

int main() {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    unsigned long long emitted = 0;
    for (unsigned test = 0; test < 104; ++test) {
        unsigned seed = 0x73bd48f1u + test * 0x3187au;
        auto next = [&] { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
        auto bus = std::make_unique<gba::GbaBus>();
        bus->rtc().set_emulated_clock(0);
        auto vram = std::span(bus->vram_ptr(), 98304);
        auto pal = std::span(bus->pal_ptr(), 1024);
        auto oam = std::span(bus->oam_ptr(), 1024);
        std::array<unsigned char, 1024> io{};
        auto w16 = [&](unsigned a, unsigned v) { io[a] = v; io[a+1] = v >> 8; };
        auto w32 = [&](unsigned a, unsigned v) { w16(a,v); w16(a+2,v>>16); };
        auto ow16 = [&](unsigned a, unsigned v) { oam[a] = v; oam[a+1] = v >> 8; };
        for (auto& value : vram) value = next();
        for (auto& value : pal) value = next();
        for (unsigned layer = 0; layer < 4; ++layer) {
            w16(8+layer*2,next());
            w16(0x10+layer*4,next()); w16(0x12+layer*4,next());
        }
        for (unsigned a = 0x40; a <= 0x54; a += 2) w16(a,next());
        // Explicitly include no effects, alpha, brightness, saturation and
        // target-bit combinations; independent random windows remain active.
        w16(0x50, (test % 4) * 0x40 | (next() & 0x3f3f));
        if (test % 8 == 0) w16(0x50, 0);
        if (test % 8 == 1) w16(0x50, 0x3f7f);
        w16(0x52, test % 9 == 0 ? 0x1000 : next());
        for (unsigned index = 0; index < 128; ++index) {
            unsigned a0 = (next() % 256) | ((next() % 3) << 14) |
                ((next() & 1) << 13) | ((next() % 4) << 10);
            if (index % 3 == 0) a0 |= 0x100;
            if (index % 7 == 0) a0 |= 0x200;
            if (test % 6 == 0 && index >= 8) a0 = 0x200;
            const unsigned a1 = (next() % 512) | (next() & 0xfe00);
            ow16(index*8,a0); ow16(index*8+2,a1); ow16(index*8+4,next());
        }
        for (unsigned group = 0; group < 32; ++group) {
            ow16(group*32+6, 256+int(next()%257)-128);
            ow16(group*32+14, int(next()%257)-128);
            ow16(group*32+22, int(next()%257)-128);
            ow16(group*32+30, 256+int(next()%257)-128);
        }
        const unsigned mode = test % 8;
        unsigned dispcnt = mode | 0x1f00 | ((test & 1) ? 0x40 : 0) |
            ((test % 3) ? (next() & 0xe000) : 0) | ((test & 4) ? 0x10 : 0);
        if (test % 12 == 0) dispcnt |= 0x80;
        if (test >= 80) dispcnt &= (next() & 0x1f00) | 0xe0ff;
        auto ppu = std::make_unique<gba::GbaPpu>();
        std::unique_ptr<gba::GbaPpu> restored;
        std::unique_ptr<gba::GbaBus> restored_bus;
        std::vector<uint16_t> foreign(240*160), overlay_pixels(64*64);
        std::vector<uint8_t> overlay_alpha(64*64);
        for (auto& value : foreign) value = next();
        for (auto& value : overlay_pixels) value = next();
        for (auto& value : overlay_alpha) value = next()%17;
        GbaForeignScreenOverlay overlay{GBA_FOREIGN_SCREEN_OVERLAY_ABI_VERSION,
            overlay_pixels.data(),overlay_alpha.data(),-7,43,64,64,64,0,0};
        GbaForeignObjFocusTransform focus{};
        focus.abi_version = GBA_FOREIGN_OBJ_FOCUS_ABI_VERSION;
        focus.source_link_feet_x = 70; focus.source_link_feet_y = 80;
        focus.destination_link_feet_x = 120; focus.destination_link_feet_y = 95;
        focus.source_radius_x = 100; focus.source_radius_y = 80;
        focus.source_obj_scale_q8_8 = 192;
        auto publish = [&] {
            gba::foreign_presentation_internal::clear();
            if (test >= 96 && (test & 1)) gba::foreign_presentation_internal::set_background(foreign.data());
            if (test >= 96 && (test & 2)) gba::foreign_presentation_internal::set_screen_overlay(&overlay);
            if (test >= 96 && (test & 4)) gba::foreign_presentation_internal::set_obj_focus(&focus);
        };
        publish();
        for (unsigned y = 0; y < 160; ++y) {
            for (unsigned bg = 2; bg <= 3; ++bg) {
                const unsigned a = 0x20+(bg-2)*0x10;
                if (y == 0 || y % 7 == 0) {
                    w16(a,256+int(next()%1025)-512); w16(a+2,int(next()%513)-256);
                    w16(a+4,int(next()%513)-256); w16(a+6,256+int(next()%1025)-512);
                    w32(a+8,next() & 0xfffffff); w32(a+12,next() & 0xfffffff);
                    ppu->note_affine_reference_write(bg,false); ppu->note_affine_reference_write(bg,true);
                    if (restored) { restored->note_affine_reference_write(bg,false); restored->note_affine_reference_write(bg,true); }
                }
            }
            if (y % 3 == 0) {
                unsigned i = next()%pal.size(); pal[i] ^= 0x3f;
                if (restored_bus) restored_bus->pal_ptr()[i] = pal[i];
            }
            if (y % 11 == 0) {
                unsigned i = next()%vram.size(); vram[i] ^= 0xa5;
                if (restored_bus) restored_bus->vram_ptr()[i] = vram[i];
            }
            if (y % 13 == 0) {
                unsigned i = next()%oam.size(); oam[i] ^= 0x80;
                if (restored_bus) restored_bus->oam_ptr()[i] = oam[i];
            }
            if (y % 19 == 0) w16(0x40,next());
            ppu->render_scanline(y,dispcnt,io.data(),vram.data(),oam.data(),pal.data());
            if (restored) restored->render_scanline(y,dispcnt,io.data(),restored_bus->vram_ptr(),restored_bus->oam_ptr(),restored_bus->pal_ptr());
            if (y % 17 == 0 || y == 53) {
                const auto bytes = gba::save_device_state(*bus,*ppu);
                if (std::fwrite(bytes.data(),1,bytes.size(),stdout) != bytes.size()) return 2;
                emitted += bytes.size();
                if (restored) {
                    if (gba::save_device_state(*restored_bus,*restored) != bytes) { std::fprintf(stderr,"restore differs case=%u y=%u\n",test,y); return 1; }
                }
                if (y == 53) {
                    restored = std::make_unique<gba::GbaPpu>();
                    restored_bus = std::make_unique<gba::GbaBus>();
                    if (!gba::load_device_state(*restored_bus,*restored,bytes)) return 3;
                    publish(); // host-owned presentation is deliberately not saved
                }
            }
        }
        ppu->mark_framebuffer_latched(); restored->mark_framebuffer_latched();
        const auto bytes = gba::save_device_state(*bus,*ppu);
        if (bytes != gba::save_device_state(*restored_bus,*restored)) return 4;
        if (std::fwrite(bytes.data(),1,bytes.size(),stdout) != bytes.size()) return 2;
        emitted += bytes.size();
        gba::foreign_presentation_internal::clear();
    }
    std::fprintf(stderr,"104 cases,16640 rows,all modes,7 foreign fallback combinations,partial/current-build restore; %llu exact-oracle output bytes\n",emitted);
#if defined(GBARECOMP_ORDERED_SCANLINE_EXPERIMENT) && defined(GBARECOMP_ORDERED_DRAW_TEST_COVERAGE)
    const auto active = gba_ordered_draw_test_rows();
    std::fprintf(stderr,"ordered compositor rows=%llu\n",static_cast<unsigned long long>(active));
    if (active != 97u*(160u+106u)) return 5;
#endif
}
