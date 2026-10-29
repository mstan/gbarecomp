#include "gba_ppu.h"
#include "gba_native_draw_frame.h"
#include "hd_gpu_renderer.h"
#include "color_lut.h"
#include "foreign_presentation_internal.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>
#include <SDL.h>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s (%s)\n",__LINE__,#x,SDL_GetError());std::exit(1); } } while (0)
namespace {
void half(uint8_t* p,unsigned v){p[0]=uint8_t(v);p[1]=uint8_t(v>>8);}
unsigned half(const uint8_t* p){return unsigned(p[0])|(unsigned(p[1])<<8);}
int signed_half(const uint8_t* p){return int16_t(half(p));}
void reference(uint8_t* p,int value){unsigned v=unsigned(value)&0x0fffffffu;half(p,v);half(p+2,v>>16);}
int reference(const uint8_t* p){unsigned v=half(p)|(half(p+2)<<16);return int32_t(v<<4)>>4;}
void retain(const std::string& path,const std::vector<uint8_t>& bytes){
    auto* file=std::fopen(path.c_str(),"wb");CHECK(file);
    CHECK(std::fwrite(bytes.data(),1,bytes.size(),file)==bytes.size());CHECK(!std::fclose(file));
}
void run(gbarecomp::HdGpuRenderer& gpu,unsigned scenario){
    std::mt19937 random(0x4e415449+scenario);
    std::vector<uint8_t> vram(gba::GbaDrawMemory::kVramBytes);
    std::array<uint8_t,0x60> io{};
    std::array<uint8_t,1024> oam{},palette{};
    for(auto& byte:vram)byte=uint8_t(random());
    for(auto& byte:oam)byte=uint8_t(random());
    for(auto& byte:palette)byte=uint8_t(random());
    for(unsigned object=0;object<4;++object){
        half(oam.data()+object*8,(object*19+20)|((object==1)?0x300:0)|((object==2)?0x800:0x400));
        half(oam.data()+object*8+2,(object*29+31)|(3u<<14));
        half(oam.data()+object*8+4,512+object*16);
    }
    for(unsigned group=0;group<32;++group){
        half(oam.data()+group*32+6,267);half(oam.data()+group*32+14,unsigned(-31));
        half(oam.data()+group*32+22,19);half(oam.data()+group*32+30,243);
    }
    const unsigned mode=scenario%8;
    auto ppu=std::make_unique<gba::GbaPpu>();
    auto memory=std::make_unique<gba::GbaDrawMemory>();
    gba::GbaNativeDrawBuilder builder;
    for(unsigned y=0;y<160;++y){
        unsigned dispcnt=mode|0x1f00|((scenario&1)?64:0);
        if((y/11)%2)dispcnt|=0xe000;
        if(y%19==0)dispcnt|=128;
        if(scenario>=8)dispcnt&=~(0x100u<<((scenario-8)%4));
        for(unsigned layer=0;layer<4;++layer){
            unsigned cnt=(layer&3)|((layer&3)<<2)|(((layer*3+8)%32)<<8)|(((y/17+layer)&3)<<14);
            if((scenario+y+layer)&1)cnt|=128;
            if((y/13)&1)cnt|=8192;
            half(io.data()+8+layer*2,cnt);
            half(io.data()+16+layer*4,y*3+layer*67);half(io.data()+18+layer*4,y*5+layer*11);
        }
        for(unsigned base:{32u,48u}){
            half(io.data()+base,239+y%23);half(io.data()+base+2,unsigned(-17));
            half(io.data()+base+4,unsigned(-11));half(io.data()+base+6,251);
            reference(io.data()+base+8,int(y)*41-1500);reference(io.data()+base+12,int(y)*219-3000);
        }
        half(io.data()+64,(y%31)<<8|(240-y%23));half(io.data()+66,0xf010);
        half(io.data()+68,0x059b);half(io.data()+70,0x9b05);
        half(io.data()+72,(y&1)?0x2d3b:0x3f1f);half(io.data()+74,(y&2)?0x3f20:0x1b3f);
        half(io.data()+80,63u|(((y/7)&3)<<6)|(63u<<8));
        half(io.data()+82,(y%32)|((31-y%32)<<8));half(io.data()+84,y%32);
        // Preserve the original12 per-row-write oracles. Additional cases
        // vary every row's registers/OAM/palette across unchanged VRAM,
        // including writes at irregular boundaries between long spans.
        const bool write_vram=scenario<12 ||
            (scenario%3==1 && (y==53 || y==119)) ||
            (scenario%3==2 && y%7==0);
        if(write_vram) {
            const auto offset=(y*619u)%(vram.size()-9);
            for(unsigned j=0;j<9;++j)vram[offset+j]=uint8_t(random());
            CHECK(memory->mark(offset,9));
        }
        const unsigned palette_offset=(y*23u)%1022;
        half(palette.data()+palette_offset,unsigned(random()));
        half(oam.data()+8*127+2,y%200);
        for(unsigned bg:{2u,3u})for(bool axis:{false,true})ppu->note_affine_reference_write(bg,axis);
        ppu->render_scanline(y,uint16_t(dispcnt),io.data(),vram.data(),oam.data(),palette.data());
        auto effective=io;
        for(unsigned base:{32u,48u}){
            reference(effective.data()+base+8,reference(io.data()+base+8)-int(y)*signed_half(io.data()+base+2));
            reference(effective.data()+base+12,reference(io.data()+base+12)-int(y)*signed_half(io.data()+base+6));
        }
        CHECK(builder.capture(y,uint16_t(dispcnt),effective,oam,palette,vram,*memory));
    }
    ppu->mark_framebuffer_latched();
    std::vector<uint8_t> expected(ppu->latched_framebuffer(),ppu->latched_framebuffer()+gba::GbaPpu::kFramebufferBytes);
    auto frame=builder.finish();CHECK(frame);
    // The GPU cannot consult any live device or provider memory after capture.
    std::fill(vram.begin(),vram.end(),0xee);io.fill(0xee);oam.fill(0xee);palette.fill(0xee);
    std::array<uint16_t,240*160> later_foreign{};later_foreign.fill(0x7fff);
    gba::foreign_presentation_internal::set_background(later_foreign.data());
    const auto portable=gba::GbaPpu::render_native_draw_frame(*frame);
    CHECK(gba::foreign_presentation_internal::background()==later_foreign.data());
    gba::foreign_presentation_internal::clear();
    CHECK(portable==expected);
    CHECK(gpu.render_native(*frame,nullptr));
    std::vector<uint8_t> actual;unsigned width=0,height=0;
    CHECK(gpu.readback(actual,width,height));CHECK(width==240 && height==160);
    const auto prefix="native-"+std::to_string(scenario);
    retain(prefix+".reference.rgb",expected);retain(prefix+".gpu.rgb",actual);
    if(actual!=expected){
        const auto at=std::mismatch(actual.begin(),actual.end(),expected.begin()).first-actual.begin();
        std::fprintf(stderr,"native GPU mismatch case=%u byte=%zu xy=%zu,%zu channel=%zu expected=%u actual=%u\n",scenario,size_t(at),size_t(at)/3%240,size_t(at)/720,size_t(at)%3,expected[at],actual[at]);
        std::exit(1);
    }
    gbarecomp::runtime::ColorSettings settings;
    settings.screen=gbarecomp::runtime::ScreenKind::Classic;
    gbarecomp::runtime::ColorLut lut(settings);
    std::vector<uint8_t> expected_graded(expected.size());
    lut.map_rgb888(expected.data(),expected_graded.data(),240,160);
    CHECK(gpu.render_native(*frame,&lut));CHECK(gpu.readback(actual,width,height));
    retain(prefix+".graded-reference.rgb",expected_graded);retain(prefix+".graded-gpu.rgb",actual);
    CHECK(actual==expected_graded);
    CHECK(gpu.readback_native(actual));CHECK(actual==expected);
    CHECK(gpu.native_display(nullptr));CHECK(gpu.readback(actual,width,height));CHECK(actual==expected);
    CHECK(gpu.native_display(&lut));CHECK(gpu.readback(actual,width,height));CHECK(actual==expected_graded);
    if(!scenario){
        // Exercise the existing HD consumer and return to native on one SDL
        // context. Input texture formats and target dimensions must rebind.
        gba::GbaHighResolutionFrame::DrawFrame hd;
        hd.width=240;hd.height=160;hd.scale=2;hd.lines.resize(160);
        for(unsigned y=0;y<160;++y){auto& line=hd.lines[y];
            line.kind=gba::GbaHighResolutionFrame::LineJob::UpscaleLine;
            line.native_rgb.assign(expected.begin()+y*720,expected.begin()+(y+1)*720);
        }
        CHECK(gpu.render(hd,nullptr));CHECK(gpu.readback(actual,width,height));CHECK(width==480 && height==320);
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x)for(unsigned channel=0;channel<3;++channel)
            CHECK(actual[(y*width+x)*3+channel]==expected[((y/2)*240+x/2)*3+channel]);
        CHECK(gpu.render_native(*frame,nullptr));CHECK(gpu.readback(actual,width,height));CHECK(actual==expected);
    }
    std::printf("native GPU case=%u mode=%u exact=%zu owned_payload=%zu\n",scenario,mode,actual.size(),frame->payload_bytes());std::fflush(stdout);
}
}
int main(){
    CHECK(SDL_Init(SDL_INIT_VIDEO)==0);
    SDL_SetHint(SDL_HINT_RENDER_DRIVER,"opengl");
    auto* window=SDL_CreateWindow("Native raster qualification",0,0,240,160,SDL_WINDOW_HIDDEN|SDL_WINDOW_OPENGL);CHECK(window);
    auto* renderer=SDL_CreateRenderer(window,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_TARGETTEXTURE);CHECK(renderer);
    auto* texture=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,240,160);CHECK(texture);
    {
        gbarecomp::HdGpuRenderer gpu;CHECK(gpu.initialize(renderer,texture));
        for(unsigned scenario=0;scenario<24;++scenario)run(gpu,scenario);
    }
    SDL_DestroyTexture(texture);SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_Quit();
    std::puts("Native GPU:24 complete owned frames match CPU oracle across modes0..7,OBJ/windows/effects, historical writes and unchanged-memory row spans; no game-speed claim");
}
