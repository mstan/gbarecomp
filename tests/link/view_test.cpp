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
void raster() {
    auto bus=std::make_unique<gba::GbaBus>();
    bus->rtc().set_emulated_clock(0);
    auto native=std::make_unique<gba::GbaPpu>();
    auto observed=std::make_unique<gba::GbaPpu>();
    auto reference=std::make_unique<gba::GbaPpu>();
    GbaNetplayPresentation mirror({true,true});
    observed->set_presentation_observer(&mirror);
    // Affine BG2, a wrapped colorful tilemap. Writes below emulate changing
    // road matrices and hidden-reference reloads on individual HBlanks.
    std::array<std::uint8_t,0x400> io{};
    auto w16=[&](unsigned a,unsigned n) { io[a]=n; io[a+1]=n>>8; };
    auto w32=[&](unsigned a,unsigned n) { w16(a,n); w16(a+2,n>>16); };
    w16(0x0c,0x2080); // 256-color affine BG, wrap
    for (unsigned i=0;i<96*1024;++i) bus->vram_ptr()[i]=(i*17+i/19)&255;
    for (unsigned i=0;i<512;++i) bus->pal_ptr()[i]=(i*29)&255;
    for (const unsigned width : {284u,373u,569u,576u,240u,317u}) {
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
        CHECK(gba::save_device_state(*bus,*native)==gba::save_device_state(*bus,*observed));
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
    CHECK(gba_netplay_view_width(GbaNetplayView::Adaptive,9999,1)==576);
    CHECK(gba_netplay_view_width(GbaNetplayView::Adaptive,100,500)==240);
    GbaNetplayLaunch launch; launch.view_policy=policy;
    std::vector<std::string> args={"game","--netplay-view","adaptive","--launcher"};
    parse_gba_netplay_arguments(args,launch);
    CHECK(!launch.enabled && launch.view==GbaNetplayView::Adaptive && launch.view_explicit);
    CHECK((args==std::vector<std::string>{"game","--launcher"}));
    raster(); restores();
    std::puts("netplay presentation: raster parity, canonical state, restore and capability gates passed");
}
