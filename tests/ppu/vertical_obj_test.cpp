#include "gba_ppu.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::exit(1); } } while(0)
namespace {
int object_y = 176;
int authorize(int i, std::uint16_t, std::uint16_t, std::uint16_t, int* y) {
    if (i != 0) return 0;
    *y = object_y; return 1;
}
int tiles(int,int,int,std::uint16_t*) { return 0; }
}
int main() {
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
