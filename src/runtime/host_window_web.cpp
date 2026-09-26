#include "host_window.h"
#include "host_web_shared.h"
#include "host_overlay.h"
#include "presentation_layout.h"
#include "touch_input.h"
#include "color_lut.h"
#include <emscripten.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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
    // Touch (touch_input.h). The page owns the virtual pad (its presses arrive
    // in `keys` through the touch mask) and forwards every other pointer
    // through the touch ring; pump() feeds those to the TouchHub.
    bool touch_policy=false, touch_emulation=false;
    std::uint32_t touch_claims=0;
    std::uint32_t margin_left=0, margin_right=0, margin_top=0, margin_bottom=0;
    void (*host_overlay)(HostOverlay*)=nullptr;
    // Development overrides, as on the native backend.
    float dpi_override=0.0f;                 // GBARECOMP_TOUCH_DPI
    bool insets_from_env=false;              // GBARECOMP_SAFE_INSETS
    TouchFrameInfo::Insets env_insets{};
    // Page visibility lifecycle (see pump / wait_for_foreground).
    bool backgrounded=false;
};
Backend* backend(void* p) { return static_cast<Backend*>(p); }
bool dimensions(int w,int h) { return w>0 && h>0 && w<=int(gba::GbaPpu::kMaxRenderWidth) && h<=int(gba::GbaPpu::kMaxRenderHeight); }

// The open window that owns the page vibrator (one per process).
HostWindow* g_haptic_window=nullptr;
// Settings-menu requests from game policies (any thread; consumed by pump).
std::atomic<bool> g_settings_menu_requested{false};

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
      << ",\"overlay\":" << offsetof(VideoSlot,overlayCount) << ",\"overlayCmds\":" << OverlayCmds
      << ",\"overlayStride\":" << sizeof(OverlayCmd)
      << ",\"audio\":" << offsetof(Shared,audio) << ",\"audioStride\":" << sizeof(AudioSlot)
      << ",\"audioSamples\":" << AudioSamples << ",\"audioSlots\":" << AudioSlots
      << ",\"commands\":" << offsetof(Shared,commands) << ",\"commandSlots\":" << CommandSlots
      << ",\"touches\":" << offsetof(Shared,touches) << ",\"touchSlots\":" << TouchSlots
      << ",\"touchStride\":" << sizeof(TouchSlot) << '}';
    return o.str();
}
std::string read_file(const std::string& p) { std::ifstream f(p); return {std::istreambuf_iterator<char>(f), {}}; }

bool touch_input_enabled(const Backend& b) {
    return b.shared.control.touchControls.load() || b.touch_policy || b.touch_emulation;
}

// Presentation facts for touch mapping and the host overlay. The page is the
// authority on where it drew the last frame (viewXY/viewWH); before its first
// draw the worker derives the same layout the page will use.
struct Geometry {
    int dw=0, dh=0;
    PresentationLayout layout{};
    float px_per_mm=96.0f/25.4f;
    TouchFrameInfo::Insets insets{};
};
Geometry geometry(const Backend& b) {
    const auto& c=b.shared.control;
    Geometry g;
    const uint32_t d=c.drawable.load();
    g.dw=int(d&65535); g.dh=int(d>>16);
    const uint32_t lt=c.insetsLT.load(), rb=c.insetsRB.load();
    g.insets=b.insets_from_env ? b.env_insets
        : TouchFrameInfo::Insets{int(lt&65535),int(lt>>16),int(rb&65535),int(rb>>16)};
    const uint32_t xy=c.viewXY.load(), wh=c.viewWH.load();
    if(wh) {
        g.layout={int(xy&65535),int(xy>>16),int(wh&65535),int(wh>>16),0};
        if(b.width>0 && b.height>0 && g.layout.width%b.width==0 && g.layout.height%b.height==0 &&
           g.layout.width/b.width==g.layout.height/b.height)
            g.layout.integer_scale=g.layout.width/b.width;
    } else {
        g.layout=compute_presentation_layout(g.dw,g.dh,b.width,b.height);
        if(c.anchorTop.load() && g.dh>g.dw && g.layout.height>0 && g.layout.height<g.dh)
            g.layout.y=std::min(g.insets.top,g.dh-g.layout.height);
    }
    // CSS defines 96 px per inch; devicePixelRatio converts to drawable pixels.
    const uint32_t dpr=c.dprMilli.load();
    g.px_per_mm=b.dpi_override>0.0f ? b.dpi_override/25.4f
        : (dpr ? float(dpr)/1000.0f : 1.0f)*96.0f/25.4f;
    return g;
}
void publish_touch_presentation(const Backend& b,const Geometry& g) {
    TouchPresentation p;
    p.drawable_width=g.dw; p.drawable_height=g.dh; p.layout=g.layout;
    p.view_width=b.width; p.view_height=b.height;
    p.extra_left=b.margin_left; p.extra_right=b.margin_right;
    p.extra_top=b.margin_top; p.extra_bottom=b.margin_bottom;
    p.drawable_px_per_mm=g.px_per_mm; p.safe_insets=g.insets;
    TouchHub::instance().set_presentation(p);
}

