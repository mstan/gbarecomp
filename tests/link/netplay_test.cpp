#include "multiplayer_netplay.h"
#include "multiplayer_checkpoint.h"
#include "multiplayer_pacing.h"
#include "netplay_fixture.h"
#include <cstdio>
#include <cstdlib>

#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); std::exit(1); } } while (0)
namespace {
using link_fixture::create;
void admission_pacing() {
    using namespace std::chrono;
    using P=gbarecomp::GbaNetplayPacer;
    P pacer;
    const P::Time start(seconds(100));
    CHECK(pacer.ready(start));
    pacer.forwarded(start);
    // A ten-ms frame must leave only the remaining fraction to wait.
    CHECK(!pacer.ready(start+milliseconds(10)));
    CHECK(pacer.ready(start+P::frame_period));
    pacer.forwarded(start+P::frame_period+milliseconds(3));
    // Ordinary scheduling/network jitter must not accumulate into slowdown.
    CHECK(pacer.ready(start+P::frame_period*2));
    // A long network stall does not accumulate a burst of old frame slots.
    const auto recovered=start+seconds(6);
    pacer.forwarded(recovered);
    CHECK(!pacer.ready(recovered+milliseconds(1)));
    CHECK(pacer.ready(recovered+P::frame_period));
}
void confirmed_session_control() {
    using namespace gbarecomp;
    auto simulation=create(true); GbaNetplayHost host(*simulation); auto api=host.callbacks();
    RNetRbFrame rows[2]{}; for (auto& row:rows) row.is_valid=1;
    CHECK(api.snap_save(api.ctx,0));
    rows[1].buttons=GbaNetplayHost::kCheckpointRequest;
    api.publish(api.ctx,0,rows,2,0); CHECK(host.run_published_tick());
    const auto requested=simulation->save_state();
    const auto requested_digest=api.digest_master(api.ctx);
    CHECK(requested_digest!=simulation->state_hash());
    CHECK(api.snap_save(api.ctx,1));
    CHECK(!host.checkpoint_request(UINT32_MAX));
    CHECK(host.checkpoint_request(0)==0);
    api.resim_begin(api.ctx); CHECK(api.snap_load(api.ctx,0));
    CHECK(!host.checkpoint_request(0));
    rows[1].buttons=0;
    api.publish(api.ctx,0,rows,2,1); CHECK(api.run_tick(api.ctx,0)); api.resim_end(api.ctx);
    CHECK(!host.checkpoint_request(0));
    CHECK(simulation->save_state()==requested); // control bit never reaches the guest
    CHECK(api.digest_master(api.ctx)!=requested_digest);
    CHECK(api.snap_load(api.ctx,1));
    CHECK(host.checkpoint_request(0)==0 && api.digest_master(api.ctx)==requested_digest);
}
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
    CHECK(sample.size==2 && sample.valid && sample.bytes[0]==3 && sample.bytes[1]==4);
    RNetInputSample samples[]={sample,sample};
    api.publish(0,samples,2,api.ctx); CHECK(h.run_published_tick());
    CHECK(h.next_tick()==1);
    CHECK(h.checkpoint_request(0)==0); // bit 10 is session control, higher bits are stripped
    CHECK(simulation->machine(0).bus.read32(0x02000000)==0x22251114);
    samples[0].size=1; api.publish(1,samples,2,api.ctx);
    CHECK(h.return_to_lobby_requested() && !h.run_published_tick());
    CHECK(h.next_tick()==1);
}
void local_output() {
    using namespace gbarecomp;
    auto simulation=create(true,true); GbaNetplayHost host(*simulation); const auto api=host.callbacks();
    GbaNetplayOutput output;
    CHECK(!host.take_output(0,output));
    RNetRbFrame rows[2]{}; for (auto& row:rows) row.is_valid=1;
    CHECK(api.snap_save(api.ctx,0));
    api.publish(api.ctx,0,rows,2,0); CHECK(host.run_published_tick());
    const auto state=simulation->save_state();
    CHECK(host.take_output(1,output)); CHECK(output.tick==0 && output.machine==0);
    CHECK(output.audio.size()>=548 && output.audio.size()<=550);
    CHECK(!host.take_output(0,output)); // one local view, no duplicate sound
    CHECK(simulation->save_state()==state);
    std::int16_t samples[8];
    for (auto seat:{0,1}) CHECK(simulation->machine(seat).bus.audio().drain_samples(samples,8)==0);
    api.resim_begin(api.ctx); CHECK(api.snap_load(api.ctx,0));
    rows[1].buttons=16;
    api.publish(api.ctx,0,rows,2,1); CHECK(api.run_tick(api.ctx,0));
    CHECK(!host.take_output(1,output)); api.resim_end(api.ctx);
    CHECK(!host.take_output(1,output)); // resim_end does not present catch-up frames
    for (unsigned tick=1;tick<4;++tick) {
        api.publish(api.ctx,tick,rows,2,0); CHECK(host.run_published_tick());
    }
    CHECK(host.take_output(0,output)); CHECK(output.tick==3 && output.machine==1);
    CHECK(output.audio.size()>=548 && output.audio.size()<=550); // only newest tick
    host.begin_match(); CHECK(!host.take_output(0,output));
    bool rejected=false;
    try { (void)host.take_output(2,output); } catch (const std::out_of_range&) { rejected=true; }
    CHECK(rejected);
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
    GbaConfirmedCheckpoint exact;
    CHECK(h.copy_checkpoint(1,0,exact) && exact.state==after_zero && exact.next_tick==1);
    CHECK(!h.copy_checkpoint(2,0,exact)); // current watermark excludes this boundary
    CHECK(!h.copy_checkpoint(1,UINT32_MAX,exact));
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
    CHECK(!h.copy_checkpoint(2,0,exact)); // cached watermark cannot override demotion
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
void agreement_gate() {
    using namespace gbarecomp;
    auto simulation=create(); GbaNetplayHost host(*simulation);
    RNetConfig config; rnet_config_init_defaults(&config);
    auto callbacks=host.delay_callbacks(); auto* network=rnet_session_create(&config,&callbacks);
    CHECK(network);
    GbaNetplayCheckpointAgreement agreement(host,network,0,"fixture",UINT32_MAX,0);
    bool refused=false;
    try { (void)agreement.archive(); } catch (const std::logic_error&) { refused=true; }
    CHECK(refused);
    CHECK(agreement.poll(1)==GbaNetplayCheckpointAgreement::Status::Waiting);
    CHECK(agreement.poll(60001)==GbaNetplayCheckpointAgreement::Status::Failed);
    refused=false;
    try { (void)agreement.checkpoint(); } catch (const std::logic_error&) { refused=true; }
    CHECK(refused);
    rnet_session_destroy(network);
}
void demoted_candidate() {
    using namespace gbarecomp;
    auto simulation=create(true); GbaNetplayHost host(*simulation); const auto api=host.callbacks();
    RNetRbFrame rows[2]{}; for (auto& row:rows) row.is_valid=1;
    for (unsigned tick=0;tick<3;++tick) {
        CHECK(api.snap_save(api.ctx,tick)); rows[0].buttons=tick+1;
        api.publish(api.ctx,tick,rows,2,0); CHECK(host.run_published_tick());
    }
    CHECK(host.retain_confirmed(1));
    const auto accepted=host.confirmed_checkpoint();
    CHECK(host.checkpoint_unchanged(accepted));
    const auto stale=host.confirmed_checkpoint().state;
    CHECK(!stale.empty());
    api.resim_begin(api.ctx); CHECK(api.snap_load(api.ctx,1)); api.snap_drop_after(api.ctx,1);
    CHECK(host.confirmed_checkpoint().state.empty());
    rows[0].buttons=16;
    api.publish(api.ctx,1,rows,2,1); CHECK(api.run_tick(api.ctx,1)); api.resim_end(api.ctx);
    CHECK(host.retain_confirmed(1));
    CHECK(host.confirmed_checkpoint().state!=stale);
    CHECK(!host.checkpoint_unchanged(accepted));
    CHECK(host.confirmed_checkpoint().state==simulation->save_state());
}
// Three and four consoles through the same host callbacks: every seat's row
// reaches its own machine, a correction replays to exactly the direct run,
// output follows the seat mapping, and a short publication is refused.
void n_player_host(unsigned players) {
    using namespace gbarecomp;
    auto simulation=create(true,true,players); GbaNetplayHost h(*simulation); const auto api=h.callbacks();
    RNetRbFrame rows[4]{};
    for (auto& row:rows) row.is_valid=1;
    const auto run=[&](unsigned tick,unsigned base,bool replay) {
        for (unsigned seat=0;seat<players;++seat) rows[seat].buttons=static_cast<std::uint16_t>(base<<seat);
        if (!replay) CHECK(api.snap_save(api.ctx,tick));
        api.publish(api.ctx,tick,rows,static_cast<int>(players),replay ? 1 : 0);
        CHECK(replay ? api.run_tick(api.ctx,tick)!=0 : h.run_published_tick());
    };
    run(0,1,false); run(1,2,false); run(2,1,false);
    // Every console saw port 2 (and port 3 on a four-port cable).
    for (unsigned port=0;port<players;++port) {
        const auto high=simulation->machine(port).bus.read32(0x02000004);
        CHECK((high&0xffff)!=0xffff && ((high>>16)==0xffff)==(players==3));
    }
    GbaNetplayOutput output;
    CHECK(h.take_output(0,output) && output.machine==players-1 && output.tick==2);
    api.resim_begin(api.ctx); CHECK(api.snap_load(api.ctx,1)); api.snap_drop_after(api.ctx,1);
    run(1,4,true); run(2,8,true);
    api.resim_end(api.ctx);
    CHECK(!h.take_output(0,output) && h.next_tick()==3 && h.error().empty());
    auto expected=create(true,true,players);
    for (unsigned base:{1u,4u,8u}) {
        std::vector<std::uint16_t> inputs;
        for (unsigned seat=0;seat<players;++seat) inputs.push_back(static_cast<std::uint16_t>(base<<seat));
        expected->run_frame(inputs);
    }
    CHECK(simulation->save_state()==expected->save_state());
    CHECK(api.digest_master(api.ctx)==expected->state_hash());
    for (unsigned port=0;port<players;++port) // seat N-1-p's corrected keys
        CHECK(simulation->machine(port).bus.save().sram_read(0)==((8u<<(players-1-port))&0xff));
    CHECK(api.snap_save(api.ctx,3));
    api.publish(api.ctx,3,rows,static_cast<int>(players)-1,0);
    CHECK(h.return_to_lobby_requested() && !h.run_published_tick());
}
// The agreed boundary outlives the rollback ring, is recaptured by a replay
// across it, and a correction before it withdraws the old candidate.
void pinned_checkpoint() {
    using namespace gbarecomp;
    auto simulation=create(true); GbaNetplayHost h(*simulation); const auto api=h.callbacks();
    CHECK(h.snapshot_depth()==GbaNetplayHost::kSnapshotDepth);
    h.pin_checkpoint(2);
    RNetRbFrame rows[2]{}; for (auto& row:rows) row.is_valid=1;
    std::vector<std::uint8_t> at_two;
    const unsigned last=h.snapshot_depth()+8;
    for (unsigned tick=0;tick<last;++tick) {
        if (tick==2) at_two=simulation->save_state();
        CHECK(api.snap_save(api.ctx,tick)); rows[0].buttons=static_cast<std::uint16_t>(tick&0xff);
        api.publish(api.ctx,tick,rows,2,0); CHECK(h.run_published_tick());
    }
    CHECK(!api.snap_has(api.ctx,2)); // evicted from the rollback ring
    GbaConfirmedCheckpoint pinned;
    CHECK(h.copy_checkpoint(2,last-1,pinned) && pinned.next_tick==2 && pinned.state==at_two);
    CHECK(h.checkpoint_unchanged(pinned));
    CHECK(!h.copy_checkpoint(3,last-1,pinned)); // only the pinned boundary survives
    // A correction starting before the boundary withdraws it until the
    // corrected run passes the boundary again, then pins the new bytes.
    h.pin_checkpoint(last-3);
    CHECK(api.snap_save(api.ctx,last));
    rows[0].buttons=1; api.publish(api.ctx,last,rows,2,0); CHECK(h.run_published_tick());
    CHECK(h.copy_checkpoint(last-3,last,pinned)); const auto accepted=pinned;
    api.resim_begin(api.ctx); CHECK(api.snap_load(api.ctx,last-4)); api.snap_drop_after(api.ctx,last-4);
    CHECK(!h.checkpoint_unchanged(accepted));
    for (unsigned tick=last-4;tick<=last;++tick) {
        if (tick!=last-4) CHECK(api.snap_save(api.ctx,tick));
        rows[0].buttons=0x200; api.publish(api.ctx,tick,rows,2,1); CHECK(api.run_tick(api.ctx,tick));
    }
    api.resim_end(api.ctx);
    CHECK(h.copy_checkpoint(last-3,last,pinned) && pinned.state!=accepted.state);
    CHECK(!h.checkpoint_unchanged(accepted) && h.checkpoint_unchanged(pinned));
    CHECK(h.error().empty());
}
}
int main() { n_player_host(3); n_player_host(4); pinned_checkpoint(); admission_pacing(); confirmed_session_control(); restore_corrected_session(); delay_publication(); local_output(); confirmed_checkpoint(); agreement_gate(); demoted_candidate(); std::puts("whole-session netplay host tests passed"); }
