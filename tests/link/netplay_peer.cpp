#include "multiplayer_netplay.h"
#include "multiplayer_startup.h"
#include "multiplayer_checkpoint.h"
#include "netplay_fixture.h"
#include <recomp_net/recomp_net.h>
#include <retcomm_rbengine/mono_ms.h>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <thread>
#include <chrono>

int main(int argc,char** argv) {
    if (argc!=6) return 2; // slot, base port, session nonce, rollback, output
    int slot=std::atoi(argv[1]),slots=link_fixture::players(),delay=2,prediction=6;
    unsigned port=std::strtoul(argv[2],nullptr,10);
    bool rollback=std::atoi(argv[4])!=0;
    if (slot<0 || slot>=slots) return 2;
    auto simulation=link_fixture::create(true,false,static_cast<unsigned>(slots));
    simulation->input_machine(slot).bus.save().sram_write(1,0x90+slot);
    gbarecomp::GbaNetplayHost host(*simulation);
    host.sample_local=[slot](std::uint32_t tick) { return std::uint16_t(((tick/5+slot)%3) ? (1u<<slot) : 0); };
    RNetConfig config; rnet_config_init_defaults(&config);
    config.local_slot=slot; config.input_delay=delay; config.session_id=std::strtoul(argv[3],nullptr,10);
    config.slot_count=static_cast<rnet_u8>(slots); config.occupied_mask=(1u<<slots)-1;
    auto transport_host=host.delay_callbacks();
    auto* session=rnet_session_create(&config,&transport_host);
    char bind[64],peer[64];
    std::snprintf(bind,sizeof(bind),"127.0.0.1:%u",port+slot);
    const auto* routed=std::getenv("GBA_TEST_PEER_PORT");
    const unsigned peer_port=routed ? std::strtoul(routed,nullptr,10) : port+1-slot;
    std::snprintf(peer,sizeof(peer),"127.0.0.1:%u",peer_port);
    const bool hub=link_fixture::hub(slot);
    const auto start_transport=[&] {
        return hub ? rnet_session_start_lan_hub(session,bind) : rnet_session_start_lan(session,bind,peer);
    };
    if (!session || start_transport()) return 3;
    auto start=rbe_mono_ms();
    while (!rnet_session_is_running(session) && rbe_mono_ms()-start<5000) {
        rnet_session_pump(session); std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!rnet_session_is_running(session)) return 4;
    const auto* boot_identity=std::getenv("GBA_TEST_BOOT_ID");
    gbarecomp::GbaNetplayStartup bootstrap(*simulation,session,slot,
        boot_identity ? boot_identity : "link-fixture/runtime-v1/bios-none/mods-none",1000+slot);
    for (;;) {
        auto result=bootstrap.poll(rbe_mono_ms());
        if (result==gbarecomp::GbaNetplayStartup::Status::Failed) {
            std::fprintf(stderr,"startup: %s\n",bootstrap.error().c_str()); return 11;
        }
        if (result==gbarecomp::GbaNetplayStartup::Status::Ready) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!link_fixture::owner_saves_intact(*simulation)) {
        std::fprintf(stderr,"startup did not preserve every player's individual save\n"); return 12;
    }
    host.begin_match();
    RNetRbDriver* driver=nullptr;
    if (rollback) {
        driver=rnet_rb_driver_create();
        RNetRbDriverConfig cfg{};
        cfg.session=&session; cfg.local_slot=&slot; cfg.slot_count=&slots;
        cfg.input_delay=&delay; cfg.input_prediction=&prediction;
        cfg.replay_mode=RNET_RB_REPLAY_INCREMENTAL;
        cfg.part_names[0]="GBA0"; cfg.part_names[1]="other-GBAs"; cfg.part_names[2]="cable-and-scheduler";
        cfg.snap_depth=host.snapshot_depth();
        cfg.log_prefix="gba_rb"; cfg.env_alias="GBA_RB";
        rnet_rb_driver_set_identity(driver,0x47424101,0x12345678);
        auto callbacks=host.callbacks();
        if (!rnet_rb_driver_start(driver,&cfg,&callbacks)) return 5;
    }
    std::map<std::uint32_t,std::uint32_t> timeline;
    start=rbe_mono_ms(); bool drained=false, saw_reconnecting=false, recovered=false;
    while (rbe_mono_ms()-start<55000) {
        rnet_session_pump(session);
        const auto connection=gbarecomp::gba_netplay_connection_status(session);
        if (connection.phase==gbarecomp::GbaConnectionPhase::Reconnecting) saw_reconnecting=true;
        if (saw_reconnecting && connection.phase==gbarecomp::GbaConnectionPhase::Connected) recovered=true;
        if (connection.phase==gbarecomp::GbaConnectionPhase::TimedOut ||
            connection.phase==gbarecomp::GbaConnectionPhase::PeerLeft) {
            // A draining peer may already have sent its final marker then
            // left. Keep polling so the driver consumes that queued marker.
            if (!driver || rnet_rb_driver_quiesce_state(driver)==RNET_RB_QUIESCE_NONE) {
                std::fprintf(stderr,"connection recovery grace expired or peer left\n"); return 13;
            }
        }
        if (host.return_to_lobby_requested()) { std::fprintf(stderr,"host error: %s\n",host.error().c_str()); return 6; }
        // A simulated tick can still be speculative. Drain only after the
        // agreed watermark covers the target; quiesce deliberately stops new
        // corrections, and cannot make an unconfirmed tail authoritative.
        if (driver && host.next_tick()>=80 && rnet_rb_driver_confirmed_through(driver)>=79)
            rnet_rb_driver_request_quiesce(driver);
        if (driver && rnet_rb_driver_quiesce_state(driver)==RNET_RB_QUIESCE_DRAINED) { drained=true; break; }
        if (!driver && host.next_tick()>=80) { drained=true; break; }
        bool ran=false,replay=false;
        if (driver) {
            auto admit=rnet_rb_driver_poll_admit(driver);
            replay=admit==RNET_RB_ADMIT_REPLAY;
            if (admit!=RNET_RB_ADMIT_STALL) {
                ran=host.run_published_tick();
                if (!ran) { std::fprintf(stderr,"tick error: %s\n",host.error().c_str()); return 7; }
                rnet_rb_driver_finish_frame(driver);
            }
        } else ran=host.try_delay_frame(session);
        if (ran) {
            timeline[host.next_tick()-1]=simulation->state_hash();
            simulation->discard_audio_output();
            if (slot==0 && host.next_tick()==20) if (const auto* trigger=std::getenv("GBA_TEST_OUTAGE_TRIGGER")) {
                if (FILE* signal=std::fopen(trigger,"w")) { std::fputs("ready\n",signal); std::fclose(signal); }
            }
        }
        if (driver) {
            // At this shared-driver pin zero is also the uninitialized
            // watermark. No separate validity bit exists: wait past zero.
            const auto through=rnet_rb_driver_confirmed_through(driver);
            if (through && !rnet_rb_driver_episode_active(driver)) host.retain_confirmed(through);
        } else if (host.next_tick()) host.retain_confirmed(host.next_tick()-1);
        if (!replay) std::this_thread::sleep_for(std::chrono::milliseconds(ran ? 16 : 1));
    }
    if (!drained) { std::fprintf(stderr,"loopback did not drain: tick %u %s\n",host.next_tick(),host.error().c_str()); return 8; }
    const auto confirmed=driver ? rnet_rb_driver_confirmed_through(driver) : host.next_tick()-1;
    const auto replay_ticks=driver ? rnet_rb_driver_resim_ticks(driver) : 0;
    const auto episodes=driver ? rnet_rb_driver_episode_count(driver) : 0;
    const auto desyncs=driver ? rnet_rb_driver_desync_count(driver) : 0;
    if (driver) { rnet_rb_driver_destroy(driver); driver=nullptr; }
    if (std::getenv("GBA_TEST_CHECKPOINT_CORRUPT")) simulation->machine(1).bus.write8(0x02000100,0x5a);
    // Stop admission/episodes before freezing the selected common boundary.
    // Delay-sync ends at exactly 80; rollback's drained tails can differ, so
    // select the shared earlier snapshot at 60, still in each local ring.
    gbarecomp::GbaNetplayCheckpointAgreement agreement(host,session,slot,
        "link-fixture/runtime-v1/bios-none/mods-none",confirmed,rollback ? 60 : 80);
    for (;;) {
        const auto status=agreement.poll(rbe_mono_ms());
        if (status==gbarecomp::GbaNetplayCheckpointAgreement::Status::Failed) {
            std::fprintf(stderr,"checkpoint agreement: %s\n",agreement.error().c_str()); return 14;
        }
        if (status==gbarecomp::GbaNetplayCheckpointAgreement::Status::Ready) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto archive=agreement.archive();
    gbarecomp::GbaConfirmedCheckpoint decoded;
    std::string error;
    if (!gbarecomp::gba_decode_checkpoint_archive(archive,"link-fixture/runtime-v1/bios-none/mods-none",decoded,&error) ||
        decoded.state!=agreement.checkpoint().state || decoded.next_tick!=agreement.checkpoint().next_tick) return 15;
    const auto archive_path=std::filesystem::path(std::string(argv[5])+".paired");
    // Creation, replacement and a failed write must leave a loadable agreed
    // pair. Test files live only in the harness's private temporary directory.
    if (!gbarecomp::gba_store_agreed_checkpoint(agreement,archive_path,&error) ||
        !gbarecomp::gba_store_agreed_checkpoint(agreement,archive_path,&error) ||
        gbarecomp::gba_store_agreed_checkpoint(agreement,archive_path/"nonexistent"/"fail",&error) ||
        !gbarecomp::gba_load_checkpoint_archive(archive_path,"link-fixture/runtime-v1/bios-none/mods-none",decoded,&error) ||
        decoded.state!=agreement.checkpoint().state) {
        std::fprintf(stderr,"checkpoint storage: %s\n",error.c_str()); return 16;
    }
    const auto unchanged=decoded.state;
    // Corruption, wrong program identity and truncation must not replace the
    // last usable checkpoint. Tick/header corruption is covered by the digest.
    for (auto offset: {std::size_t(0),std::size_t(20),archive.size()/2,archive.size()-1}) {
        archive[offset]^=1;
        if (gbarecomp::gba_decode_checkpoint_archive(archive,"link-fixture/runtime-v1/bios-none/mods-none",decoded,&error) ||
            decoded.state!=unchanged) return 17;
        archive[offset]^=1;
    }
    if (gbarecomp::gba_decode_checkpoint_archive(archive,"another-build",decoded,&error) ||
        gbarecomp::gba_decode_checkpoint_archive(std::span(archive).first(12),
            "link-fixture/runtime-v1/bios-none/mods-none",decoded,&error)) return 17;
    FILE* report=std::fopen(argv[5],"w"); if (!report) return 9;
    std::fprintf(report,"confirmed %u replay %llu episodes %u desyncs %u reconnecting %u recovered %u\n",confirmed,
        static_cast<unsigned long long>(replay_ticks),episodes,desyncs,
        saw_reconnecting,recovered);
    for (auto [tick,hash]:timeline) if (tick<=confirmed) std::fprintf(report,"%u %08x\n",tick,hash);
    std::fclose(report);
    // Keep pumping long enough for the other peer's final confirmation/QUIESCE.
    start=rbe_mono_ms();
    while (rbe_mono_ms()-start<1000) { rnet_session_pump(session); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    rnet_session_destroy(session);
    const auto checkpoint_tick=host.confirmed_checkpoint().next_tick;
    if (!checkpoint_tick || !host.restore_confirmed_for_restart() ||
        simulation->state_hash()!=timeline.at(checkpoint_tick-1)) {
        std::fprintf(stderr,"confirmed checkpoint did not restore its recorded timeline\n"); return 10;
    }
    if (!simulation->load_state(decoded.state,&error) ||
        simulation->state_hash()!=timeline.at(decoded.next_tick-1)) {
        std::fprintf(stderr,"agreed paired archive did not restore its recorded timeline\n"); return 18;
    }
    host.begin_match();
    if (host.next_tick()!=0) return 18;
    if (std::getenv("GBA_TEST_RESTART")) {
        // A fresh connection restores the agreed paired archive, verifies that
        // exact warm state on both peers, and restarts controller time at zero.
        // New session identity rejects packets still in flight from the old
        // match even though this test deliberately reuses its UDP endpoints.
        config.session_id++; config.input_delay=2;
        session=rnet_session_create(&config,&transport_host);
        if (!session || start_transport()) return 19;
        gbarecomp::GbaNetplayCheckpointAgreement restored(host,session,slot,
            "link-fixture/runtime-v1/bios-none/mods-none",UINT32_MAX,0);
        for (;;) {
            const auto status=restored.poll(rbe_mono_ms());
            if (status==gbarecomp::GbaNetplayCheckpointAgreement::Status::Failed) {
                std::fprintf(stderr,"restart agreement: %s\n",restored.error().c_str()); return 19;
            }
            if (status==gbarecomp::GbaNetplayCheckpointAgreement::Status::Ready) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        rnet_session_hard_resync(session);
        const std::uint8_t neutral[2]{}; rnet_session_prime_delay_inputs(session,neutral,sizeof(neutral));
        start=rbe_mono_ms();
        while (host.next_tick()<30 && rbe_mono_ms()-start<15000) {
            rnet_session_pump(session);
            const bool ran=host.try_delay_frame(session);
            if (host.return_to_lobby_requested()) return 19;
            if (ran) simulation->discard_audio_output();
            std::this_thread::sleep_for(std::chrono::milliseconds(ran ? 16 : 1));
        }
        if (host.next_tick()!=30) return 19;
        FILE* restarted=std::fopen((std::string(argv[5])+".restart").c_str(),"w");
        if (!restarted) return 19;
        std::fprintf(restarted,"%u %08x\n",host.next_tick(),simulation->state_hash()); std::fclose(restarted);
        start=rbe_mono_ms();
        while (rbe_mono_ms()-start<1000) { rnet_session_pump(session); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        rnet_session_destroy(session);
    }
    return 0;
}
