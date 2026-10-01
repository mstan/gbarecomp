#include "multiplayer_match.h"
#include "netplay_fixture.h"
#include <retcomm_rbengine/mono_ms.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <thread>

namespace {
using namespace gbarecomp;
constexpr auto identity="link-fixture/runtime-v1/bios-none/mods-none";
int run(GbaMultiplayerSession& simulation,int slot,unsigned port,unsigned nonce,bool rollback,
        const std::string& output,bool restored=false) {
    GbaNetplayMatchOptions options;
    options.network.local_slot=slot; options.network.session_id=nonce;
    const auto seats=static_cast<unsigned>(simulation.input_count());
    options.network.slot_count=static_cast<rnet_u8>(seats); options.network.occupied_mask=(1u<<seats)-1;
    const auto* override_identity=std::getenv("GBA_TEST_BOOT_ID");
    options.identity=override_identity ? override_identity : identity;
    options.build_fingerprint=0x47424103; options.rollback=rollback;
    options.restored_pair=restored; options.rtc_seed_seconds=1000+slot;
    if (std::getenv("GBA_TEST_POLICY_MISMATCH")) options.network.input_delay=3;
    GbaNetplayMatch match(simulation,std::move(options));
    match.sample_local=[slot](std::uint32_t tick) { return std::uint16_t(((tick/5+slot)%3) ? (1u<<slot) : 0); };
    const auto* routed=std::getenv("GBA_TEST_PEER_PORT");
    const auto peer_port=routed ? std::strtoul(routed,nullptr,10) : port+1-slot;
    const auto bind="127.0.0.1:"+std::to_string(port+slot), peer="127.0.0.1:"+std::to_string(peer_port);
    if (link_fixture::hub(slot) ? rnet_session_start_lan_hub(match.transport(),bind.c_str()) :
        rnet_session_start_lan(match.transport(),bind.c_str(),peer.c_str())) return 3;
    std::string error;
    // A runner cannot publish a speculative pair, even before its first poll.
    if (match.store_checkpoint(output+".paired",&error) || std::filesystem::exists(output+".paired")) return 20;
    unsigned target=restored ? 30 : 80;
    const bool requested=!restored && std::getenv("GBA_TEST_REQUEST_CHECKPOINT");
    if (!requested) match.finish_at(target);
    std::map<std::uint32_t,std::uint32_t> timeline;
    bool saw_reconnecting=false,recovered=false;
    const auto start=rbe_mono_ms();
    GbaNetplayOutput frame;
    std::uint64_t heartbeat=start;
    while (match.phase()!=GbaNetplayMatch::Phase::CheckpointReady && rbe_mono_ms()-start<95000) {
        if (rbe_mono_ms()-heartbeat>=5000) {
            // Survives a harness kill: shows where a stalled seat is stuck.
            heartbeat=rbe_mono_ms();
            RNetSessionStats stats{}; rnet_session_get_stats(match.transport(),&stats);
            std::fprintf(stderr,"peer heartbeat seat=%d tick=%u phase=%d connection=%d stall=%s tips=",slot,
                match.next_tick(),static_cast<int>(match.phase()),static_cast<int>(match.connection().phase),
                rnet_admit_stall_name(stats.last_stall));
            for (int other=0;other<static_cast<int>(simulation.input_count());++other) {
                rnet_u32 tip=0;
                if (other!=slot && rnet_session_remote_tip(match.transport(),other,&tip)) std::fprintf(stderr,"%d:%u ",other,tip);
            }
            std::fputc('\n',stderr);
        }
        if (match.phase()==GbaNetplayMatch::Phase::Running) {
            const auto tick=match.next_tick();
            if (requested && slot==1 && tick>=20) match.request_checkpoint();
            if (match.poll(false)!=GbaNetplayMatch::Step::Idle || match.next_tick()!=tick) return 23;
        }
        const auto step=match.poll();
        if (match.phase()==GbaNetplayMatch::Phase::Failed) {
            std::fprintf(stderr,"match: %s\n",match.error().c_str());
            return match.next_tick() ? 6 : 11;
        }
        if (match.connection().phase==GbaConnectionPhase::Reconnecting) saw_reconnecting=true;
        if (saw_reconnecting && match.connection().phase==GbaConnectionPhase::Connected) recovered=true;
        if (step!=GbaNetplayMatch::Step::Idle) {
            if (!link_fixture::owner_saves_intact(simulation)) return 12;
            const bool replay=step==GbaNetplayMatch::Step::Replay;
            const bool output_ready=match.take_output(frame);
            if (output_ready==replay || (output_ready && (frame.tick+1!=match.next_tick() || frame.machine!=unsigned(slot))) ||
                match.take_output(frame)) return 21;
            timeline[match.next_tick()-1]=simulation.state_hash();
            if (slot==0 && match.next_tick()==20) if (const auto* trigger=std::getenv("GBA_TEST_OUTAGE_TRIGGER")) {
                std::ofstream signal(trigger); signal << "ready\n";
            }
        }
        if (step!=GbaNetplayMatch::Step::Replay)
            std::this_thread::sleep_for(std::chrono::milliseconds(step==GbaNetplayMatch::Step::Forward ? 16 : 1));
    }
    if (match.phase()!=GbaNetplayMatch::Phase::CheckpointReady) return 8;
    if (requested) {
        target=match.checkpoint().next_tick;
        if (target<84 || target>128) return 24;
    }
    if (match.checkpoint().next_tick!=target || match.take_output(frame)) return 22;
    if (!match.store_checkpoint(output+".paired",&error)) return 16;
    GbaConfirmedCheckpoint decoded;
    if (!gba_load_checkpoint_archive(output+".paired",identity,decoded,&error) ||
        decoded.state!=match.checkpoint().state || !simulation.load_state(decoded.state,&error) ||
        simulation.state_hash()!=timeline.at(target-1)) return 18;
    std::ofstream report(output);
    if (!restored) report << "confirmed " << target-1 << " replay " << match.replay_ticks()
                         << " reconnecting " << saw_reconnecting << " recovered " << recovered << '\n';
    for (auto [tick,hash]:timeline) if (tick<target) report << tick << ' ' << std::hex << hash << std::dec << '\n';
    report.close();
    const auto linger=rbe_mono_ms();
    while (rbe_mono_ms()-linger<1000) { match.poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    return 0;
}
}
int main(int argc,char** argv) {
    if (argc!=6) return 2;
    std::setvbuf(stderr,nullptr,_IONBF,0); // logs must survive a harness kill
    try {
        const int slot=std::atoi(argv[1]);
        const unsigned port=std::strtoul(argv[2],nullptr,10), nonce=std::strtoul(argv[3],nullptr,10);
        const bool rollback=std::atoi(argv[4])!=0;
        const auto players=static_cast<unsigned>(link_fixture::players());
        if (slot<0 || slot>=static_cast<int>(players)) return 2;
        auto simulation=link_fixture::create(true,false,players);
        simulation->input_machine(slot).bus.save().sram_write(1,0x90+slot);
        const auto status=run(*simulation,slot,port,nonce,rollback,argv[5]);
        if (status || !std::getenv("GBA_TEST_RESTART")) return status;
        return run(*simulation,slot,port,nonce+1,false,std::string(argv[5])+".restart",true);
    } catch (const std::exception& e) { std::fprintf(stderr,"match exception: %s\n",e.what()); return 1; }
}
