#include "gba_ppu.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::exit(1); } } while(0)
namespace {
int object_y = 176;
int authorize(int i, std::uint16_t, std::uint16_t, std::uint16_t, int* y) {
    if (i != 0) return 0;
    *y = object_y; return 1;
}
int tiles(int,int,int,std::uint16_t*) { return 0; }
int repeat_world(int i, std::uint16_t, std::uint16_t, std::uint16_t) {
    return i < 4 ? 240 : 0;
}
int cyclic_window(int x, int, int* out) {
    *out = (x % 240 + 240) % 240; return 1;
}
void periodic_objects() {
    std::array<std::uint8_t,0x400> io{}, oam{}, pal{};
    std::array<std::uint8_t,0x18000> vram{};
    auto put=[](auto& b,unsigned o,unsigned v) {b[o]=v;b[o+1]=v>>8;};
    auto object=[&](unsigned i,unsigned a0,unsigned a1,unsigned a2) {
        put(oam,i*8,a0);put(oam,i*8+2,a1);put(oam,i*8+4,a2);
    };
    for(unsigned i=0;i<128;++i) object(i,0x0200,0,0);
    put(pal,2,0x03e0);put(pal,0x202,0x001f);put(pal,0x204,0x7c00);
    for(unsigned i=0;i<32;++i) {vram[i]=0x11;vram[0x10000+i]=0x11;vram[0x10020+i]=0x22;}
    put(io,8,0x1f01); // uniform green BG0, behind priority-zero OBJ
    put(io,0x40,0x2878);put(io,0x44,0x00a0);
    put(io,0x48,0x003f);put(io,0x4a,0x003f);
    put(io,0x50,0x0140);put(io,0x52,0x0808);
    auto reference=std::make_unique<gba::GbaPpu>();
    std::vector<std::uint8_t> native(240*160*3);
    for(unsigned scene=0;scene<5;++scene) {
        // The game already publishes the two pieces at the native seam.
        object(0,40 | (scene==1 ? 0x0400 : 0),238,0);
        object(1,40 | (scene==1 ? 0x0400 : 0),510,0);
        object(2,60 | (scene==2 ? 0x0100 : 0),64,1 | (scene==3 ? 0x0800 : 0));
        object(3,60 | 0x0800,66,0); // native OBJ-window stencil
        put(oam,6,256);put(oam,14,0);put(oam,22,0);put(oam,30,256);
        const unsigned display=0x1140 | (scene==3 ? 0x2000 : 0) | (scene==4 ? 0x8000 : 0);
        if(scene==3) put(io,0x4a,0x0021); // OBJ hidden outside WIN0
        else if(scene==4) put(io,0x4a,0x3f21); // OBJ visible only in OBJ-window
        else put(io,0x4a,0x003f);
        gba::g_ws_window_x_provider=cyclic_window;
        gba::g_ws_obj_window_x_provider=cyclic_window;
        gba::g_ws_obj_x_period_provider=repeat_world;
        reference->render(native.data(),display,io.data(),vram.data(),oam.data(),pal.data());
        for(unsigned width : {288u,480u}) {
            auto wide=std::make_unique<gba::GbaPpu>();
            const unsigned left=(width-240)/2;
            wide->set_view_margins(left,left,0,0);
            for(unsigned y=0;y<160;++y)
                wide->render_scanline(y,display,io.data(),vram.data(),oam.data(),pal.data());
            wide->mark_framebuffer_latched();
            const auto* rgb=wide->latched_framebuffer();
            for(unsigned y=0;y<160;++y)for(unsigned x=0;x<width;++x)for(unsigned c=0;c<3;++c) {
                const int hx=(int(x)-int(left)+480)%240;
                CHECK(rgb[(y*width+x)*3+c]==native[(y*240+hx)*3+c]);
            }
            if(scene==0 && width==480) {
                gba::g_ws_obj_x_period_provider=nullptr;
                wide->render_scanline(60,display,io.data(),vram.data(),oam.data(),pal.data());
                wide->mark_framebuffer_latched();
                CHECK(wide->latched_framebuffer()[(60*width+424)*3+2]==0);
                gba::g_ws_obj_x_period_provider=repeat_world;
            }
        }
    }
    // HUD objects keep ordinary placement even while the world repeats.
    object(2,0x0200,0,0);object(3,0x0200,0,0);object(4,24,70,0);
    put(io,0x4a,0x003f);
    auto wide=std::make_unique<gba::GbaPpu>();wide->set_view_margins(120,120,0,0);
    for(unsigned y=0;y<160;++y)wide->render_scanline(y,0x1140,io.data(),vram.data(),oam.data(),pal.data());
    wide->mark_framebuffer_latched();const auto* rgb=wide->latched_framebuffer();
    CHECK(rgb[(24*480+190)*3]>200 && rgb[(24*480+430)*3]==0);
    gba::g_ws_obj_x_period_provider=nullptr;gba::g_ws_window_x_provider=nullptr;
    gba::g_ws_obj_window_x_provider=nullptr;
    std::puts("periodic OBJ native-center/seam/alpha/affine/priority/windows and HUD exclusion passed");
}
}
int main() {
    periodic_objects();
    auto ppu=std::make_unique<gba::GbaPpu>();
    ppu->set_view_margins(65,65,12,36);
    std::array<std::uint8_t,0x400> io{},oam{},pal{};
    std::array<std::uint8_t,0x18000> vram{};
    for(unsigned i=0;i<128;++i) oam[i*8+1]=2;
    oam[0]=176; oam[1]=0; oam[2]=50;
    pal[0x202]=31;
    for(unsigned i=0;i<32;++i)vram[0x10000+i]=0x11;
    gba::g_ws_tilemap_provider=tiles;
    gba::g_ws_authored_margin_layers=1;
    auto render=[&] {
        for(unsigned y=0;y<160;++y)ppu->render_scanline(y,0x1040,io.data(),vram.data(),oam.data(),pal.data());
        ppu->mark_framebuffer_latched();
    };
    auto red=[&](unsigned x,unsigned y) { const auto* p=ppu->latched_framebuffer()+(y*ppu->render_width()+x)*3;return p[0]>200&&p[1]==0&&p[2]==0; };
    render(); CHECK(!red(115,188));
    gba::g_ws_obj_attr_y_provider=authorize;
    render(); CHECK(red(115,188));
    // Remapping a known HUD sprite to the top edge also works in extra rows.
    oam[0]=0; object_y=-12; render(); CHECK(red(115,0));
    // A provider must authenticate each sprite; raw OAM cannot draw extra rows.
    gba::g_ws_obj_attr_y_provider=nullptr;
    render(); CHECK(!red(115,0));
    gba::g_ws_tilemap_provider=nullptr;gba::g_ws_authored_margin_layers=0;
    std::puts("vertical OBJ attribution, raw OAM exclusion and top-edge HUD passed");
}
