#include "multiplayer_launch.h"
#include "multiplayer_session.h"
#include "gba_simulation_state.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::exit(1); } } while (0)
using namespace gbarecomp;
namespace {
int anchor_left = -1;
void anchor_frame(const ExtendedViewFrameInfo*) { gba::g_ws_native_view_left = anchor_left; }
void anchored_raster() {
    auto bus=std::make_unique<gba::GbaBus>();
    bus->rtc().set_emulated_clock(0);
    auto source=std::make_unique<gba::GbaPpu>();
    auto canonical=std::make_unique<gba::GbaPpu>();
    auto reference=std::make_unique<gba::GbaPpu>();
    GbaNetplayPresentation mirror({true,false,nullptr,anchor_frame});
    mirror.request_width(284);
    source->set_rasterization_enabled(false);
    canonical->set_rasterization_enabled(false);
    source->set_presentation_observer(&mirror);
    std::array<std::uint8_t,0x400> registers{};
    auto* io=registers.data();
    const auto w16=[](std::uint8_t* p,unsigned n) { p[0]=n; p[1]=n>>8; };
    // Colorful text BG, OBJ and a window edge must move together. The
    // reference uses real asymmetric PPU margins, with no policy override.
    w16(io+0x08,0x0100); w16(io+0x40,0x1060); w16(io+0x44,0x00a0);
    w16(io+0x48,0x003f); w16(io+0x4a,0x0001);
    for(unsigned i=0;i<96*1024;++i) bus->vram_ptr()[i]=(i*17+i/19)&255;
    for(unsigned i=0;i<1024;++i) bus->pal_ptr()[i]=(i*29)&255;
    for(unsigned i=0;i<128;++i) w16(bus->oam_ptr()+8*i,0x0200);
    w16(bus->oam_ptr(),20); w16(bus->oam_ptr()+2,40); w16(bus->oam_ptr()+4,0);
    for(const int requested : {-1,0,11,22,44,1000}) {
        anchor_left=requested;
        const unsigned left=requested<0 ? 22 : std::min(requested,44);
        reference->set_view_margins(left,44-left,0,0);
        for(unsigned y=0;y<160;++y) {
            gba::g_ws_native_view_left=13; // an unrelated presentation's policy
            source->render_scanline(y,0x3100,io,bus->vram_ptr(),bus->oam_ptr(),bus->pal_ptr());
            canonical->render_scanline(y,0x3100,io,bus->vram_ptr(),bus->oam_ptr(),bus->pal_ptr());
            CHECK(gba::g_ws_native_view_left==13);
            gba::g_ws_native_view_left=-1;
            reference->render_scanline(y,0x3100,io,bus->vram_ptr(),bus->oam_ptr(),bus->pal_ptr());
        }
        source->mark_framebuffer_latched(); reference->mark_framebuffer_latched();
        canonical->mark_framebuffer_latched();
        CHECK(mirror.pixels());
        CHECK(std::memcmp(mirror.pixels(),reference->latched_framebuffer(),284*160*3)==0);
        CHECK(gba::save_device_state(*bus,*source)==gba::save_device_state(*bus,*canonical));
        mirror.restored();
    }
    source->set_presentation_observer(nullptr);
    gba::g_ws_native_view_left=-1;
}
void raster() {
    auto bus=std::make_unique<gba::GbaBus>();
    bus->rtc().set_emulated_clock(0);
    auto native=std::make_unique<gba::GbaPpu>();
    auto observed=std::make_unique<gba::GbaPpu>();
    auto reference=std::make_unique<gba::GbaPpu>();
    // A game that opts into the engine's full capacity (50:9 and beyond).
    GbaNetplayPresentation mirror({true,true,nullptr,nullptr,nullptr,gba::GbaPpu::kMaxRenderWidth});
    observed->set_presentation_observer(&mirror);
    observed->set_rasterization_enabled(false);
    // Affine BG2, a wrapped colorful tilemap. Writes below emulate changing
    // road matrices and hidden-reference reloads on individual HBlanks.
    std::array<std::uint8_t,0x400> io{};
    auto w16=[&](unsigned a,unsigned n) { io[a]=n; io[a+1]=n>>8; };
    auto w32=[&](unsigned a,unsigned n) { w16(a,n); w16(a+2,n>>16); };
    w16(0x0c,0x2080); // 256-color affine BG, wrap
    for (unsigned i=0;i<96*1024;++i) bus->vram_ptr()[i]=(i*17+i/19)&255;
    for (unsigned i=0;i<512;++i) bus->pal_ptr()[i]=(i*29)&255;
    for (const unsigned width : {284u,373u,569u,576u,889u,gba::GbaPpu::kMaxRenderWidth,240u,317u}) {
        mirror.request_width(width);
        const auto extra=width-240;
        reference->set_view_margins(extra/2,extra-extra/2,0,0);
        for (unsigned y=0;y<160;++y) {
            w16(0x20,0x100+y); w16(0x22,3); w16(0x24,7); w16(0x26,0x100-y);
            if (y%7==0) {
                w32(0x28,0x2300+y*511); w32(0x2c,0x4200-y*99);
                for (auto* p:{native.get(),observed.get(),reference.get()}) {
                    p->note_affine_reference_write(2,false); p->note_affine_reference_write(2,true);
                }
            }
            for (auto* p:{native.get(),observed.get(),reference.get()})
                p->render_scanline(y,0x0402,io.data(),bus->vram_ptr(),bus->oam_ptr(),bus->pal_ptr());
            if (y==80) mirror.request_width(300); // deferred, no mixed-stride frame
        }
        for (auto* p:{native.get(),observed.get(),reference.get()}) p->mark_framebuffer_latched();
        CHECK(mirror.pixels() && mirror.width()==width);
        CHECK(std::memcmp(mirror.pixels(),reference->latched_framebuffer(),width*160*3)==0);
        // Pixel production changes no hardware timing or affine registers.
        native->set_rasterization_enabled(false);
        CHECK(gba::save_device_state(*bus,*native)==gba::save_device_state(*bus,*observed));
        native->set_rasterization_enabled(true);
    }
    // External provider state must not leak into/out of the mirror.
    gba::g_ws_pillarbox=1;
    mirror.restored();
    CHECK(!mirror.pixels());
    observed->render_scanline(50,0x0402,io.data(),bus->vram_ptr(),bus->oam_ptr(),bus->pal_ptr());
    observed->mark_framebuffer_latched();
    CHECK(!mirror.pixels() && gba::g_ws_pillarbox==1);
    gba::g_ws_pillarbox=0;
}
void restores() {
    GbaSessionConfig c;
    c.machines={{0,"fixture",std::string(40,'a')},{1,"fixture",std::string(40,'a')}};
    c.links={{GbaLinkMedium::Cable,{0,1}}}; c.input_machines={0,1};
    GbaMultiplayerSession session(c);
    session.machine(1).immediate_override_is_observer=true;
    session.machine(1).ppu.set_rasterization_enabled(false);
    GbaNetplayPresentation mirror({true,true});
    auto& ppu=session.machine(1).ppu;
    ppu.set_presentation_observer(&mirror);
    const auto snapshot=session.save_state();
    std::string error;
    auto broken=snapshot; broken.pop_back();
    CHECK(!session.load_state(broken,&error));
    CHECK(&ppu==&session.machine(1).ppu);
    CHECK(session.machine(1).ppu.presentation_observer()==&mirror);
    CHECK(session.load_state(snapshot,&error));
    CHECK(session.machine(1).immediate_override_is_observer);
    CHECK(!session.machine(1).ppu.rasterization_enabled());
    CHECK(session.machine(0).ppu.rasterization_enabled());
    CHECK(!session.machine(0).immediate_override_is_observer);
    CHECK(session.machine(1).ppu.presentation_observer()==&mirror);
    CHECK(!session.machine(0).ppu.presentation_observer());
    CHECK(session.save_state()==snapshot);
    session.machine(1).ppu.set_presentation_observer(nullptr);
}
}
int main() {
    GbaNetplayViewPolicy policy{true,false};
    bool rejected=false;
    try { validate_gba_netplay_view(GbaNetplayView::Adaptive,policy); }
    catch (const std::invalid_argument&) { rejected=true; }
    CHECK(rejected);
    validate_gba_netplay_view(GbaNetplayView::Wide32x9,policy);
    policy.adaptive_supported=true;
    validate_gba_netplay_view(GbaNetplayView::Adaptive,policy);
    CHECK(gba_netplay_view_width(GbaNetplayView::Adaptive,1920,1080)==284);
    CHECK(gba_netplay_view_width(GbaNetplayView::Adaptive,3440,1440)==382);
    CHECK(gba_netplay_view_width(GbaNetplayView::Adaptive,5120,1440)==569);
    // Default game ceiling: raising engine capacity never widens a game
    // that did not opt in.
    CHECK(gba_netplay_view_width(GbaNetplayView::Adaptive,9999,1)==576);
    CHECK(gba_netplay_view_width(GbaNetplayView::Adaptive,100,500)==240);
    CHECK(gba_netplay_view_width(GbaNetplayView::Adaptive,5000,900)==576);
    // Opted-in ceilings, bounded by engine capacity.
    CHECK(gba::GbaPpu::kMaxRenderWidth==896);
    CHECK(gba_netplay_view_width(GbaNetplayView::Adaptive,5000,900,896)==889);
    CHECK(gba_netplay_view_width(GbaNetplayView::Adaptive,9999,1,896)==896);
    CHECK(gba_netplay_view_width(GbaNetplayView::Adaptive,9999,1,100000)==gba::GbaPpu::kMaxRenderWidth);
    CHECK(gba_netplay_view_width(GbaNetplayView::Adaptive,1920,1080,480)==284);
    CHECK(gba_netplay_view_width(GbaNetplayView::Adaptive,5120,1440,480)==480);
    CHECK(gba_netplay_view_width(GbaNetplayView::Adaptive,0,0,260)==260);
    CHECK(gba_netplay_view_width(GbaNetplayView::Wide32x9,9999,1,300)==569);
    const auto rejects=[](GbaNetplayView view,const GbaNetplayViewPolicy& p) {
        try { validate_gba_netplay_view(view,p); } catch (const std::invalid_argument&) { return true; }
        return false;
    };
    GbaNetplayViewPolicy narrow=policy; narrow.max_width=400;
    CHECK((gba_netplay_available_views(narrow)==std::vector{GbaNetplayView::Native,GbaNetplayView::Wide16x9,GbaNetplayView::Wide21x9,GbaNetplayView::Adaptive}));
    CHECK(!rejects(GbaNetplayView::Wide21x9,narrow) && rejects(GbaNetplayView::Wide32x9,narrow));
    CHECK(!rejects(GbaNetplayView::Adaptive,narrow));
    narrow.max_width=284; narrow.adaptive_supported=false;
    CHECK((gba_netplay_available_views(narrow)==std::vector{GbaNetplayView::Native,GbaNetplayView::Wide16x9}));
    narrow.max_width=239; CHECK(rejects(GbaNetplayView::Native,narrow));
    narrow.max_width=gba::GbaPpu::kMaxRenderWidth+1; CHECK(rejects(GbaNetplayView::Native,narrow));
    {
        GbaNetplayPresentation fixed({true,true});
        bool too_wide=false;
        try { fixed.request_width(577); } catch (const std::invalid_argument&) { too_wide=true; }
        CHECK(too_wide);
        fixed.request_width(576);
        GbaNetplayPresentation wide({true,true,nullptr,nullptr,nullptr,896});
        wide.request_width(889); wide.request_width(896);
        too_wide=false;
        try { wide.request_width(897); } catch (const std::invalid_argument&) { too_wide=true; }
        CHECK(too_wide);
    }
    GbaNetplayLaunch launch; launch.view_policy=policy;
    std::vector<std::string> args={"game","--netplay-view","adaptive","--launcher"};
    parse_gba_netplay_arguments(args,launch);
    CHECK(!launch.enabled && launch.view==GbaNetplayView::Adaptive && launch.view_explicit);
    CHECK((args==std::vector<std::string>{"game","--launcher"}));
    raster(); anchored_raster(); restores();
    std::puts("netplay presentation: raster parity, canonical state, restore and capability gates passed");
}
