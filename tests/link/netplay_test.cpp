#include "multiplayer_netplay.h"
#include "netplay_fixture.h"
#include <cstdio>
#include <cstdlib>

#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); std::exit(1); } } while (0)
namespace {
using link_fixture::create;
void restore_corrected_session() {
    using namespace gbarecomp;
    auto simulation=create(); GbaNetplayHost h(*simulation); const auto api=h.callbacks();
    const auto saved=api.snap_save(api.ctx,0);
    if (!saved) std::fprintf(stderr,"snapshot: %s\n",h.error().c_str());
    CHECK(saved);
    RNetRbFrame rows[2]{};
    for (auto& row:rows) row.is_valid=1;
    rows[0].buttons=1; rows[1].buttons=2;
    api.publish(api.ctx,0,rows,2,0); CHECK(h.run_published_tick());
    CHECK(simulation->machine(0).bus.read32(0x02000000)==0x22241112);
    CHECK(api.snap_save(api.ctx,1));
    std::int16_t samples[32];
    CHECK(simulation->machine(0).bus.audio().drain_samples(samples,32)>0);
    api.resim_begin(api.ctx);
    CHECK(api.snap_load(api.ctx,0));
    api.snap_drop_after(api.ctx,0); CHECK(!api.snap_has(api.ctx,1));
    rows[0].buttons=8; rows[1].buttons=16;
    api.publish(api.ctx,0,rows,2,1); CHECK(api.run_tick(api.ctx,0));
    api.resim_end(api.ctx);
    CHECK(!h.replaying() && h.next_tick()==1 && h.error().empty());
    CHECK(simulation->machine(0).bus.audio().drain_samples(samples,32)==0);
    CHECK(simulation->machine(0).bus.read32(0x02000000)==0x22321119);
    CHECK(simulation->machine(0).bus.save().sram_read(0)==8);
    CHECK(simulation->machine(1).bus.save().sram_read(0)==16);
    auto expected=create(); const std::uint16_t corrected[]={8,16}; expected->run_frame(corrected);
    CHECK(simulation->save_state()==expected->save_state());
    RNetRbDigestParts parts{}; api.digest_parts(api.ctx,&parts);
    CHECK(parts.master==expected->state_hash());
    CHECK(parts.part[0]!=parts.part[1]);
}
void delay_publication() {
    auto simulation=create(); gbarecomp::GbaNetplayHost h(*simulation); auto api=h.delay_callbacks();
    h.sample_local=[](std::uint32_t tick) { return std::uint16_t(tick|0xf400); };
    RNetInputSample sample{}; api.sample_local(3,&sample,api.ctx);
    CHECK(sample.size==2 && sample.valid && sample.bytes[0]==3 && sample.bytes[1]==0);
    RNetInputSample samples[]={sample,sample};
    api.publish(0,samples,2,api.ctx); CHECK(h.run_published_tick());
    CHECK(h.next_tick()==1);
    CHECK(simulation->machine(0).bus.read32(0x02000000)==0x22251114);
    samples[0].size=1; api.publish(1,samples,2,api.ctx);
    CHECK(h.return_to_lobby_requested() && !h.run_published_tick());
    CHECK(h.next_tick()==1);
}
void confirmed_checkpoint() {
    using namespace gbarecomp;
    auto simulation=create(true); GbaNetplayHost h(*simulation); const auto api=h.callbacks();
    const auto cold=simulation->save_state();
    CHECK(h.confirmed_checkpoint().next_tick==0 && h.confirmed_checkpoint().state==cold);
    CHECK(!h.retain_confirmed(UINT32_MAX));
    RNetRbFrame rows[2]{}; for (auto& row:rows) row.is_valid=1;
    auto run=[&](unsigned tick,unsigned input) {
        CHECK(api.snap_save(api.ctx,tick));
        rows[0].buttons=input; rows[1].buttons=input+1;
        api.publish(api.ctx,tick,rows,2,0); CHECK(h.run_published_tick());
    };
    run(0,1);
    const auto after_zero=simulation->save_state();
    run(1,5); run(2,9);
    CHECK(h.retain_confirmed(0));
    CHECK(h.confirmed_checkpoint().next_tick==1);
    CHECK(h.confirmed_checkpoint().state==after_zero);
    CHECK(!h.retain_confirmed(UINT32_MAX));
    CHECK(!h.retain_confirmed(0));
    // Correct a speculative tail. Confirmed storage must neither follow that
    // tail nor disappear when the driver's snapshot ring is invalidated.
    api.resim_begin(api.ctx); CHECK(api.snap_load(api.ctx,1));
    api.snap_drop_after(api.ctx,1);
    rows[0].buttons=16; rows[1].buttons=32;
    api.publish(api.ctx,1,rows,2,1); CHECK(api.run_tick(api.ctx,1));
    CHECK(!h.retain_confirmed(1));
    api.resim_end(api.ctx);
    CHECK(h.confirmed_checkpoint().state==after_zero);
    CHECK(h.retain_confirmed(1)); // exact live boundary, even without a ring entry
    const auto corrected=simulation->save_state();
    CHECK(h.confirmed_checkpoint().next_tick==2);
    CHECK(h.confirmed_checkpoint().state==corrected);
    run(2,3);
    api.snap_drop_after(api.ctx,0);
    CHECK(h.restore_confirmed_for_restart());
    CHECK(h.next_tick()==0 && h.confirmed_checkpoint().next_tick==0);
    CHECK(simulation->save_state()==corrected);
    CHECK(!api.snap_has(api.ctx,0) && !api.snap_has(api.ctx,1));
    CHECK(simulation->machine(0).bus.save().sram_read(0)==16);
    CHECK(simulation->machine(1).bus.save().sram_read(0)==32);
    // An impossible watermark is an error, never permission to save a tail.
    CHECK(!h.retain_confirmed(100) && h.return_to_lobby_requested());
    CHECK(h.confirmed_checkpoint().state==corrected);
}
}
int main() { restore_corrected_session(); delay_publication(); confirmed_checkpoint(); std::puts("whole-session netplay host tests passed"); }
