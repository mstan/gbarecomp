// A portable, deterministic input corpus for separate original/candidate
// compositor executables. Emit full canonical bytes at partial rows and the
// latched boundary; the host runner performs exact comparisons, not a second
// implementation of pixel ordering. This target is correctness-only.
#include "gba_ppu.h"
#include "gba_bus.h"
#include "gba_simulation_state.h"
#include "foreign_presentation_internal.h"
#include "snapshot.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <vector>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#if defined(GBARECOMP_ORDERED_SCANLINE_EXPERIMENT) && defined(GBARECOMP_ORDERED_DRAW_TEST_COVERAGE)
extern "C" uint64_t gba_ordered_draw_test_rows();
#if defined(GBARECOMP_PUBLISHED_TEXT_SPANS_API)
extern "C" uint64_t gba_ordered_wide_text_span_test_jobs();
#endif
#endif


namespace {
// The multiplayer canonical serializer deliberately rejects expanded views.
// Use the supported public user-state component serializers, configure host
// geometry explicitly on restore, and observe the full public RGB latch.
// Restored branches are compared against the independent original's restored
// branch: the user format intentionally does not persist authored margins or
// hidden affine accumulators. Built-game restore is a separate acceptance gate.
std::vector<uint8_t> save_bus_bytes(gba::GbaBus& bus) {
    gbarecomp::debug::SnapshotWriter w;
    bus.serialize(w);bus.io().serialize(w);bus.audio().serialize(w);bus.save().serialize(w);
    return w.take_buffer();
}
std::vector<uint8_t> save_view_bytes(gba::GbaBus& bus,gba::GbaPpu& ppu) {
    gbarecomp::debug::SnapshotWriter w;
    const auto b=save_bus_bytes(bus);w.bytes(b.data(),b.size());ppu.serialize(w);
    return w.take_buffer();
}
bool restore_view_bytes(gba::GbaBus& bus,gba::GbaPpu& ppu,const std::vector<uint8_t>& bytes) {
    gbarecomp::debug::SnapshotReader r(bytes.data(),bytes.size());
    bus.deserialize(r);bus.io().deserialize(r);bus.audio().deserialize(r);bus.save().deserialize(r);ppu.deserialize(r);
    return r.ok() && r.remaining()==0;
}
std::vector<uint8_t> observe_view(gba::GbaBus& bus,gba::GbaPpu& ppu) {
    ppu.mark_framebuffer_latched();
    gbarecomp::debug::SnapshotWriter w;
    const auto state=save_view_bytes(bus,ppu);
    w.u32(ppu.render_width());w.u32(ppu.render_height());w.u64(state.size());
    w.bytes(state.data(),state.size());w.bytes(ppu.latched_framebuffer(),ppu.render_bytes());
    return w.take_buffer();
}
unsigned current_profile = 0, current_case = 0;
int output_width = 288;
const uint8_t* row_io = nullptr;
std::vector<uint32_t> callback_trace;

int bg_x(int bg, int x, int, int* source) {
    if (bg >= 2) return 0;
    if (x < output_width / 3) { *source = (current_case & 1u) ? output_width - 1 - x : x; return 1; }
    if (x < output_width * 2 / 3) return -1;
    *source = 239; return 1;
}
int stateful_bg_x(int bg, int x, int y, int* source) {
    callback_trace.insert(callback_trace.end(), {0xb1000000u | unsigned(bg), unsigned(x), unsigned(y)});
    return bg_x(bg,x,y,source);
}
int bg_xy(int bg, int x, int y, int* sx, int* sy) {
    if ((x+29)%29 == 0) return -1;
    if (bg != 1 || x >= 0) return 0;
    *sx = 239; *sy = y-3; return 1;
}
int tilemap(int bg, int x, int y, uint16_t* entry) {
    if (current_profile == 3 || current_profile == 4)
        callback_trace.insert(callback_trace.end(), {0xb2000000u | unsigned(bg), unsigned(x), unsigned(y)});
    const unsigned off = 0x10+bg*4;
    const unsigned h = (row_io[off] | (row_io[off+1]<<8)) & 511;
    const unsigned v = (row_io[off+2] | (row_io[off+3]<<8)) & 511;
    const int64_t tx = int64_t(x)+h, ty = int64_t(y)+v;
    const uint64_t cx = uint64_t(tx)>>3, cy = uint64_t(ty)>>3;
    unsigned value = unsigned(cx*17+cy*31+bg*97+(x<0 ? 13:41));
    if (current_profile == 7) value ^= unsigned(x)&7u; // deliberately not tile invariant
    *entry = uint16_t((value&1023) | ((value&7)<<10));
    return value%7==0 ? 0 : value%4==0 ? 2 : 1;
}
int changed_tilemap(int bg,int x,int y,uint16_t* entry) { return tilemap(bg,x,y,entry); }
int stateful_bg_xy(int bg,int x,int y,int* sx,int* sy) {
    callback_trace.insert(callback_trace.end(),{0xb3000000u|unsigned(bg),unsigned(x),unsigned(y)});
    return bg_xy(bg,x,y,sx,sy);
}
int obj_y(int index,uint16_t,uint16_t,uint16_t,int* y) {
    if (index%5) return 0;
    *y=-4; return 1;
}
int ui_pixel(int x,int y,uint16_t* color) {
    if (y%11 || x<70 || x>180) return 0;
    *color=0x03e0;return 1;
}
const gba::WsMarginObjPixel* margin_obj(int y,int* left,int* width) {
    static std::array<gba::WsMarginObjPixel,1024> pixels;
    *left=-12;*width=output_width;
    for (int x=0;x<output_width;++x)
        pixels[x]={uint16_t((x+y)&31),uint8_t(x%4),uint8_t(x&127),bool(x&1)};
    return pixels.data();
}
bool hd_layer(int,int) { return true; }
int affine_filter(int,int) { return 1; }
#if defined(GBARECOMP_WIDE_BG_SAMPLER_API)
int bg_x_row(int bg,int,int width,int,gba::WsBgXSpan* spans,int capacity) {
    if (capacity<3) return -1;
    if (current_profile==6) {
        spans[0]={0,width,0,1,0};
        switch (current_case&7) {
        case 0:return 9;
        case 1:spans[0].begin=1;return 1;
        case 2:spans[0].end=width/2;spans[1]={width/2-1,width,0,1,0};return 2;
        case 3:spans[0].end=width-1;return 1;
        case 4:spans[0].step=8;return 1;
        case 5:spans[0].source_x=0x7fffffff;return 1;
        case 6:return -1;
        default:spans[0].action=2;return 1;
        }
    }
    if (bg>=2) {spans[0]={0,width,0,1,0};return 1;}
    spans[0]={0,width/3,(current_case & 1u) ? width-1 : 0,(current_case & 1u) ? -1 : 1,1};
    spans[1]={width/3,width*2/3,0,1,-1};
    spans[2]={width*2/3,width,239,0,1};
    return 3;
}
#if defined(GBARECOMP_ORDERED_SCANLINE_EXPERIMENT) && defined(GBARECOMP_ORDERED_DRAW_TEST_COVERAGE)
extern "C" uint64_t gba_ordered_wide_draw_test_rows();
#endif
#endif
}

