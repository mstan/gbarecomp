#include "gba_bus.h"
#include "gba_draw_memory.h"
#include "gba_ppu.h"
#include "gba_simulation_state.h"
#include "snapshot.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::exit(1); } } while (0)

void check_native_lifecycle() {
    auto bus=std::make_unique<gba::GbaBus>();
    auto reference=std::make_unique<gba::GbaPpu>();
    auto owned=std::make_unique<gba::GbaPpu>();
    bus->rtc().set_emulated_clock(0);
    bus->enable_draw_memory();owned->set_native_draw_enabled(true);
    for(unsigned frame=0;frame<4;++frame) {
        for(unsigned y=0;y<160;++y) {
            const unsigned offset=(y*240+(frame*379)%240)*2;
            bus->write32(0x06000000+offset,0x01234567u*(y+frame+1));
            bus->write16(0x05000000,uint16_t(frame*0x421+y));
            auto* memory=frame==2 && y>=45 ? nullptr : bus->draw_memory();
            reference->render_scanline(y,0x403,bus->io().raw(),bus->vram_read_view().data(),bus->oam_ptr(),bus->pal_ptr());
            owned->render_scanline(y,0x403,bus->io().raw(),bus->vram_read_view().data(),bus->oam_ptr(),bus->pal_ptr(),memory);
            if(frame==1 && y==73) {
                const auto original=gba::save_device_state(*bus,*reference);
                const auto captured=gba::save_device_state(*bus,*owned);
                CHECK(original==captured); // Includes previous latch and partial work image.
                CHECK(gba::load_device_state(*bus,*owned,captured));
                CHECK(!owned->native_draw_frame());
            }
        }
        reference->mark_framebuffer_latched();owned->mark_framebuffer_latched();
        CHECK(bool(owned->native_draw_frame())==(frame==0 || frame==3));
        CHECK(gba::save_device_state(*bus,*reference)==gba::save_device_state(*bus,*owned));
        gbarecomp::debug::SnapshotWriter a,b;
        reference->serialize(a);owned->serialize(b);CHECK(a.buffer()==b.buffer());
        CHECK(std::equal(reference->latched_framebuffer(),reference->latched_framebuffer()+gba::GbaPpu::kFramebufferBytes,owned->latched_framebuffer()));
    }
    CHECK(owned->native_draw_frames()==2);
    unsigned rejected_reads=0;
    owned->set_native_draw_enabled(true,[&](const auto&,auto&) {++rejected_reads;return false;});
    for(unsigned y=0;y<160;++y) {
        reference->render_scanline(y,0x403,bus->io().raw(),bus->vram_read_view().data(),bus->oam_ptr(),bus->pal_ptr());
        owned->render_scanline(y,0x403,bus->io().raw(),bus->vram_read_view().data(),bus->oam_ptr(),bus->pal_ptr(),bus->draw_memory());
    }
    reference->mark_framebuffer_latched();owned->mark_framebuffer_latched();
    owned->set_native_draw_enabled(false);
    CHECK(!owned->native_draw_frame());
    CHECK(rejected_reads==1);
    CHECK(gba::save_device_state(*bus,*reference)==gba::save_device_state(*bus,*owned));
    bus->disable_draw_memory();CHECK(!bus->draw_memory_supported());
    CHECK(bus->enable_draw_memory());
    CHECK(bus->draw_memory()->publish(bus->vram_read_view()).reset);
    std::puts("native draw lifecycle: exact full and partial checkpoints, own-build restore, mid-frame admission revocation and CPU continuation");
}

