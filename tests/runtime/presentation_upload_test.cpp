#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "color_lut.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s (%s)\n",__LINE__,#x,SDL_GetError());std::exit(1); } } while(0)
int main() {
    SDL_SetMainReady();SDL_setenv("SDL_VIDEODRIVER","dummy",1);
    CHECK(SDL_Init(SDL_INIT_VIDEO)==0);
    auto* window=SDL_CreateWindow("upload contract",0,0,64,32,SDL_WINDOW_HIDDEN);CHECK(window);
    auto* renderer=SDL_CreateRenderer(window,-1,SDL_RENDERER_SOFTWARE);CHECK(renderer);
    constexpr int width=32,height=16;
    std::vector<uint8_t> source(width*height*3),graded(source.size()),actual(64*32*3),reference(actual.size());
    std::vector<uint32_t> packed(width*height);
    for(std::size_t i=0;i<source.size();++i)source[i]=uint8_t(i*73u+(i>>4)*19u);
    using namespace gbarecomp::runtime;
    for(const auto kind:{ScreenKind::Raw,ScreenKind::Unlit,ScreenKind::Frontlit,ScreenKind::Backlit,ScreenKind::Classic}) {
        ColorSettings settings{};settings.screen=kind;ColorLut colors(settings);
        colors.map_rgb888(source.data(),graded.data(),width,height);
        for(const Uint32 format:{SDL_PIXELFORMAT_RGB24,SDL_PIXELFORMAT_ARGB8888,SDL_PIXELFORMAT_ABGR8888}) {
            auto* texture=SDL_CreateTexture(renderer,format,SDL_TEXTUREACCESS_STREAMING,width,height);CHECK(texture);
            CHECK(SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_NONE)==0);
            if(format==SDL_PIXELFORMAT_RGB24)CHECK(SDL_UpdateTexture(texture,nullptr,graded.data(),width*3)==0);
            else {
                ColorLut native_colors(settings,format==SDL_PIXELFORMAT_ABGR8888 ? UploadPixelOrder::Abgr8888 : UploadPixelOrder::Argb8888);
                native_colors.map_packed8888(source.data(),packed.data(),width,height,format==SDL_PIXELFORMAT_ABGR8888);
                CHECK(SDL_UpdateTexture(texture,nullptr,packed.data(),width*4)==0);
            }
            CHECK(SDL_SetRenderDrawColor(renderer,17,29,41,255)==0);CHECK(SDL_RenderClear(renderer)==0);
            CHECK(SDL_RenderCopy(renderer,texture,nullptr,nullptr)==0);
            CHECK(SDL_RenderReadPixels(renderer,nullptr,SDL_PIXELFORMAT_RGB24,actual.data(),64*3)==0);
            if(format==SDL_PIXELFORMAT_RGB24)reference=actual;
            else CHECK(reference==actual);
            SDL_DestroyTexture(texture);
        }
    }
    SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_Quit();
    std::puts("presentation upload: raw/graded RGB24, ARGB8888 and ABGR8888 render identically");
}