// HostOverlay as a display list in the video slot being published. The page
// replays it on a 2D canvas over the WebGL image (host_web.js drawOverlay).
// There is no font stack in the browser host: text is reported unsupported.
class WebOverlay final : public HostOverlay {
public:
    WebOverlay(const Backend& b,const Geometry& g,VideoSlot& slot,std::uint32_t now)
        : b_(b), g_(g), slot_(slot), now_(now) {}
    int drawable_width() const override { return g_.dw; }
    int drawable_height() const override { return g_.dh; }
    PresentationLayout game_rect() const override { return g_.layout; }
    int view_width() const override { return b_.width; }
    int view_height() const override { return b_.height; }
    float drawable_px_per_mm() const override { return g_.px_per_mm; }
    Insets safe_insets() const override { return {g_.insets.left,g_.insets.top,g_.insets.right,g_.insets.bottom}; }
    std::uint32_t host_ms() const override { return now_; }
    void to_drawable(float x,float y,OverlaySpace space,float* dx,float* dy) const override {
        if(space==OverlaySpace::Drawable) { *dx=x; *dy=y; return; }
        logical_point_to_presentation(g_.layout,b_.width,b_.height,x,y,dx,dy);
    }
    void line(float x0,float y0,float x1,float y1,float t,OverlayColor c,OverlaySpace s) override {
        float a,b,d,e; to_drawable(x0,y0,s,&a,&b); to_drawable(x1,y1,s,&d,&e);
        push(OverlayLine,c,{a,b,d,e,t*scale(s),0});
    }
    void polyline(const OverlayPoint* p,std::size_t n,float t,OverlayColor c,OverlaySpace s) override {
        for(std::size_t i=1;p&&i<n;++i) line(p[i-1].x,p[i-1].y,p[i].x,p[i].y,t,c,s);
    }
    void fill_rect(float x,float y,float w,float h,float r,OverlayColor c,OverlaySpace s) override {
        float dx,dy; to_drawable(x,y,s,&dx,&dy); const float k=scale(s);
        push(OverlayFillRect,c,{dx,dy,w*k,h*k,r*k,0});
    }
    void stroke_rect(float x,float y,float w,float h,float r,float t,OverlayColor c,OverlaySpace s) override {
        float dx,dy; to_drawable(x,y,s,&dx,&dy); const float k=scale(s);
        push(OverlayStrokeRect,c,{dx,dy,w*k,h*k,r*k,t*k});
    }
    void fill_circle(float x,float y,float r,OverlayColor c,OverlaySpace s) override {
        float dx,dy; to_drawable(x,y,s,&dx,&dy); push(OverlayFillCircle,c,{dx,dy,r*scale(s),0,0,0});
    }
    void stroke_circle(float x,float y,float r,float t,OverlayColor c,OverlaySpace s) override {
        float dx,dy; to_drawable(x,y,s,&dx,&dy); const float k=scale(s);
        push(OverlayStrokeCircle,c,{dx,dy,r*k,t*k,0,0});
    }
    void arc(float x,float y,float r,float t,float a0,float a1,OverlayColor c,OverlaySpace s) override {
        float dx,dy; to_drawable(x,y,s,&dx,&dy); const float k=scale(s);
        push(OverlayArc,c,{dx,dy,r*k,t*k,a0,a1});
    }
    bool text_supported() const override { return false; }
    void text(float,float,float,OverlayColor,const char*,OverlayAlign,OverlaySpace) override {}
    float text_width(const char*,float,OverlaySpace) const override { return 0.0f; }
private:
    float scale(OverlaySpace s) const {
        if(s==OverlaySpace::Drawable || b_.width<=0) return 1.0f;
        return float(g_.layout.width)/float(b_.width);
    }
    void push(OverlayKind kind,OverlayColor c,std::initializer_list<float> v) {
        for(float x:v) if(!std::isfinite(x)) return;  // never hand NaN/Inf to the page
        if(slot_.overlayCount>=OverlayCmds) { ++slot_.overlayDropped; return; }
        OverlayCmd& cmd=slot_.overlay[slot_.overlayCount++];
        cmd.kind=kind; cmd.rgba=uint32_t(c.r)|uint32_t(c.g)<<8|uint32_t(c.b)<<16|uint32_t(c.a)<<24;
        std::copy(v.begin(),v.end(),cmd.v);
    }
    const Backend& b_; const Geometry& g_; VideoSlot& slot_; std::uint32_t now_;
};