int main() {
    check_native_lifecycle();
    auto bus = std::make_unique<gba::GbaBus>();
    auto ppu = std::make_unique<gba::GbaPpu>();
    bus->rtc().set_emulated_clock(0); // Canonical device snapshots require the session RTC timeline.
    CHECK(!bus->draw_memory_supported());
    bus->enable_draw_memory(); CHECK(bus->draw_memory_supported());
    std::vector<std::uint8_t> replica(gba::GbaDrawMemory::kVramBytes);
    std::uint64_t sequence = 0;
    auto publish = [&] {
        auto* journal = bus->draw_memory(); CHECK(journal);
        auto delta = journal->publish(bus->vram_read_view());
        CHECK(gba::GbaDrawMemory::apply(delta,replica,sequence));
        CHECK(std::equal(replica.begin(),replica.end(),bus->vram_read_view().begin()));
        return delta;
    };
    CHECK(publish().reset);
    bus->write8(0x0600001f,0x91);
    bus->write16(0x0600003f,0x1234); // A store straddling dirty blocks.
    bus->write32(0x06017ffc,0x87654321);
    bus->write32(0x0601fffc,0x10203040); // Existing mirrored VRAM address contract.
    CHECK(!publish().ranges.empty());
    const auto before = std::vector(bus->vram_read_view().begin(),bus->vram_read_view().end());
    CHECK(!bus->try_write_draw_vram(replica.size(),std::span(before).first(1)));
    CHECK(!bus->try_write_draw_vram(std::size_t(-1),std::span(before).first(1)));
    CHECK(std::equal(before.begin(),before.end(),bus->vram_read_view().begin()));
    CHECK(bus->try_write_draw_vram(12,std::span(before).subspan(31,217)));
    CHECK(!publish().ranges.empty());
    auto expected = std::vector(bus->vram_read_view().begin(),bus->vram_read_view().end());
    std::memmove(expected.data()+16,expected.data()+12,217);
    CHECK(bus->try_write_draw_vram(16,bus->vram_read_view().subspan(12,217)));
    publish(); CHECK(replica==expected);

    struct Observer : gba::BusWriteObserver {
        bool suppress=false; unsigned calls=0;
        bool on_bus_write(gba::BusWriteRegion region,uint32_t,uint32_t,uint8_t,uint32_t,uint32_t) override {
            CHECK(region==gba::BusWriteRegion::Vram); ++calls; return suppress;
        }
    } observer;
    bus->set_write_observer(&observer); CHECK(!bus->draw_memory());
    CHECK(!bus->try_write_draw_vram(0,std::span(before).first(1)));
    bus->write8(0x06000123,0x5a); bus->write16(0x06000234,0x5678);
    bus->write32(0x06000345,0xabcdef01); CHECK(observer.calls==3);
    observer.suppress=true;
    bus->write8(0x06000123,0); bus->write16(0x06000234,0); bus->write32(0x06000345,0);
    CHECK(observer.calls==6);
    bus->set_write_observer(nullptr);
    CHECK(!publish().ranges.empty());
    CHECK(replica[0x123]==0x5a && replica[0x234]==0x78 && replica[0x345]==1);

    gbarecomp::debug::SnapshotWriter writer; bus->serialize(writer);
    bus->write32(0x06000000,0x13579bdf); publish();
    gbarecomp::debug::SnapshotReader reader(writer.buffer().data(),writer.buffer().size());
    bus->deserialize(reader); CHECK(reader.ok()); CHECK(publish().reset);
    const auto canonical=gba::save_device_state(*bus,*ppu);
    bus->write32(0x06000000,0x2468ace0); publish();
    CHECK(gba::load_device_state(*bus,*ppu,canonical)); CHECK(publish().reset);

    auto* escaped = bus->vram_ptr(); CHECK(!bus->draw_memory_supported());
    escaped[0]=0xcc; bus->enable_draw_memory(); CHECK(!bus->draw_memory());
    auto early_alias = std::make_unique<gba::GbaBus>();
    auto* retained=early_alias->vram_ptr(); early_alias->enable_draw_memory();
    retained[0]=0xdd; CHECK(!early_alias->draw_memory_supported());
    std::puts("draw memory real bus: stores/mirrors/bulk overlap/observer/suppression/native and canonical restore/escaped aliases passed");
}
