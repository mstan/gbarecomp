#include "gba_high_resolution.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::exit(1); } } while (0)
namespace {
bool authorize(int, int) { return true; }
void run(unsigned scale, bool filter, bool effects) {
    using Hd = gba::GbaHighResolutionFrame;
    Hd immediate(scale), deferred(scale);
    deferred.set_deferred_rendering(true);
    constexpr unsigned width = 257, height = 160;
    immediate.configure(width,height); deferred.configure(width,height);
    std::array<uint8_t,0x60> io{};
    std::array<uint8_t,98304> vram{};
    std::array<uint8_t,1024> palette{};
    std::array<uint8_t,width*3> native{};
    auto w16 = [&](unsigned offset, unsigned value) {
        io[offset]=uint8_t(value); io[offset+1]=uint8_t(value>>8);
    };
    auto w32 = [&](unsigned offset, int value) { w16(offset,unsigned(value)); w16(offset+2,unsigned(value)>>16); };
    w16(0x0c,0x2004); w16(0x0e,0x2109);
    if(effects) { w16(0x50,0x3f7f); w16(0x52,0x090b); w16(0x54,7); }
    for(unsigned i=0;i<16384;++i) {
        vram[0x4000+i]=uint8_t(i%13+1); vram[0x8000+i]=uint8_t(i%17+1);
    }
    for(unsigned y=0;y<height;++y) {
        for(unsigned i=0;i<256;++i) {
            const unsigned c=(i*137+y*193) & 0xffff;
            palette[2*i]=uint8_t(c);palette[2*i+1]=uint8_t(c>>8);
        }
        if(y%7==0) { vram[0x4000+y]=uint8_t(y);vram[0x8000+y]=uint8_t(159-y); }
        if(y%13==0) vram[(y*19)%2048]=uint8_t(y);
        w16(0x20,257-y);w16(0x24,13-int(y));w16(0x22,-3);w16(0x26,263);
        w16(0x30,-181);w16(0x34,23);w16(0x32,7);w16(0x36,190);
        w32(0x28,int(y)*-31-4096);w32(0x2c,int(y)*263);
        w32(0x38,int(y)*17);w32(0x3c,int(y)*190-1024);
        for(auto* hd:{&immediate,&deferred}) {
            if(y%11==0) {
                for(unsigned i=0;i<native.size();++i)native[i]=uint8_t(i+y);
                hd->upscale_line(y,native.data());
                continue;
            }
            CHECK(hd->begin_line(int(y),8,0,0x0c02,io.data(),vram.data(),palette.data(),authorize,filter));
            for(unsigned x=0;x<width;++x) {
                hd->window(x,x%19?0x3f:0x1f);
                if(x%23==0)hd->black_column(x);
                if(x%7==0)hd->submit(x,uint16_t(x*31+y),0,4,effects && x%3==0,true);
            }
            hd->resolve();
        }
    }
    immediate.latch();deferred.latch();
    CHECK(immediate.pixels());CHECK(deferred.samples()==0);
    auto frame=deferred.draw_frame();CHECK(frame);
    CHECK(frame->lines[1].surfaces[0]==frame->lines[2].surfaces[0]);
    CHECK(frame->lines[6].surfaces[0]!=frame->lines[7].surfaces[0]);
    CHECK(frame->lines[1].palette!=frame->lines[2].palette);
    const size_t bytes=size_t(width)*height*scale*scale*3;
    std::vector<uint8_t> expected(immediate.pixels(),immediate.pixels()+bytes);
    // Destroy all borrowed source data before asking for CPU fallback pixels.
    io.fill(0);vram.fill(0);palette.fill(0);native.fill(0);
    auto rendered=Hd::render_jobs(*frame);
    CHECK(rendered==expected);
    CHECK(std::memcmp(deferred.pixels(),expected.data(),bytes)==0);
    deferred.begin_frame();deferred.upscale_line(0,native.data());deferred.latch();
    CHECK(!deferred.draw_frame());CHECK(!deferred.pixels());
    deferred.invalidate();deferred.configure(260,160);
    CHECK(Hd::render_jobs(*frame)==expected); // held old frame remains immutable
    auto invalid=*frame;invalid.lines[0].kind=Hd::LineJob::Missing;
    bool rejected=false;
    try { (void)Hd::render_jobs(invalid); } catch(const std::invalid_argument&) { rejected=true; }
    CHECK(rejected);
    invalid=*frame;invalid.lines[1].affine[0].cache=2;
    rejected=false;
    try { (void)Hd::render_jobs(invalid); } catch(const std::invalid_argument&) { rejected=true; }
    CHECK(rejected);
    deferred.set_deferred_rendering(false);CHECK(!deferred.pixels());
}
}
int main() {
    for(unsigned scale:{2u,3u,8u,16u})for(bool filter:{false,true})run(scale,filter,filter);
    std::puts("HD immutable jobs: exact full pixels, two layers, signed transforms, filtering/blend/window, HBlank edits, version sharing, destroyed sources, held frames and partial/invalid input passed");
}
