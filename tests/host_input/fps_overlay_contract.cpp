#define SDL_MAIN_HANDLED
// Link with the production HostWindow/runtime/UI libraries and GNU ld
// --wrap=SDL_RenderPresent. Optional argv[1] supplies a 240x160 RGB game frame;
// otherwise this uses a generated pattern. Run under SDL dummy/software only.
// Captures occur before presentation and preserve the raw viewport and scale.
#include <SDL.h>
#include "host_window.h"
#include "recomp_runtime_ui.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
#include <map>
#include <cmath>
#include <algorithm>
// This host-presentation fixture never executes guest instructions. Keep the
// normal empty cart-dispatch contract used by the existing runtime fixture.
struct DispatchEntry { unsigned pc; unsigned char thumb,resume; void (*fn)(); };
extern "C" const DispatchEntry kDispatchTable[]={{0xFFFFFFFFu,0,0,nullptr}};
extern "C" const unsigned kDispatchTableLen=0;
static const char* capture_name=nullptr;
static bool write_captures=false;
struct Capture {int w,h;std::vector<unsigned char> rgba;};
static std::map<std::string,Capture> captures;
extern "C" void __real_SDL_RenderPresent(SDL_Renderer*);
extern "C" void __wrap_SDL_RenderPresent(SDL_Renderer* r) {
 if(capture_name) {
  int w=0,h=0;SDL_GetRendererOutputSize(r,&w,&h);
  int lw=0,lh=0;float sx=1,sy=1;SDL_Rect viewport{};SDL_RenderGetLogicalSize(r,&lw,&lh);SDL_RenderGetScale(r,&sx,&sy);SDL_RenderSetScale(r,1,1);SDL_RenderGetViewport(r,&viewport);
  SDL_RenderSetLogicalSize(r,0,0);SDL_RenderSetViewport(r,nullptr);
  std::vector<unsigned char> rgba(w*h*4);
  if(SDL_RenderReadPixels(r,nullptr,SDL_PIXELFORMAT_RGBA32,rgba.data(),w*4)!=0)std::abort();
  SDL_RenderSetLogicalSize(r,lw,lh);SDL_RenderSetScale(r,1,1);SDL_RenderSetViewport(r,&viewport);SDL_RenderSetScale(r,sx,sy);
  captures[capture_name]={w,h,rgba};
  if(write_captures){std::string path=std::string(capture_name)+".rgba";FILE* f=std::fopen(path.c_str(),"wb");if(!f)std::abort();std::fwrite(rgba.data(),1,rgba.size(),f);std::fclose(f);}
  std::printf("CAPTURE %s %d %d\n",capture_name,w,h);std::fflush(stdout);
 }
 __real_SDL_RenderPresent(r);
}
static bool same(const char* a,const char* b) {return captures.at(a).rgba==captures.at(b).rgba;}
static bool panel_only(const char* a,const char* b,float mm) {
 const auto& before=captures.at(a);const auto& after=captures.at(b);if(before.w!=after.w||before.h!=after.h)return false;
 int left=0,top=0;if(const char* insets=std::getenv("GBARECOMP_SAFE_INSETS"))std::sscanf(insets,"%d,%d",&left,&top);
 int x0=before.w,y0=before.h,x1=-1,y1=-1,count=0;
 for(int y=0;y<before.h;++y)for(int x=0;x<before.w;++x){auto i=(y*before.w+x)*4;if(!std::equal(before.rgba.begin()+i,before.rgba.begin()+i+3,after.rgba.begin()+i)){++count;x0=std::min(x0,x);y0=std::min(y0,y);x1=std::max(x1,x);y1=std::max(y1,y);}}
 std::printf("PANEL %s changed=%d bounds=%d,%d,%d,%d\n",a,count,x0,y0,x1+1,y1+1);
 return count>100&&std::abs(x0-(left+2*mm))<4&&std::abs(y0-(top+2*mm))<4&&x1-x0<200&&y1-y0<50;
}
int main(int argc,char**argv) {
 write_captures=argc>1;
 SDL_SetMainReady();SDL_setenv("SDL_VIDEODRIVER","dummy",1);SDL_setenv("SDL_AUDIODRIVER","dummy",1);SDL_setenv("GBARECOMP_WINDOW_HIDDEN","1",1);SDL_SetHint(SDL_HINT_RENDER_DRIVER,"software");
 std::vector<unsigned char> rgb(240*160*3);
 if(argc>1){FILE* file=std::fopen(argv[1],"rb");if(!file)return 3;auto size=std::fread(rgb.data(),1,rgb.size(),file);std::fclose(file);if(size!=rgb.size())return 4;}
 else for(int y=0;y<160;++y)for(int x=0;x<240;++x){auto i=(y*240+x)*3;rgb[i]=x;rgb[i+1]=y;rgb[i+2]=(x^y);}
 RecompRuntimeUiItem item{};item.key="check";item.label="Overlay qualification";item.section="Display";item.type=RECOMP_RUNTIME_UI_ACTION;
 RecompRuntimeUiConfig config{};config.title="Prepared overlay check";config.items=&item;config.item_count=1;auto* ui=recomp_runtime_ui_create(&config);if(!ui)return 5;
 float mm=0;{
  gbarecomp::HostWindow window;if(!window.open(3,240,160,"Overlay qualification"))return 6;window.set_runtime_ui(ui);
  if(window.fps_readout())return 7;
  mm=window.px_per_mm();std::printf("DENSITY %.6f\n",mm);
  auto render=[&](const char* name){capture_name=name;window.present(rgb.data());capture_name=nullptr;};
  render("off");window.set_fps_readout(true);auto start=SDL_GetTicks();while(SDL_GetTicks()-start<650){window.present(rgb.data());SDL_Delay(15);}render("on");
  window.set_fps_readout(false);render("off-again");
  recomp_runtime_ui_open(ui);window.present(rgb.data());render("menu-off");window.set_fps_readout(true);start=SDL_GetTicks();while(SDL_GetTicks()-start<650){window.present(rgb.data());SDL_Delay(15);}render("menu-on");
  recomp_runtime_ui_close(ui);window.set_fps_readout(false);window.set_fullscreen(1);for(int i=0;i<10;++i){window.pump();window.present(rgb.data());SDL_Delay(15);}render("fullscreen-off");window.set_fps_readout(true);start=SDL_GetTicks();while(SDL_GetTicks()-start<650){window.present(rgb.data());SDL_Delay(15);}render("fullscreen-on");
  window.set_runtime_ui(nullptr);
 }
 recomp_runtime_ui_destroy(ui);SDL_Quit();
 bool pass=same("off","off-again")&&same("menu-off","menu-on")&&!same("off","menu-off")&&panel_only("off","on",mm)&&panel_only("fullscreen-off","fullscreen-on",mm);
 std::printf("FPS overlay contract: %s\n",pass?"PASS":"FAIL");return pass?0:8;
}
