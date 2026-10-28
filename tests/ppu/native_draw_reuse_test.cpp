// Compare the shared preparation with the retained independent native
// compositor, including canonical state at partial and latched boundaries.
#include "gba_ppu.h"
#include "gba_bus.h"
#include "gba_simulation_state.h"
#include "foreign_presentation_internal.h"
#include <cstdlib>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>

bool authorize(int bg,int) { return bg==2 || bg==3; }
int bg_provider(int,int x,int,int* output) {*output=x+17;return 1;}
int obj_provider(int x,int* output) {*output=x+11;return 1;}
void policy_fallbacks() {
    for(unsigned policy=0;policy<5;++policy) {
        auto bus=std::make_unique<gba::GbaBus>();
        auto candidate=std::make_unique<gba::GbaPpu>();
        auto reference=std::make_unique<gba::GbaPpu>();
        bus->rtc().set_emulated_clock(0);
        candidate->set_internal_resolution(2,authorize);
        candidate->set_deferred_hd_rendering(true);
        std::array<uint16_t,240*160> foreign{};
        foreign.fill(0x7c00);
        if(policy==0)gba::g_ws_affine_filter_enabled=1;
        if(policy==1)gba::g_ws_bg_x_provider=bg_provider;
        if(policy==2)gba::g_ws_obj_x_provider=obj_provider;
        if(policy==3)gba::g_ws_native_view_left=0;
        if(policy==4)gba::foreign_presentation_internal::set_background(foreign.data());
        std::array<uint8_t,1024> io{};
        io[0x0c]=4;io[0x0d]=0x20;io[0x21]=1;io[0x27]=1;
        for(unsigned i=0;i<16384;++i)bus->vram_ptr()[0x4000+i]=(i%251)+1;
        for(unsigned i=0;i<1024;++i)bus->pal_ptr()[i]=i*13;
        for(unsigned i=0;i<128;++i)bus->oam_ptr()[i*8+1]=2;
        for(unsigned y=0;y<160;++y)for(auto* p:{candidate.get(),reference.get()})
            p->render_scanline(y,0x0401,io.data(),bus->vram_ptr(),bus->oam_ptr(),bus->pal_ptr());
        candidate->mark_framebuffer_latched();reference->mark_framebuffer_latched();
        if(candidate->high_resolution()->canonical_shared_lines()!=0 ||
           gba::save_device_state(*bus,*candidate)!=gba::save_device_state(*bus,*reference)) {
            std::fprintf(stderr,"presentation policy fallback differs: %u\n",policy);std::exit(1);
        }
        gba::g_ws_affine_filter_enabled=0;gba::g_ws_bg_x_provider=nullptr;
        gba::g_ws_obj_x_provider=nullptr;gba::g_ws_native_view_left=-1;
        gba::foreign_presentation_internal::clear();
    }
}
int main() {
    gba::GbaHighResolutionFrame untouched(2);
    untouched.configure(240,160); untouched.set_deferred_rendering(true);
    std::array<uint8_t,240*3> sentinel{}; sentinel.fill(0xa5);
    const auto before=sentinel;
    if(untouched.try_render_native_line(sentinel.data()) || sentinel!=before) return 1;
    policy_fallbacks();
    unsigned mismatched=0,affine_rows=0,shared_rows=0;
    for (unsigned test=0;test<32;++test) {
        auto bus=std::make_unique<gba::GbaBus>();
        auto ppu=std::make_unique<gba::GbaPpu>();
        auto reference=std::make_unique<gba::GbaPpu>();
        bus->rtc().set_emulated_clock(0);
        ppu->set_internal_resolution(2,authorize);
        ppu->set_deferred_hd_rendering(true);
        unsigned seed=0x7351a94fu+test*0x17f32u;
        auto next=[&] { seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed; };
        for(unsigned i=0;i<98304;++i)bus->vram_ptr()[i]=next();
        for(unsigned i=0;i<1024;++i)bus->pal_ptr()[i]=next();
        std::array<unsigned char,1024> io{};
        auto w16=[&](unsigned a,unsigned n) {io[a]=n;io[a+1]=n>>8;};
        auto w32=[&](unsigned a,unsigned n) {w16(a,n);w16(a+2,n>>16);};
        for(unsigned i=0;i<128;++i) {
            auto* obj=bus->oam_ptr()+i*8;
            const unsigned a0=i<24 ? (next()&0x23ff) : 0x0200;
            const unsigned a1=(next()&0xffff),a2=next()&0xffff;
            obj[0]=a0;obj[1]=a0>>8;obj[2]=a1;obj[3]=a1>>8;obj[4]=a2;obj[5]=a2>>8;
            obj[6]=next();obj[7]=next();
        }
        for(unsigned bg=0;bg<4;++bg) {
            w16(8+bg*2,next());w16(0x10+bg*4,next());w16(0x12+bg*4,next());
        }
        w16(0x40,next());w16(0x42,next());w16(0x44,next());w16(0x46,next());
        w16(0x48,next());w16(0x4a,next());
        w16(0x50,(test%4)*0x40|0x3f3f);w16(0x52,next());w16(0x54,next());
        w16(0x4c,test&4 ? next() : 0);
        const unsigned dispcnt=(test&1 ? 2u : 1u)|0x1f40u|((test&2) ? 0xe000u : 0);
        for(unsigned y=0;y<160;++y) {
            for(unsigned bg=2;bg<=3;++bg) {
                const unsigned a=0x20+(bg-2)*0x10;
                if(y==0 || y%7==0) {
                    w16(a,256+int(next()%1024)-512);w16(a+2,next()%513-256);
                    w16(a+4,next()%513-256);w16(a+6,256+int(next()%1024)-512);
                    w32(a+8,next()&0xfffffff);w32(a+12,next()&0xfffffff);
                    for(auto* output:{ppu.get(),reference.get()}) {
                        output->note_affine_reference_write(bg,false);output->note_affine_reference_write(bg,true);
                    }
                }
            }
            if(y%11==0)bus->vram_ptr()[next()%98304]^=0xa5;
            if(y%3==0)bus->pal_ptr()[next()%1024]^=0x3f;
            for(auto* output:{ppu.get(),reference.get()})
                output->render_scanline(y,dispcnt,io.data(),bus->vram_ptr(),bus->oam_ptr(),bus->pal_ptr());
            if(y%17==0 && gba::save_device_state(*bus,*reference)!=gba::save_device_state(*bus,*ppu)) {
                std::fprintf(stderr,"partial canonical state differs case=%u y=%u\n",test,y);return 1;
            }
        }
        ppu->mark_framebuffer_latched(); reference->mark_framebuffer_latched();
        if(gba::save_device_state(*bus,*reference)!=gba::save_device_state(*bus,*ppu)) {
            std::fprintf(stderr,"latched canonical state differs case=%u\n",test);return 1;
        }
        shared_rows+=ppu->high_resolution()->canonical_shared_lines();
        auto jobs=ppu->high_resolution()->draw_frame();
        if(!jobs) {std::fprintf(stderr,"missing complete jobs\n");return 2;}
        auto native_jobs=*jobs;
        for(auto& line:native_jobs.lines) {
            line.filter=false;
            if(line.kind==gba::GbaHighResolutionFrame::LineJob::AffineLine)++affine_rows;
        }
        const auto rendered=gba::GbaHighResolutionFrame::render_jobs(native_jobs);
        const auto* native=reference->latched_framebuffer();
        unsigned differences=0,first_x=0,first_y=0;
        for(unsigned y=0;y<160;++y)for(unsigned x=0;x<240;++x) {
            if(std::memcmp(native+(y*240+x)*3,rendered.data()+(y*2*480+x*2)*3,3)) {
                if(!differences) {first_x=x;first_y=y;}
                ++differences;
            }
        }
        mismatched+=differences!=0;
        std::printf("{\"case\":%u,\"mode\":%u,\"windows\":%s,\"different_pixels\":%u,\"first_x\":%u,\"first_y\":%u}\n",
            test,dispcnt&7,test&2 ? "true":"false",differences,first_x,first_y);
    }
    std::printf("{\"cases\":32,\"mismatched_cases\":%u,\"affine_job_rows\":%u,\"hypothesis\":\"unfiltered owned enhanced jobs can also produce canonical native sample positions under these inputs\"}\n",mismatched,affine_rows);
    if(mismatched || shared_rows!=5120) {std::fprintf(stderr,"mismatched=%u shared_rows=%u\n",mismatched,shared_rows);return 1;}
    std::printf("canonical shared preparation: independent original compositor and partial/full state parity, 5120 active rows passed\n");
    return 0;
}