// Progress ring for the engine-owned long press (it asks the page for its
// settings) when no game policy claims the gesture. Mirrors host_window.cpp.
void draw_engine_long_press(const Backend& b,const Geometry& g,HostOverlay& ov) {
    if(!touch_input_enabled(b) || (b.touch_claims&kTouchClaimLongPress)) return;
    const TouchHub& hub=TouchHub::instance();
    const std::uint32_t hold=hub.gesture_config().long_press_ms, now=touch_clock_ms();
    for(const TouchPointSnapshot& pt:hub.points()) {
        if(pt.moved || pt.long_pressed) continue;
        const std::uint32_t elapsed=now-pt.down_ms;
        if(elapsed<150 || hold==0) continue;
        const float progress=std::min(1.0f,float(elapsed)/float(hold));
        ov.arc(pt.drawable_x,pt.drawable_y,7.0f*g.px_per_mm,1.2f*g.px_per_mm,0.0f,progress,
               {80,210,255,200},OverlaySpace::Drawable);
    }
}

void request_page_settings() {
    MAIN_THREAD_ASYNC_EM_ASM({ globalThis.GbrHost?.requestSettings?.(); });
}

// Page pointer events -> TouchHub (drawable pixels; view space from the layout
// the page drew with). Always drains, so a disabled path cannot back up.
void drain_touches(Backend& b,const Geometry& g) {
    auto& s=b.shared; auto& c=s.control;
    uint32_t r=c.touchRead.load(std::memory_order_relaxed);
    const uint32_t w=c.touchWrite.load(std::memory_order_acquire);
    if(uint32_t(w-r)>TouchSlots) r=w-TouchSlots;  // never read unpublished slots
    const bool enabled=touch_input_enabled(b);
    TouchHub& hub=TouchHub::instance();
    for(;r!=w;++r) {
        const TouchSlot t=s.touches[r&(TouchSlots-1)];
        if(!enabled || !std::isfinite(t.x) || !std::isfinite(t.y)) continue;
        const uint32_t kind=t.kind&0xFF;
        const TouchSource source=(t.kind&TouchFromMouse) ? TouchSource::Mouse : TouchSource::Finger;
        const std::uint32_t now=touch_clock_ms();
        if(kind<=TouchCancel) {
            TouchEvent e;
            e.t_ms=now; e.pointer=t.pointer&0x7FFFFFFF; e.phase=static_cast<TouchPhase>(kind);
            e.source=source; e.drawable_x=t.x; e.drawable_y=t.y;
            e.in_view=presentation_point_to_logical(g.layout,b.width,b.height,t.x,t.y,&e.view_x,&e.view_y);
            hub.submit(e);
        } else if(kind==TouchTwoFingerTap || kind==TouchThreeFingerTap || kind==TouchBack) {
            Gesture ge;
            ge.kind=kind==TouchBack ? GestureKind::Back
                : kind==TouchTwoFingerTap ? GestureKind::TwoFingerTap : GestureKind::ThreeFingerTap;
            ge.fingers=kind==TouchThreeFingerTap ? 3 : kind==TouchTwoFingerTap ? 2 : 1;
            ge.t_ms=now; ge.source=source;
            if(kind!=TouchBack) {
                ge.drawable_x=ge.start_drawable_x=t.x; ge.drawable_y=ge.start_drawable_y=t.y;
                ge.start_in_view=presentation_point_to_logical(g.layout,b.width,b.height,t.x,t.y,&ge.view_x,&ge.view_y);
                ge.start_view_x=ge.view_x; ge.start_view_y=ge.view_y;
            }
            hub.submit_gesture(ge);
        }
    }
    c.touchRead.store(r,std::memory_order_release);
}
}
HostWindow::HostWindow()=default;
HostWindow::~HostWindow(){close();}
bool HostWindow::is_available(){return true;}
bool HostWindow::open(int scale,int w,int h,const char*,const char* screen,bool linear,bool sharp,bool resize,bool) {
    close();
    if(!dimensions(w,h)) return false;
    auto b=std::make_unique<Backend>(); b->width=w; b->height=h; b->scale=std::clamp(scale,1,8); b->resize=resize;
    if(const char* dpi=std::getenv("GBARECOMP_TOUCH_DPI")) {
        const float v=float(std::atof(dpi));
        if(v>=20.0f && v<=2000.0f) b->dpi_override=v;
    }
    if(const char* insets=std::getenv("GBARECOMP_SAFE_INSETS")) {
        int l=0,t=0,r=0,bo=0;
        if(std::sscanf(insets,"%d,%d,%d,%d",&l,&t,&r,&bo)==4) {
            b->env_insets={std::max(0,l),std::max(0,t),std::max(0,r),std::max(0,bo)};
            b->insets_from_env=true;
        }
    }
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
    // Orientation policy (0 landscape, 1 portrait, 2 any): the page applies it
    // as a screen orientation lock while it is fullscreen, where browsers allow one.
    const int attached=MAIN_THREAD_EM_ASM_INT({
        try { return globalThis.GbrHost.attach(()=>wasmMemory.buffer,$0,JSON.parse(UTF8ToString($1)),$2)?1:0; }
        catch(e) { console.error(e); return 0; }
    }, &b->shared,d.c_str(),orientation_policy_);
    if(!attached) return false;
    impl_=b.release(); open_=true;
    g_haptic_window=this;
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
    if(g_haptic_window==this) g_haptic_window=nullptr;
    TouchHub::instance().cancel_all(touch_clock_ms());
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
    const Geometry g=geometry(b);
    publish_touch_presentation(b,g);
    v.overlayCount=0; v.overlayDropped=0;
    {
        WebOverlay ov(b,g,v,touch_clock_ms());
        if(b.host_overlay) b.host_overlay(&ov);
        draw_engine_long_press(b,g,ov);
    }
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
    Events e;if(!impl_)return e;auto& b=*backend(impl_);auto& s=b.shared;auto& c=s.control;
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
    // Page visibility is the browser's foreground: hiding the page reports the
    // mobile background transition once (the runtime flushes the battery save,
    // writes its suspend state and then blocks in wait_for_foreground()). A
    // browser may discard a hidden tab without further notice, exactly like a
    // mobile OS. Unload cannot be delayed for an asynchronous IndexedDB sync,
    // so `terminating` is never reported.
    const bool hidden=c.hidden.load();
    if(hidden && !b.backgrounded && !e.quit) { b.backgrounded=true; e.enter_background=true; }
    else if(!hidden) b.backgrounded=false;  // visible again without a wait (quit path)
    const Geometry g=geometry(b);
    drain_touches(b,g);
    bool settings=false;
    if(touch_input_enabled(b)) {
        // Gesture timers (long press) run between drains so engine-owned
        // actions fire while the finger is still held.
        TouchHub& hub=TouchHub::instance();
        hub.advance(touch_clock_ms());
        settings=!hub.take_host_actions().empty();
    }
    if(g_settings_menu_requested.exchange(false)) settings=true;
    // The browser host has no in-canvas runtime menu: the page's own controls
    // are its settings, so an unclaimed long press / three-finger tap / Back,
    // or a policy's explicit request, asks the page to reveal them.
    if(settings) request_page_settings();
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

// ---- touch (touch_input.h) -------------------------------------------------
void HostWindow::configure_touch(bool policy,std::uint32_t claims,int pad_default,bool emulate) {
    if(!impl_)return;
    auto& b=*backend(impl_);
    b.touch_policy=policy; b.touch_claims=claims; b.touch_emulation=emulate;
    b.shared.control.touchMode=(policy?TouchModePolicy:0u)|(emulate?TouchModeEmulate:0u);
    TouchHub& hub=TouchHub::instance();
    hub.set_claims(policy?claims:0);
    // Same timing policy as the native backend.
    const bool game_long_press=policy && (claims&kTouchClaimLongPress);
    hub.set_timing(350,game_long_press?450:650);
    // The page owns the virtual pad (capability, default, saved choice) and
    // publishes touchControls/padVisible back through the control block.
    MAIN_THREAD_ASYNC_EM_ASM({ globalThis.GbrHost?.configureTouch?.($0!==0,$1,$2!==0); },
                             policy?1:0,pad_default,emulate?1:0);
    std::fprintf(stderr,"host_web: touch policy=%s claims=0x%x pad_default=%d emulation=%s\n",
                 policy?"game":"none",unsigned(claims),pad_default,emulate?"on":"off");
}
// The page persists the pad choice per game in localStorage (the browser
// analogue of <dir>/config.ini [Touch]); MEMFS paths do not survive a reload.
void HostWindow::set_touch_config_dir(const char*) {}
bool HostWindow::touch_pad_visible() const { return impl_ && backend(impl_)->shared.control.padVisible.load()!=0; }
void HostWindow::set_touch_pad_visible(bool visible) {
    if(!impl_)return;
    auto& c=backend(impl_)->shared.control;
    c.padVisible=visible && c.touchControls.load();
    MAIN_THREAD_ASYNC_EM_ASM({ globalThis.GbrHost?.setPadVisible?.($0!==0); },visible?1:0);
}
void HostWindow::set_view_margins(std::uint32_t l,std::uint32_t r,std::uint32_t t,std::uint32_t bo) {
    if(!impl_)return;
    auto& b=*backend(impl_); b.margin_left=l; b.margin_right=r; b.margin_top=t; b.margin_bottom=bo;
}
void HostWindow::set_host_overlay(void (*overlay)(HostOverlay*)) { if(impl_) backend(impl_)->host_overlay=overlay; }
float HostWindow::px_per_mm() const { return impl_ ? geometry(*backend(impl_)).px_per_mm : 96.0f/25.4f; }
void HostWindow::set_presentation_anchor_top(bool anchor_top) { if(impl_) backend(impl_)->shared.control.anchorTop=anchor_top?1:0; }
bool HostWindow::wait_for_foreground() {
    if(!impl_)return false;
    auto& b=*backend(impl_); auto& c=b.shared.control;
    std::fprintf(stderr,"host_web: page hidden — emulation suspended\n");
    TouchHub::instance().cancel_all(touch_clock_ms());
    report_paused(true); reset_audio();
    while(c.hidden.load() && !c.quit.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    b.backgrounded=false;
    const bool resumed=!c.quit.load();
    // A new playback epoch: nothing queued before the page was hidden replays.
    reset_audio(); report_paused(false);
    std::fprintf(stderr,"host_web: %s\n",resumed?"page visible — emulation resumed":"quit while hidden");
    return resumed;
}
void HostWindow::haptic_pulse(int duration_ms,float strength) {
    if(!impl_ || duration_ms<=0 || !(strength>0.0f)) return;
    // navigator.vibrate lives on the page (Window), has no amplitude control
    // and is absent on some browsers (then this is a no-op, reported once).
    MAIN_THREAD_ASYNC_EM_ASM({ globalThis.GbrHost?.haptic?.($0); },std::clamp(duration_ms,1,5000));
}
void host_haptic_pulse(int duration_ms,float strength) {
    if(g_haptic_window) g_haptic_window->haptic_pulse(duration_ms,strength);
}
void host_request_settings_menu() { g_settings_menu_requested.store(true); }

void web_notify_storage_write(WebStorageWrite kind,bool ok) {
    // Async proxy: only integers cross, so nothing can be freed before it runs.
    MAIN_THREAD_ASYNC_EM_ASM({ globalThis.GbrSaves?.onWrite($0,$1); },static_cast<int>(kind),ok?1:0);
}
}
