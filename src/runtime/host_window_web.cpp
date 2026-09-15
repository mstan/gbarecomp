#include "host_window.h"
#include "host_web_shared.h"
#include "color_lut.h"
#include <emscripten.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>

namespace gbarecomp {
namespace {
using namespace web;
struct Backend {
    Shared shared{};
    int width=240, height=160, scale=3;
    uint32_t back=2, seq=0, epoch=0;
    bool assist=true, resize=false;
    int turbo=4;
    std::unique_ptr<runtime::ColorLut> lut;
};
Backend* backend(void* p) { return static_cast<Backend*>(p); }
bool dimensions(int w,int h) { return w>0 && h>0 && w<=int(gba::GbaPpu::kMaxRenderWidth) && h<=int(gba::GbaPpu::kMaxRenderHeight); }
std::string descriptor() {
    std::ostringstream o;
    o << "{\"magic\":" << Magic << ",\"version\":" << Version << ",\"bytes\":" << sizeof(Shared) << ",\"fields\":{";
#define FIELD(name) o << "\"" #name "\":" << offsetof(Control,name)/4 << ',';
    GBR_WEB_FIELDS(FIELD)
#undef FIELD
    o << "\"_end\":0},\"video\":" << offsetof(Shared,video)
      << ",\"videoStride\":" << sizeof(VideoSlot) << ",\"pixels\":" << offsetof(VideoSlot,pixels)
      << ",\"pixelBytes\":" << PixelBytes << ",\"maxWidth\":" << gba::GbaPpu::kMaxRenderWidth
      << ",\"maxHeight\":" << gba::GbaPpu::kMaxRenderHeight
      << ",\"audio\":" << offsetof(Shared,audio) << ",\"audioStride\":" << sizeof(AudioSlot)
      << ",\"audioSamples\":" << AudioSamples << ",\"audioSlots\":" << AudioSlots
      << ",\"commands\":" << offsetof(Shared,commands) << ",\"commandSlots\":" << CommandSlots << '}';
    return o.str();
}
std::string read_file(const std::string& p) { std::ifstream f(p); return {std::istreambuf_iterator<char>(f), {}}; }
}
HostWindow::HostWindow()=default;
HostWindow::~HostWindow(){close();}
bool HostWindow::is_available(){return true;}
bool HostWindow::open(int scale,int w,int h,const char*,const char* screen,bool linear,bool sharp,bool resize,bool) {
    close();
    if(!dimensions(w,h)) return false;
    auto b=std::make_unique<Backend>(); b->width=w; b->height=h; b->scale=std::clamp(scale,1,8); b->resize=resize;
    runtime::ColorSettings settings;
    if(const char* env=std::getenv("GBARECOMP_SCREEN")) screen=env;
    if(screen && !runtime::screen_kind_from_name(screen,settings.screen))
        std::fprintf(stderr,"host_web: unknown screen model %s; using raw\n",screen);
    b->lut=std::make_unique<runtime::ColorLut>(settings);
    auto& c=b->shared.control;
    static uint32_t generation=0;
    c.magic=Magic; c.version=Version; c.bytes=sizeof(Shared); c.generation=++generation;
    c.middle=1; c.keys=1023; c.scale=b->scale; c.volume=100; c.audioEnabled=1;
    c.filter=sharp?2:linear?1:0; c.drawable=(w*b->scale)|(h*b->scale<<16);
    auto d=descriptor();
    const int attached=MAIN_THREAD_EM_ASM_INT({
        try { return globalThis.GbrHost.attach(()=>wasmMemory.buffer,$0,JSON.parse(UTF8ToString($1)))?1:0; }
        catch(e) { console.error(e); return 0; }
    }, &b->shared,d.c_str());
    if(!attached) return false;
    impl_=b.release(); open_=true;
    // Setup only: the page remains free to finish the worklet handshake.
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    while(!backend(impl_)->shared.control.audioReady.load() && std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    uint32_t pending=0;
    if(backend(impl_)->shared.control.audioReady.compare_exchange_strong(pending,2)) {
        // A missing audio device never takes video down. A late READY from the
        // worklet may still enable playback; the state stays observable.
        std::fprintf(stderr,"host_web: audio handshake timed out; audio unavailable\n");
    }
    backend(impl_)->shared.control.state=Running;
    std::fprintf(stderr,"host_backend=web abi=%u generation=%u\n",Version,generation);
    return true;
}
void HostWindow::close() {
    if(!impl_) return;
    auto* b=backend(impl_); auto& c=b->shared.control; c.state=Stopping;
    MAIN_THREAD_EM_ASM({ globalThis.GbrHost.detach(); });
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(c.detached.load(std::memory_order_acquire)==DetachPending) {
        if(std::chrono::steady_clock::now()>=deadline) {
            // Never free storage that a late AudioWorklet can still access.
            std::fprintf(stderr,"host_web: detach timeout; retaining backend storage\n");
            impl_=nullptr; open_=false;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if(c.detached.load(std::memory_order_acquire)==DetachSafeToFree) {
        delete b;
    } else {
        std::fprintf(stderr,"host_web: audio detach not confirmed; retaining backend storage\n");
    }
    impl_=nullptr; open_=false;
}
bool HostWindow::set_surface_size(int w,int h) { if(!impl_||!dimensions(w,h))return false; backend(impl_)->width=w;backend(impl_)->height=h;return true; }
bool HostWindow::drawable_size(int* w,int* h) const { if(!impl_||!w||!h)return false; auto d=backend(impl_)->shared.control.drawable.load(); *w=d&65535;*h=d>>16;return *w&&*h; }
void HostWindow::present(const uint8_t* rgb) {
    if(!impl_||!rgb)return;
    auto& b=*backend(impl_);auto& s=b.shared;auto& v=s.video[b.back];
    v.width=b.width;v.height=b.height;v.stride=b.width*3;v.seq=++b.seq;
    b.lut->map_rgb888(rgb,v.pixels,b.width,b.height);
    const auto old=s.control.middle.exchange(b.back|Dirty,std::memory_order_acq_rel);
    b.back=old&3; if(old&Dirty)++s.control.replaced; ++s.control.published;
}
void HostWindow::push_audio_samples(const int16_t* x,std::size_t n){push_audio_block(x,n,65536);}
void HostWindow::push_audio_block(const int16_t* x,std::size_t n,uint32_t rate) {
    if(!impl_||!x||!(rate==32768||rate==65536||rate==131072||rate==262144))return;
    auto& b=*backend(impl_);auto& s=b.shared;auto& c=s.control;
    if(c.audioReady!=1 || c.resetAck.load()!=c.resetRequest.load())return;
    while(n) {
        uint32_t w=c.audioWrite.load(std::memory_order_relaxed),r=c.audioRead.load(std::memory_order_acquire);
        if(uint32_t(w-r)>=AudioSlots){c.audioOverflow.fetch_add(n);reset_audio();return;}
        auto& a=s.audio[w&(AudioSlots-1)]; a.count=std::min(n,std::size_t(AudioSamples)); a.rate=rate;a.epoch=b.epoch;
        std::copy_n(x,a.count,a.samples); x+=a.count;n-=a.count;
        c.audioPublished.fetch_add(a.count);c.audioWrite.store(w+1,std::memory_order_release);
    }
}
void HostWindow::reset_audio(){if(impl_){auto& b=*backend(impl_);++b.epoch;b.shared.control.resetRequest=b.epoch;}}
bool HostWindow::auto_paused()const{return impl_&&backend(impl_)->shared.control.hidden.load();}
void HostWindow::report_paused(bool p){if(impl_)backend(impl_)->shared.control.paused=p;}
HostWindow::Events HostWindow::pump() {
    Events e;if(!impl_)return e;auto& s=backend(impl_)->shared;auto& c=s.control;
    e.keyinput=c.keys.load();c.appliedKeys=e.keyinput;e.quit=c.quit.load();e.fast_forward=c.turbo.load();
    // Events has one entry per action: consume one command per pump so repeated
    // saves/loads/pauses cannot be collapsed by this legacy interface.
    uint32_t r=c.commandRead.load(std::memory_order_relaxed);
    if(r!=c.commandWrite.load(std::memory_order_acquire)) {
        auto cmd=s.commands[r&(CommandSlots-1)];
        switch(cmd.kind){case Pause:e.toggle_pause=true;break;case Save:e.save_slot=cmd.arg;break;case Load:e.load_slot=cmd.arg;break;
        case Bigger:e.window_bigger=true;break;case Smaller:e.window_smaller=true;break;case VolumeUp:e.volume_up=true;break;case VolumeDown:e.volume_down=true;break;
        case Fps:e.toggle_fps=true;break;case Rewind:e.rewind=true;break;case SolarUp:e.solar_brighter=true;break;case SolarDown:e.solar_dimmer=true;break;case SolarLive:e.solar_live=true;break;}
        c.commandRead.store(r+1,std::memory_order_release);
    }
    return e;
}
void HostWindow::service_events(){} // All DOM service belongs to the page; do not consume commands here.
void HostWindow::load_input_config(const char* dir,bool assist,int turbo) {
    if(!impl_)return;auto& b=*backend(impl_);b.assist=assist;b.turbo=std::clamp(turbo,2,10);
    std::string base=dir?dir:".";base+='/';auto keys=read_file(base+"keybinds.ini"),config=read_file(base+"config.ini");
    MAIN_THREAD_EM_ASM({globalThis.GbrHost.configure(UTF8ToString($0),UTF8ToString($1));},keys.c_str(),config.c_str());
}
void HostWindow::set_fullscreen(int mode){if(impl_)MAIN_THREAD_EM_ASM({globalThis.GbrHost.fullscreenRequest($0);},std::clamp(mode,0,2));}
int HostWindow::fullscreen()const{return impl_?backend(impl_)->shared.control.fullscreen.load():0;}
void HostWindow::adjust_scale(int d){if(impl_){auto& b=*backend(impl_);b.scale=std::clamp(b.scale+d,1,8);b.shared.control.scale=b.scale;}}
int HostWindow::window_scale()const{return impl_?backend(impl_)->scale:1;}
#define CONTROL_SETGET(setter,getter,field,type,defaultValue,expr) \
void HostWindow::setter(type v){if(impl_)backend(impl_)->shared.control.field=(expr);} \
type HostWindow::getter()const{return impl_?backend(impl_)->shared.control.field.load():defaultValue;}
CONTROL_SETGET(set_volume,volume,volume,int,100,std::clamp(v,0,100))
CONTROL_SETGET(set_audio_enabled,audio_enabled,audioEnabled,bool,false,v)
CONTROL_SETGET(set_fps_readout,fps_readout,fps,bool,false,v)
void HostWindow::set_linear_filter(bool v){if(impl_)backend(impl_)->shared.control.filter=v?1:0;}
bool HostWindow::linear_filter()const{return impl_&&backend(impl_)->shared.control.filter.load()==1;}
void HostWindow::set_resize_driven_view(bool v){if(impl_)backend(impl_)->resize=v;}
bool HostWindow::assist_tools_enabled()const{return impl_&&backend(impl_)->assist;}
int HostWindow::fast_forward_multiplier()const{return impl_?backend(impl_)->turbo:4;}
void web_notify_storage_write(WebStorageWrite kind,bool ok) {
    // Async proxy: only integers cross, so nothing can be freed before it runs.
    MAIN_THREAD_ASYNC_EM_ASM({ globalThis.GbrSaves?.onWrite($0,$1); },static_cast<int>(kind),ok?1:0);
}
}
