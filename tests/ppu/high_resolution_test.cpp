#include "gba_ppu.h"
#include "gba_bus.h"
#include "gba_color.h"
#include "gba_simulation_state.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
namespace {
bool authorize(int bg, int) { return bg == 2; }
void opaque_and_forced_alpha() {
    gba::GbaHighResolutionFrame hd(4);
    hd.configure(240,160);
    std::array<uint8_t,0x60> io{};
    std::array<uint8_t,98304> vram{};
    std::array<uint8_t,1024> pal{};
    io[0x0c]=4; io[0x21]=1; io[0x27]=1;
    // Mode-zero BLDCNT still blends a semi-transparent OBJ against BG2.
    io[0x51]=4; io[0x52]=io[0x53]=8;
    pal[2]=0; pal[3]=0x7c;
    for(unsigned i=0;i<64;++i) vram[0x4000+i]=1;
    for(unsigned y=0;y<160;++y) {
        CHECK(hd.begin_line(y,0,0,0x0401,io.data(),vram.data(),pal.data(),authorize,false));
        for(unsigned x=0;x<240;++x) {
            hd.window(x,0x3f);
            hd.submit(x,31,0,4,(x&1)!=0,false);
        }
        hd.resolve();
    }
    hd.latch(); CHECK(hd.pixels());
    uint8_t opaque[3],alpha[3];
    gba::color::to_rgb888(31,opaque);
    gba::color::to_rgb888(gba::color::blend_alpha_gba555(31,0x7c00,8,8),alpha);
    for(unsigned sx=0;sx<4;++sx) {
        CHECK(std::memcmp(hd.pixels()+sx*3,opaque,3)==0);
        CHECK(std::memcmp(hd.pixels()+(4+sx)*3,alpha,3)==0);
    }
}
void filter_precision() {
    gba::GbaHighResolutionFrame hd(2);
    hd.configure(240,160);
    std::array<uint8_t,0x60> io{};
    std::array<uint8_t,98304> vram{};
    std::array<uint8_t,1024> pal{};
    io[0x0c] = 4; // character base 1, map base 0
    io[0x28] = io[0x2c] = 128; // exact half-texel in both axes
    vram[0x4000] = 1; vram[0x4001] = 2;
    vram[0x4008] = 3; vram[0x4009] = 4;
    for (unsigned y = 0; y < 160; ++y) {
        // White saturates all lanes; four primary/white corners average to
        // 15.5 per channel, which must round up independently. Bit 15 is not
        // a fourth interpolated channel. Also refresh palette without tiles.
        const uint16_t colors[] = {0,0x801f,0x83e0,0xfc00,0xffff};
        for (unsigned i = 1; i <= 4; ++i) {
            const uint16_t c = y ? colors[i] : 0xffff;
            pal[i*2] = uint8_t(c); pal[i*2+1] = uint8_t(c >> 8);
        }
        CHECK(hd.begin_line(y,0,0,0x0401,io.data(),vram.data(),pal.data(),authorize,true));
        hd.resolve();
    }
    hd.latch(); CHECK(hd.pixels());
    for (unsigned channel = 0; channel < 3; ++channel) {
        CHECK(hd.pixels()[channel] == 255);
        CHECK(hd.pixels()[size_t(2)*hd.width()*3+channel] == 132);
    }
}
void run(unsigned scale, bool wide, bool effects) {
    auto bus = std::make_unique<gba::GbaBus>();
    auto native = std::make_unique<gba::GbaPpu>();
    auto enhanced = std::make_unique<gba::GbaPpu>();
    bus->rtc().set_emulated_clock(0);
    enhanced->set_internal_resolution(scale, authorize);
    if (wide) for (auto* p : {native.get(), enhanced.get()}) p->set_view_margins(42, 43, 0, 0);
    std::array<uint8_t, 0x400> io{};
    auto w16 = [&](unsigned a, unsigned n) { io[a] = uint8_t(n); io[a+1] = uint8_t(n >> 8); };
    auto w32 = [&](unsigned a, unsigned n) { w16(a,n); w16(a+2,n >> 16); };
    auto pal = [&](unsigned i) { return uint16_t((i & 31) | ((i*3 & 31) << 5) | ((i*7 & 31) << 10)); };
    for (unsigned i = 0; i < 256; ++i) {
        bus->pal_ptr()[i*2] = uint8_t(pal(i)); bus->pal_ptr()[i*2+1] = uint8_t(pal(i) >> 8);
    }
    // BG2's wrapped tilemap points to tile zero at character base one.
    for (unsigned i = 0; i < 64; ++i) bus->vram_ptr()[0x4000+i] = uint8_t(i+1);
    for (unsigned i = 0; i < 64; ++i) bus->vram_ptr()[0x4040+i] = uint8_t(64-i);
    w16(0x0c, 0x2004); w16(0x20, 0x200);
    // A foreground text HUD tile; no affine interpolation may overwrite it.
    w16(0x08, 0x0408); bus->vram_ptr()[0x2000] = 1;
    std::memset(bus->vram_ptr()+0x8020, 0x11, 32);
    if (effects) { w16(0x50, 0x2044); w16(0x52, 0x0808); }
    auto frame = [&] {
        for (unsigned y = 0; y < 160; ++y) {
            // Palette edits, texel edits and transparency changes at HBlank
            // must be observed even when the same tile stays selected.
            bus->vram_ptr()[0x403f] = y & 1 ? 0 : 63;
            bus->pal_ptr()[126] = uint8_t(y); bus->pal_ptr()[127] = uint8_t(y >> 1);
            // Change the map with unchanged character data, then change the
            // source layout. This exercises persistent surface invalidation,
            // including the largest map at the highest legal screen base.
            bus->vram_ptr()[17] = uint8_t(y & 1);
            w16(0x0c, y < 120 ? 0x2004 : 0xff0c);
            if (y >= 120) {
                bus->vram_ptr()[0xf800 + (y / 8) * 128 + 1] = uint8_t(y & 1);
                bus->vram_ptr()[0xc000 + (y & 7) * 8 + 1] = uint8_t(y);
            }
            w32(0x28,0); w32(0x2c,y*0x200);
            for (auto* p : {native.get(),enhanced.get()}) {
                p->note_affine_reference_write(2,false); p->note_affine_reference_write(2,true);
                p->render_scanline(y, 0x0501, io.data(), bus->vram_ptr(), bus->oam_ptr(), bus->pal_ptr());
            }
        }
        native->mark_framebuffer_latched(); enhanced->mark_framebuffer_latched();
    };
    frame();
    CHECK(std::memcmp(native->latched_framebuffer(),enhanced->latched_framebuffer(),native->render_bytes())==0);
    // Whole-session snapshots intentionally reject a widened canonical PPU.
    // Netplay exercises expansion through its separate native-state mirror.
    if (!wide) CHECK(gba::save_device_state(*bus,*native)==gba::save_device_state(*bus,*enhanced));
    const auto* hd = enhanced->high_resolution_framebuffer();
    CHECK(hd && enhanced->high_resolution()->samples() > 0);
    const unsigned width = native->render_width(), left = native->view_extra_left();
    CHECK(enhanced->high_resolution()->width()==width*scale);
    // Every original sample still reproduces the native compositor exactly,
    // including HUD occlusion, palette precision, alpha and negative margins.
    const auto* canonical = native->latched_framebuffer();
    for (unsigned y = 0; y < 160; ++y) for (unsigned x = 0; x < width; ++x)
        CHECK(std::memcmp(hd + (size_t(y*scale)*width*scale+x*scale)*3,
                          canonical+(size_t(y)*width+x)*3,3)==0);
    uint8_t expected[3];
    gba::color::to_rgb888(pal(1),expected);
    CHECK(std::memcmp(hd+(size_t(5*scale)*width*scale+(left+5)*scale+1)*3,expected,3)==0);
    if (!effects && scale % 2 == 0) {
        // PA and adjacent row references move by two source texels. The odd
        // output samples reveal texels absent from a nearest-scaled native frame.
        gba::color::to_rgb888(pal(28),expected);
        const auto* subpixel = hd+(size_t(13*scale+scale/2)*width*scale+(left+13)*scale+scale/2)*3;
        CHECK(std::memcmp(subpixel,expected,3)==0);
        CHECK(std::memcmp(subpixel,canonical+(size_t(13)*width+left+13)*3,3)!=0);
    }
    enhanced->reset(); CHECK(!enhanced->high_resolution_framebuffer());
    enhanced->render_scanline(40,0x0501,io.data(),bus->vram_ptr(),bus->oam_ptr(),bus->pal_ptr());
    enhanced->mark_framebuffer_latched(); CHECK(!enhanced->high_resolution_framebuffer());
    frame(); CHECK(enhanced->high_resolution_framebuffer());
    enhanced->set_view_margins(5,6,0,0); CHECK(!enhanced->high_resolution_framebuffer());
    enhanced->set_internal_resolution(1); CHECK(!enhanced->high_resolution_framebuffer());
}
}
int main() {
    // The expanded logical view must also fit the optional internal surface.
    gba::GbaHighResolutionFrame capacity(2);
    capacity.configure(gba::GbaPpu::kMaxRenderWidth, 208);
    CHECK(capacity.width() == gba::GbaPpu::kMaxRenderWidth * 2 && capacity.height() == 416);
    opaque_and_forced_alpha();
    filter_precision();
    for (unsigned scale : {2u,3u,4u,8u,16u}) { run(scale,false,false); run(scale,true,true); }
    std::puts("high resolution: real fractional samples, HUD/blend parity, native state isolation, reset/partial-raster/resize passed");
}
