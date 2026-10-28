#include "gba_high_resolution.h"
#include "hd_gpu_renderer.h"
#include "color_lut.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(GBARECOMP_HAVE_SDL2)
#include <SDL.h>
#include <SDL_opengl.h>
#define CHECK(x) do { if(!(x)){std::fprintf(stderr,"line %d: %s (%s)\n",__LINE__,#x,SDL_GetError());std::exit(1);} }while(0)
namespace {
using Hd=gba::GbaHighResolutionFrame;
std::shared_ptr<const Hd::IndexedSurface> surface(unsigned size,unsigned pattern) {
    auto result=std::make_shared<Hd::IndexedSurface>();result->size=size;result->texels.resize(size_t(size)*size);
    for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x)
        result->texels[size_t(y)*size+x]=(x+y+pattern)%11 ? uint8_t((x*7+y*13+pattern)%255+1):0;
    return result;
}
Hd::DrawFrame fixture(unsigned scale,unsigned mode) {
    Hd::DrawFrame frame;frame.width=257;frame.height=160;frame.scale=scale;frame.lines.resize(frame.height);
    auto a=surface(128,mode+1),b=surface(256,mode+7),changed=surface(128,mode+31);
    for(unsigned y=0;y<frame.height;++y) {
        auto& row=frame.lines[y];row.ox=8;
        if(y%13==0) {
            row.kind=Hd::LineJob::UpscaleLine;row.native_rgb.resize(frame.width*3);
            for(unsigned x=0;x<row.native_rgb.size();++x)row.native_rgb[x]=uint8_t(x*73+y*19);
            continue;
        }
        row.kind=Hd::LineJob::AffineLine;row.filter=mode&1;
        row.bldcnt=uint16_t(0x3f3f|((mode%4)<<6));row.alpha=0x1711;row.brightness=std::min(16u,mode*7);
        for(unsigned i=0;i<256;++i)row.palette[i]=uint16_t(i*137+y*193+mode*977);
        row.top.resize(frame.width);row.second.resize(frame.width);row.windows.resize(frame.width);row.black.resize(frame.width);
        for(unsigned x=0;x<frame.width;++x) {
            const unsigned layer=x%7?0:4;
            row.top[x]={uint16_t(x*51+y*127),x%17?450:-1,uint8_t(layer),bool(x&1),true,true};
            row.second[x]={uint16_t(x*177+y*23),800,1,true,bool(x%5),true};
            row.windows[x]=x%19?0x3f:0x1b;row.black[x]=x%23==0;
        }
        row.surfaces[0]=y<80?a:changed;row.surfaces[1]=b;
        for(unsigned i=0;i<2;++i) {
            auto& affine=row.affine[i];affine.active=!(i && y%9==0);affine.cache=i;
            affine.cnt=uint16_t((i?0x4000:0)|((y%5)?0x2000:0)|(i+1));
            affine.x=int64_t(y)*-711-2398;affine.v=int64_t(y)*621-997;
            affine.pa=i?-32768:32767;affine.pc=i?1331:-1733;
            affine.dx=(mode&1)?134217727:-333;affine.dv=(mode&1)?-134217727:719;
            affine.dpa=i?65535:-65535;affine.dpc=i?-4177:3753;
        }
    }
    return frame;
}
void compare(const std::vector<uint8_t>& expected,unsigned full_w,unsigned full_h,
             const std::vector<uint8_t>& actual,unsigned width,unsigned height) {
    CHECK(actual.size()==size_t(width)*height*3);
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
        const size_t source=(uint64_t(y)*full_h/height)*full_w+uint64_t(x)*full_w/width;
        const size_t destination=size_t(y)*width+x;
        if(std::memcmp(expected.data()+source*3,actual.data()+destination*3,3)) {
            const auto* a=actual.data()+destination*3;const auto* e=expected.data()+source*3;
            std::fprintf(stderr,"pixel %u,%u actual=%u,%u,%u expected=%u,%u,%u\n",x,y,a[0],a[1],a[2],e[0],e[1],e[2]);
            std::exit(1);
        }
    }
}
}
int main() {
    if(SDL_Init(SDL_INIT_VIDEO)!=0)return 77;
    SDL_Window* window=SDL_CreateWindow("GBA hidden GPU contract",0,0,257,160,SDL_WINDOW_HIDDEN);
    if(!window){SDL_Quit();return 77;}
    int driver=-1;
    for(int i=0;i<SDL_GetNumRenderDrivers();++i){SDL_RendererInfo info{};
        if(SDL_GetRenderDriverInfo(i,&info)==0 && info.name && !std::strcmp(info.name,"opengl"))driver=i;}
    SDL_Renderer* renderer=driver>=0?SDL_CreateRenderer(window,driver,SDL_RENDERER_ACCELERATED|SDL_RENDERER_TARGETTEXTURE):nullptr;
    if(!renderer){SDL_DestroyWindow(window);SDL_Quit();return 77;}
    SDL_Texture* activation=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,257,160);
    CHECK(activation);
    CHECK(SDL_GL_BindTexture(activation,nullptr,nullptr)==0);
    using GetString=const GLubyte*(APIENTRY*)(GLenum);
    auto get_string=reinterpret_cast<GetString>(SDL_GL_GetProcAddress("glGetString"));
    const char* version=get_string?reinterpret_cast<const char*>(get_string(GL_VERSION)):nullptr;
    int major=0;if(version)std::sscanf(version,"%d",&major);
    std::fprintf(stderr,"GPU contract context=%s\n",version?version:"unavailable");
    SDL_GL_UnbindTexture(activation);
    gbarecomp::HdGpuRenderer gpu;
    if(!gpu.initialize(renderer,activation)) {
        std::fprintf(stderr,"GPU contract %s: optional consumer unavailable\n",major>=3?"FAILED":"skipped");
        gpu.reset();SDL_DestroyTexture(activation);SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_Quit();return major>=3?1:77;
    }
    unsigned cases=0;
    for(unsigned scale:{2u,3u,8u,16u})for(unsigned mode=0;mode<4;++mode) {
        auto frame=fixture(scale,mode);auto raw=Hd::render_jobs(frame);
        SDL_RenderSetLogicalSize(renderer,257,160);
        SDL_SetRenderDrawColor(renderer,8,19,27,255);SDL_RenderClear(renderer);
        SDL_Rect viewport{};SDL_RenderGetViewport(renderer,&viewport);
        SDL_Texture* texture=gpu.render(frame,nullptr);CHECK(texture);
        CHECK(SDL_GetRenderTarget(renderer)==nullptr);
        SDL_Rect after{};SDL_RenderGetViewport(renderer,&after);CHECK(std::memcmp(&viewport,&after,sizeof(viewport))==0);
        std::vector<uint8_t> actual;unsigned width=0,height=0;
        CHECK(gpu.readback(actual,width,height));compare(raw,frame.width*scale,frame.height*scale,actual,width,height);
        // SDL must still consume the resident image with the same orientation.
        SDL_Texture* copy=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_TARGET,int(width),int(height));CHECK(copy);
        CHECK(SDL_SetRenderTarget(renderer,copy)==0);CHECK(SDL_RenderCopy(renderer,texture,nullptr,nullptr)==0);
        std::vector<uint8_t> presented(size_t(width)*height*3);
        CHECK(SDL_RenderReadPixels(renderer,nullptr,SDL_PIXELFORMAT_RGB24,presented.data(),int(width*3))==0);
        CHECK(presented==actual);CHECK(SDL_SetRenderTarget(renderer,nullptr)==0);SDL_DestroyTexture(copy);
        if(scale==3)for(auto kind:{gbarecomp::runtime::ScreenKind::Unlit,gbarecomp::runtime::ScreenKind::Frontlit,
            gbarecomp::runtime::ScreenKind::Backlit,gbarecomp::runtime::ScreenKind::Classic}) {
            gbarecomp::runtime::ColorLut lut(gbarecomp::runtime::ColorSettings{kind});
            std::vector<uint8_t> expected(raw.size());lut.map_rgb888(raw.data(),expected.data(),int(frame.width*scale),int(frame.height*scale));
            CHECK(gpu.render(frame,&lut));CHECK(gpu.readback(actual,width,height));
            compare(expected,frame.width*scale,frame.height*scale,actual,width,height);++cases;
        }
        // Invalid inputs fall back before changing the SDL target or output.
        frame.lines[1].affine[0].cache=2;CHECK(!gpu.render(frame,nullptr));CHECK(SDL_GetRenderTarget(renderer)==nullptr);
        ++cases;
    }
    // A software renderer must never enter GL, but keeps the same caller ABI.
    SDL_Window* other_window=SDL_CreateWindow("GBA hidden portable contract",0,0,16,16,SDL_WINDOW_HIDDEN);CHECK(other_window);
    SDL_Renderer* software=SDL_CreateRenderer(other_window,-1,SDL_RENDERER_SOFTWARE);CHECK(software);
    SDL_Texture* software_texture=SDL_CreateTexture(software,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,16,16);CHECK(software_texture);
    gbarecomp::HdGpuRenderer portable;CHECK(!portable.initialize(software,software_texture));
    SDL_DestroyTexture(software_texture);SDL_DestroyRenderer(software);SDL_DestroyWindow(other_window);
    gpu.reset();SDL_DestroyTexture(activation);SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_Quit();
    std::printf("HD GPU: %u exact full readback/presentation cases; two affine layers, signed extremes, window/blend/filter/brightness, per-row palettes/versions, upscale RGB, grading, resize and portable rejection passed\n",cases);
    return 0;
}
#else
int main(){std::puts("GPU contract skipped: SDL unavailable");return 77;}
#endif
