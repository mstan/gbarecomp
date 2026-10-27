// Explicit before/after renderer comparison. The owner may link this fixture
// against a frozen reference PPU; identical line hashes cover live byte edits,
// text map sizes, flips, scroll seams, window/effect routing and providers.
#include "gba_ppu.h"
#include "sha1.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "text_span_reference.h"
#include <vector>

namespace {
void put16(uint8_t* p, uint16_t v) { p[0]=uint8_t(v); p[1]=uint8_t(v>>8); }
int tiles(int bg,int x,int y,uint16_t* out) {
    if ((x & 15) == 0) return 0;
    *out=uint16_t(((x>>3)+(y>>3)*17+bg*9)&0xffff); return 1;
}
int sample_x(int,int x,int,int* out) { *out=(x*3)&255; return (x%13) ? 1 : -1; }
int sample_xy(int,int x,int y,int* sx,int* sy) { *sx=(x+29)%240; *sy=y+7; return (x%11) ? 1 : -1; }
}
int main() {
    std::array<uint8_t,0x60> io{};
    std::array<uint8_t,98304> vram{};
    std::array<uint8_t,1024> oam{},pal{};
    unsigned case_index=0;
    uint32_t seed=733;
    for(auto* a : {&oam,&pal}) for(auto& b:*a) { seed=seed*1664525u+1013904223u; b=uint8_t(seed>>24); }
    for(auto& b:vram) { seed=seed*1664525u+1013904223u; b=uint8_t(seed>>24); }
    // Hide OBJ: this fixture isolates text row preparation from sprite rules.
    for(unsigned n=0;n<128;++n) put16(oam.data()+n*8,0x200);
    for(unsigned width : {240u,288u,576u,896u}) {
        gba::GbaPpu ppu;
        ppu.set_view_margins((width-240)/2,(width-240)/2,0,0);
        std::vector<uint8_t> image(width*160*3);
        for(unsigned providers=0;providers<4;++providers) {
            gba::g_ws_tilemap_provider=providers ? tiles : nullptr;
            gba::g_ws_bg_x_provider=providers==2 ? sample_x : nullptr;
            gba::g_ws_bg_x_provider_layers=2;
            gba::g_ws_bg_xy_provider=providers==3 ? sample_xy : nullptr;
            gba::g_ws_bg_xy_provider_layers=4;
            for(unsigned mode=0;mode<4;++mode) {
                for(unsigned layer=0;layer<4;++layer) {
                    put16(io.data()+8+layer*2,uint16_t((mode<<14)|(layer<<2)|((28+layer)<<8)|((layer&1)<<7)|layer));
                    put16(io.data()+0x10+layer*4,uint16_t(249+layer*7));
                    put16(io.data()+0x12+layer*4,uint16_t(253+mode*3));
                }
                put16(io.data()+0x40,0x179b); put16(io.data()+0x42,0xc91d);
                put16(io.data()+0x44,0x1070); put16(io.data()+0x46,0x30a0);
                put16(io.data()+0x48,0x2f37); put16(io.data()+0x4a,0x3b1f);
                put16(io.data()+0x50,uint16_t(0x3f3f|(mode<<6)));
                put16(io.data()+0x52,0x0808);put16(io.data()+0x54,7);
                // Change raw pointers between frames, without a bus write hook.
                vram[(width+providers*16+mode)*7%vram.size()]^=0x8f;
                ppu.render(image.data(),0x6f00,io.data(),vram.data(),oam.data(),pal.data());
                const auto actual=gba::sha1(image.data(),image.size()).hex();
                if (actual != kTextSpanReference[case_index++]) {
                    std::fprintf(stderr,"text row differs: width=%u providers=%u mode=%u hash=%s\n",
                                 width,providers,mode,actual.c_str());
                    return 1;
                }
            }
        }
    }
    gba::g_ws_tilemap_provider=nullptr; gba::g_ws_bg_x_provider=nullptr; gba::g_ws_bg_xy_provider=nullptr;
    std::puts("text span contract: 64 frozen-reference images match");
    return 0;
}
