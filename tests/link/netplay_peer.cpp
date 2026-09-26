#include "multiplayer_netplay.h"
#include "multiplayer_startup.h"
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
    int slot=std::atoi(argv[1]),slots=2,delay=2,prediction=6;
    unsigned port=std::strtoul(argv[2],nullptr,10);
    bool rollback=std::atoi(argv[4])!=0;
    auto simulation=link_fixture::create(true);
    simulation->input_machine(slot).bus.save().sram_write(1,0x90+slot);
    gbarecomp::GbaNetplayHost host(*simulation);
    host.sample_local=[slot](std::uint32_t tick) { return std::uint16_t(((tick/5+slot)%3) ? (1u<<slot) : 0); };
    RNetConfig config; rnet_config_init_defaults(&config);
    config.local_slot=slot; config.input_delay=delay; config.session_id=std::strtoul(argv[3],nullptr,10);
    auto transport_host=host.delay_callbacks();
    auto* session=rnet_session_create(&config,&transport_host);
    char bind[64],peer[64];
    std::snprintf(bind,sizeof(bind),"127.0.0.1:%u",port+slot);
    std::snprintf(peer,sizeof(peer),"127.0.0.1:%u",port+1-slot);
    if (!session || rnet_session_start_lan(session,bind,peer)) return 3;
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
    if (simulation->input_machine(0).bus.save().sram_read(1)!=0x90 ||
        simulation->input_machine(1).bus.save().sram_read(1)!=0x91) {
        std::fprintf(stderr,"startup did not preserve both players' individual saves\n"); return 12;
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
        cfg.snap_depth=gbarecomp::GbaNetplayHost::kSnapshotDepth;
        cfg.log_prefix="gba_rb"; cfg.env_alias="GBA_RB";
        rnet_rb_driver_set_identity(driver,0x47424101,0x12345678);
        auto callbacks=host.callbacks();
        if (!rnet_rb_driver_start(driver,&cfg,&callbacks)) return 5;
    }
    std::map<std::uint32_t,std::uint32_t> timeline;
    start=rbe_mono_ms(); bool drained=false;
    while (rbe_mono_ms()-start<55000) {
        rnet_session_pump(session);
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
    FILE* report=std::fopen(argv[5],"w"); if (!report) return 9;
    const auto confirmed=driver ? rnet_rb_driver_confirmed_through(driver) : host.next_tick()-1;
    std::fprintf(report,"confirmed %u replay %llu episodes %u desyncs %u\n",confirmed,
        static_cast<unsigned long long>(driver ? rnet_rb_driver_resim_ticks(driver) : 0),
        driver ? rnet_rb_driver_episode_count(driver) : 0,driver ? rnet_rb_driver_desync_count(driver) : 0);
    for (auto [tick,hash]:timeline) if (tick<=confirmed) std::fprintf(report,"%u %08x\n",tick,hash);
    std::fclose(report);
    // Keep pumping long enough for the other peer's final confirmation/QUIESCE.
    start=rbe_mono_ms();
    while (rbe_mono_ms()-start<1000) { rnet_session_pump(session); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    if (driver) rnet_rb_driver_destroy(driver);
    rnet_session_destroy(session);
    const auto checkpoint_tick=host.confirmed_checkpoint().next_tick;
    if (!checkpoint_tick || !host.restore_confirmed_for_restart() ||
        simulation->state_hash()!=timeline.at(checkpoint_tick-1)) {
        std::fprintf(stderr,"confirmed checkpoint did not restore its recorded timeline\n"); return 10;
    }
    return 0;
}