int main() {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    unsigned long long emitted = 0;
    for (unsigned test = 0; test < 88; ++test) {
        current_case=test; current_profile=test/8; callback_trace.clear();
        const unsigned profile=current_profile;
#if defined(GBARECOMP_WIDE_BG_SAMPLER_API) && defined(GBARECOMP_ORDERED_SCANLINE_EXPERIMENT) && defined(GBARECOMP_ORDERED_DRAW_TEST_COVERAGE)
        const auto before=gba_ordered_wide_draw_test_rows();
#endif
        unsigned seed = 0x73bd48f1u + test * 0x3187au;
        auto next = [&] { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
        auto bus = std::make_unique<gba::GbaBus>();
        bus->matrix().configure(nullptr,0); // normal unloaded-cartridge reset state
        bus->rtc().set_emulated_clock(0);
        auto vram = std::span(bus->vram_ptr(), 98304);
        auto pal = std::span(bus->pal_ptr(), 1024);
        auto oam = std::span(bus->oam_ptr(), 1024);
        std::array<unsigned char, 1024> io{};
        auto w16 = [&](unsigned a, unsigned v) { io[a] = v; io[a+1] = v >> 8; };
        auto w32 = [&](unsigned a, unsigned v) { w16(a,v); w16(a+2,v>>16); };
        auto ow16 = [&](unsigned a, unsigned v) { oam[a] = v; oam[a+1] = v >> 8; };
        for (auto& value : vram) value = next();
        for (auto& value : pal) value = next();
        for (unsigned layer = 0; layer < 4; ++layer) {
            w16(8+layer*2,next());
            w16(0x10+layer*4,next()); w16(0x12+layer*4,next());
        }
        for (unsigned a = 0x40; a <= 0x54; a += 2) w16(a,next());
        // Explicitly include no effects, alpha, brightness, saturation and
        // target-bit combinations; independent random windows remain active.
        w16(0x50, (test % 4) * 0x40 | (next() & 0x3f3f));
        if (test % 8 == 0) w16(0x50, 0);
        if (test % 8 == 1) w16(0x50, 0x3f7f);
        w16(0x52, test % 9 == 0 ? 0x1000 : next());
        for (unsigned index = 0; index < 128; ++index) {
            unsigned a0 = (next() % 256) | ((next() % 3) << 14) |
                ((next() & 1) << 13) | ((next() % 4) << 10);
            if (index % 3 == 0) a0 |= 0x100;
            if (index % 7 == 0) a0 |= 0x200;
            if (test % 6 == 0 && index >= 8) a0 = 0x200;
            const unsigned a1 = (next() % 512) | (next() & 0xfe00);
            ow16(index*8,a0); ow16(index*8+2,a1); ow16(index*8+4,next());
        }
        for (unsigned group = 0; group < 32; ++group) {
            ow16(group*32+6, 256+int(next()%257)-128);
            ow16(group*32+14, int(next()%257)-128);
            ow16(group*32+22, int(next()%257)-128);
            ow16(group*32+30, 256+int(next()%257)-128);
        }
        const unsigned mode = profile==6 ? 0 : test%8;
        unsigned dispcnt = mode | 0x1f00 | ((test & 1) ? 0x40 : 0) |
            ((test % 3) ? (next() & 0xe000) : 0) | ((test & 4) ? 0x10 : 0);
        if (profile!=6 && test % 12 == 0) dispcnt |= 0x80;
        if (test >= 80) dispcnt &= (next() & 0x1f00) | 0xe0ff;
        auto ppu = std::make_unique<gba::GbaPpu>();
        const unsigned widths[]{288,382,510,256};
        output_width=widths[(test/2)%4];
        const unsigned left=(unsigned(output_width)-240)/2, right=unsigned(output_width)-240-left;
        ppu->set_view_margins(left,right,profile&1 ? 4:0,profile&1 ? 6:0);
        if (profile==10) ppu->set_internal_resolution(2,hd_layer);
        row_io=io.data();
        std::unique_ptr<gba::GbaPpu> restored;
        std::unique_ptr<gba::GbaBus> restored_bus;
        std::vector<uint16_t> foreign(240*160), overlay_pixels(64*64);
        std::vector<uint8_t> overlay_alpha(64*64);
        for (auto& value : foreign) value = next();
        for (auto& value : overlay_pixels) value = next();
        for (auto& value : overlay_alpha) value = next()%17;
        GbaForeignScreenOverlay overlay{GBA_FOREIGN_SCREEN_OVERLAY_ABI_VERSION,
            overlay_pixels.data(),overlay_alpha.data(),-7,43,64,64,64,0,0};
        GbaForeignObjFocusTransform focus{};
        focus.abi_version = GBA_FOREIGN_OBJ_FOCUS_ABI_VERSION;
        focus.source_link_feet_x = 70; focus.source_link_feet_y = 80;
        focus.destination_link_feet_x = 120; focus.destination_link_feet_y = 95;
        focus.source_radius_x = 100; focus.source_radius_y = 80;
        focus.source_obj_scale_q8_8 = 192;
        auto publish = [&] {
            gba::foreign_presentation_internal::clear();
            if (profile==5 && (test&1)) gba::foreign_presentation_internal::set_background(foreign.data());
            if (profile==5 && (test&2)) gba::foreign_presentation_internal::set_screen_overlay(&overlay);
            if (profile==5 && (test&4)) gba::foreign_presentation_internal::set_obj_focus(&focus);
            gba::g_ws_tilemap_provider=profile ? tilemap:nullptr;
            gba::g_ws_bg_x_provider=profile ? bg_x:nullptr;
            gba::g_ws_bg_x_provider_layers=0xf;
            gba::g_ws_bg_xy_provider=profile==8 ? bg_xy:nullptr;
            gba::g_ws_bg_xy_provider_layers=0xf;
            gba::g_ws_authored_margin_layers=profile&1;
            gba::g_ws_pillarbox=test%9==1;
            gba::g_ws_pillarbox_left=test%13==2;
            gba::g_ws_pillarbox_right=test%17==3;
            gba::g_ws_native_view_left=test%4==0 ? int(left/2):-1;
            gba::g_ws_obj_native_clip=test%5==0;
            gba::g_ws_obj_attr_y_provider=profile==9 ? obj_y:nullptr;
            gba::g_ws_obj_margin_provider=profile==9 ? margin_obj:nullptr;
            gba::g_ws_ui_pixel_provider=profile==9 ? ui_pixel:nullptr;
            gba::g_ws_affine_filter_enabled=test&1;
            gba::g_ws_affine_filter_provider=affine_filter;
#if defined(GBARECOMP_WIDE_BG_SAMPLER_API)
            gba::clear_read_only_wide_bg_providers();
            if (profile!=3) gba::publish_read_only_wide_bg_providers(
                profile==2 || profile==6 || profile==8 || profile==10 ? bg_x_row:nullptr,profile!=7);
#endif
            if (profile==3) gba::g_ws_bg_x_provider=stateful_bg_x;
            if (profile==4) {
                if (test%3==0) gba::g_ws_bg_x_provider=stateful_bg_x;
                else if (test%3==1) gba::g_ws_tilemap_provider=changed_tilemap;
                else gba::g_ws_bg_xy_provider=stateful_bg_xy;
            }
        };
        publish();
        for (unsigned y = 0; y < 160; ++y) {
            for (unsigned bg = 2; bg <= 3; ++bg) {
                const unsigned a = 0x20+(bg-2)*0x10;
                if (y == 0 || y % 7 == 0) {
                    w16(a,256+int(next()%1025)-512); w16(a+2,int(next()%513)-256);
                    w16(a+4,int(next()%513)-256); w16(a+6,256+int(next()%1025)-512);
                    w32(a+8,next() & 0xfffffff); w32(a+12,next() & 0xfffffff);
                    ppu->note_affine_reference_write(bg,false); ppu->note_affine_reference_write(bg,true);
                    if (restored) { restored->note_affine_reference_write(bg,false); restored->note_affine_reference_write(bg,true); }
                }
            }
            if (y % 3 == 0) {
                unsigned i = next()%pal.size(); pal[i] ^= 0x3f;
                if (restored_bus) restored_bus->pal_ptr()[i] = pal[i];
            }
            if (y % 11 == 0) {
                unsigned i = next()%vram.size(); vram[i] ^= 0xa5;
                if (restored_bus) restored_bus->vram_ptr()[i] = vram[i];
            }
            if (y % 13 == 0) {
                unsigned i = next()%oam.size(); oam[i] ^= 0x80;
                if (restored_bus) restored_bus->oam_ptr()[i] = oam[i];
            }
            if (y % 19 == 0) w16(0x40,next());
            ppu->render_scanline(y,dispcnt,io.data(),vram.data(),oam.data(),pal.data());
            if (restored) restored->render_scanline(y,dispcnt,io.data(),restored_bus->vram_ptr(),restored_bus->oam_ptr(),restored_bus->pal_ptr());
            if (y % 17 == 0 || y == 53) {
                const auto bytes = observe_view(*bus,*ppu);
                if (std::fwrite(bytes.data(),1,bytes.size(),stdout) != bytes.size()) return 2;
                emitted += bytes.size();
                if (restored) {
                    const auto other=observe_view(*restored_bus,*restored);
                    if (std::fwrite(other.data(),1,other.size(),stdout)!=other.size()) return 2;
                    emitted+=other.size();
                }
                if (y == 53) {
                    restored = std::make_unique<gba::GbaPpu>();
                    restored_bus = std::make_unique<gba::GbaBus>();
                    restored_bus->matrix().configure(nullptr,0);
                    restored_bus->rtc().set_emulated_clock(0);
                    restored->set_view_margins(left,right,profile&1 ? 4:0,profile&1 ? 6:0);
                    if (profile==10) restored->set_internal_resolution(2,hd_layer);
                    const auto state=save_view_bytes(*bus,*ppu);
                    if (!restore_view_bytes(*restored_bus,*restored,state)) return 3;
                    if (save_bus_bytes(*restored_bus)!=save_bus_bytes(*bus)) return 8;
                    publish(); // host-owned presentation is deliberately not saved
                }
            }
        }
        ppu->mark_framebuffer_latched(); restored->mark_framebuffer_latched();
        const auto bytes = observe_view(*bus,*ppu);
        const auto other=observe_view(*restored_bus,*restored);
        if (std::fwrite(other.data(),1,other.size(),stdout)!=other.size()) return 2;
        emitted+=other.size();
        if (std::fwrite(bytes.data(),1,bytes.size(),stdout) != bytes.size()) return 2;
        emitted += bytes.size();
        if (profile==10) {
            const auto* hd=ppu->high_resolution();
            const auto* pixels=ppu->high_resolution_framebuffer();
            if (!hd || !pixels) return 6;
            const size_t count=size_t(hd->width())*hd->height()*3;
            if (std::fwrite(pixels,1,count,stdout)!=count) return 2;
            emitted+=count;
        }
        const uint64_t trace_size=callback_trace.size();
        if (std::fwrite(&trace_size,1,sizeof(trace_size),stdout)!=sizeof(trace_size)) return 2;
        if (trace_size && std::fwrite(callback_trace.data(),sizeof(uint32_t),trace_size,stdout)!=trace_size) return 2;
        emitted+=sizeof(trace_size)+trace_size*sizeof(uint32_t);
#if defined(GBARECOMP_WIDE_BG_SAMPLER_API) && defined(GBARECOMP_ORDERED_SCANLINE_EXPERIMENT) && defined(GBARECOMP_ORDERED_DRAW_TEST_COVERAGE)
        const auto selected=gba_ordered_wide_draw_test_rows()-before;
        // Internal scale alone is not path coverage. Only accepted affine HD
        // jobs bypass CPU composition; ordinary/bitmap rows use CPU upscale.
        const uint64_t hd_rows=profile==10 ? ppu->high_resolution()->affine_lines()+restored->high_resolution()->affine_lines():0;
        const bool expected=profile!=3 && profile!=4 && (profile!=5 || !(test&7)) && !hd_rows;
        if (profile==10 && selected+hd_rows!=266) return 9;
        if (bool(selected)!=expected) {std::fprintf(stderr,"wide selection mismatch case=%u selected=%llu expected=%d\n",test,(unsigned long long)selected,expected);return 7;}
#endif
        std::fprintf(stderr,"wide-case=%u emitted=%llu\n",test,emitted);
        gba::foreign_presentation_internal::clear();
    }
    std::fprintf(stderr,"88 expanded cases,all modes,scalar/row/tile providers,8 malformed spans,uncertified/changed/foreign fallbacks,full callback trace,HD2x,public user-state restored branches; %llu exact-oracle bytes\n",emitted);
#if defined(GBARECOMP_WIDE_BG_SAMPLER_API) && defined(GBARECOMP_ORDERED_SCANLINE_EXPERIMENT) && defined(GBARECOMP_ORDERED_DRAW_TEST_COVERAGE)
    std::fprintf(stderr,"actual ordered expanded rows=%llu\n",(unsigned long long)gba_ordered_wide_draw_test_rows());
#if defined(GBARECOMP_PUBLISHED_TEXT_SPANS_API) && defined(GBARECOMP_SCANLINE_HLE)
    if (!gba_ordered_wide_text_span_test_jobs()) return 11;
    std::fprintf(stderr,"actual published text span jobs=%llu\n",(unsigned long long)gba_ordered_wide_text_span_test_jobs());
#endif
#endif
}
