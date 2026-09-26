#include "netplay_probe.h"
#include "multiplayer_netplay.h"
#include "multiplayer_startup.h"
#include "multiplayer_checkpoint.h"
#include <recomp_net/recomp_net.h>
#include <retcomm_rbengine/mono_ms.h>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {
const char* required(const char* name) {
    const auto* value=std::getenv(name);
    if (!value || !*value) throw std::invalid_argument(std::string("missing ")+name);
    return value;
}
unsigned number(const char* name,unsigned maximum) {
    const std::string_view value(required(name)); unsigned result=0;
    const auto parsed=std::from_chars(value.data(),value.data()+value.size(),result);
    if (parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size() || result>maximum)
        throw std::invalid_argument(std::string("invalid ")+name);
    return result;
}
void idle() { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
template<class Barrier> void wait_barrier(Barrier& barrier) {
    for (;;) {
        const auto status=barrier.poll(rbe_mono_ms());
        if (status==Barrier::Status::Failed) throw std::runtime_error(barrier.error());
        if (status==Barrier::Status::Ready) return;
        idle();
    }
}
}
void gba_link_probe_netplay(gbarecomp::GbaMultiplayerSession& simulation,unsigned frames,
    const std::function<std::array<std::uint16_t,2>(std::uint32_t)>& input,const std::string& identity) {
    using namespace gbarecomp;
    int seat=number("GBA_LINK_PROBE_NET_SEAT",1),slots=2,delay=2,prediction=6;
    const auto nonce=number("GBA_LINK_PROBE_NET_SESSION",UINT32_MAX);
    if (!nonce || frames<2) throw std::invalid_argument("network probe needs a nonzero session ID and at least two frames");
    const auto* mode=std::getenv("GBA_LINK_PROBE_NET_ROLLBACK");
    const bool rollback=!mode || std::string_view(mode)!="0";
    const auto timeout=std::getenv("GBA_LINK_PROBE_NET_TIMEOUT_MS") ?
        number("GBA_LINK_PROBE_NET_TIMEOUT_MS",3600000) : 300000;
    if (timeout<1000) throw std::invalid_argument("network probe simulation deadline must be 1 second to 1 hour");
    GbaNetplayHost host(simulation);
    host.sample_local=[&](std::uint32_t tick) { return input(tick)[seat]; };
    RNetConfig config; rnet_config_init_defaults(&config);
    config.local_slot=seat; config.input_delay=delay; config.session_id=nonce;
    auto callbacks=host.delay_callbacks();
    std::unique_ptr<RNetSession,decltype(&rnet_session_destroy)> owner(rnet_session_create(&config,&callbacks),rnet_session_destroy);
    auto* session=owner.get();
    if (!session || rnet_session_start_lan(session,required("GBA_LINK_PROBE_NET_BIND"),required("GBA_LINK_PROBE_NET_PEER")))
        throw std::runtime_error("cannot start network probe transport");
    if (!simulation.cycle()) {
        GbaNetplayStartup startup(simulation,session,seat,identity,0);
        wait_barrier(startup); host.begin_match();
    } else {
        // Warm local snapshots are never silently assumed equal. Both peers
        // independently verify every byte before starting the new input era.
        GbaNetplayCheckpointAgreement restored(host,session,seat,identity,UINT32_MAX,0);
        wait_barrier(restored);
        rnet_session_hard_resync(session);
        const std::uint8_t neutral[2]{}; rnet_session_prime_delay_inputs(session,neutral,sizeof(neutral));
    }
    std::unique_ptr<RNetRbDriver,decltype(&rnet_rb_driver_destroy)> driver(nullptr,rnet_rb_driver_destroy);
    RNetRbDriverConfig cfg{};
    if (rollback) {
        driver.reset(rnet_rb_driver_create());
        if (!driver) throw std::bad_alloc();
        cfg.session=&session; cfg.local_slot=&seat; cfg.slot_count=&slots;
        cfg.input_delay=&delay; cfg.input_prediction=&prediction;
        cfg.replay_mode=RNET_RB_REPLAY_INCREMENTAL; cfg.snap_depth=GbaNetplayHost::kSnapshotDepth;
        cfg.part_names[0]="GBA0"; cfg.part_names[1]="other-GBAs"; cfg.part_names[2]="cable-and-scheduler";
        cfg.log_prefix="gba_probe_rb"; cfg.env_alias="GBA_RB";
        // Full identity and snapshot equality were checked above. This driver
        // ABI identity additionally prevents mixing this probe with a runner.
        rnet_rb_driver_set_identity(driver.get(),0x47424102,simulation.state_hash());
        auto rb_callbacks=host.callbacks();
        if (!rnet_rb_driver_start(driver.get(),&cfg,&rb_callbacks)) throw std::runtime_error("cannot start probe rollback driver");
    }
    const auto started=rbe_mono_ms(); bool done=false;
    GbaNetplayOutput output;
    while (rbe_mono_ms()-started<timeout) {
        rnet_session_pump(session);
        const auto connection=gba_netplay_connection_status(session);
        if (connection.phase==GbaConnectionPhase::TimedOut || connection.phase==GbaConnectionPhase::PeerLeft)
            throw std::runtime_error("network probe peer unavailable");
        if (host.return_to_lobby_requested()) throw std::runtime_error("network probe refused: "+host.error());
        if (driver && host.next_tick()>=frames && rnet_rb_driver_confirmed_through(driver.get())>=frames-1)
            rnet_rb_driver_request_quiesce(driver.get());
        if ((driver && rnet_rb_driver_quiesce_state(driver.get())==RNET_RB_QUIESCE_DRAINED) ||
            (!driver && host.next_tick()>=frames)) { done=true; break; }
        bool ran=false,replay=false;
        if (driver) {
            const auto admit=rnet_rb_driver_poll_admit(driver.get());
            replay=admit==RNET_RB_ADMIT_REPLAY;
            if (admit!=RNET_RB_ADMIT_STALL) {
                ran=host.run_published_tick();
                if (!ran) throw std::runtime_error(host.error());
                rnet_rb_driver_finish_frame(driver.get());
            }
        } else ran=host.try_delay_frame(session);
        if (ran) {
            const bool presented=host.take_output(seat,output);
            if (presented==replay || (presented && output.tick+1!=host.next_tick()))
                throw std::runtime_error("invalid forward/replay output boundary");
            if (!replay && host.next_tick()%30==0) {
                std::printf("network tick=%u hash=%08x\n",host.next_tick(),simulation.state_hash()); std::fflush(stdout);
            }
        }
        if (!replay) std::this_thread::sleep_for(std::chrono::milliseconds(ran ? 16 : 1));
    }
    if (!done) throw std::runtime_error("network probe did not reach its confirmed target");
    const auto through=driver ? rnet_rb_driver_confirmed_through(driver.get()) : host.next_tick()-1;
    const auto replay_ticks=driver ? rnet_rb_driver_resim_ticks(driver.get()) : 0;
    driver.reset(); // no active episodes/admission during checkpoint agreement
    GbaNetplayCheckpointAgreement agreed(host,session,seat,identity,through,frames);
    wait_barrier(agreed);
    std::string error;
    if (!simulation.load_state(agreed.checkpoint().state,&error)) throw std::runtime_error(error);
    std::printf("network agreed tick=%u hash=%08x replay=%llu bytes=%zu\n",frames,simulation.state_hash(),
        static_cast<unsigned long long>(replay_ticks),agreed.checkpoint().state.size());
    std::fflush(stdout);
    const auto finish=rbe_mono_ms();
    while (rbe_mono_ms()-finish<1000) { rnet_session_pump(session); idle(); }
}
