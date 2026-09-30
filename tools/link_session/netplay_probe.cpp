#include "netplay_probe.h"
#include "multiplayer_match.h"
#include "multiplayer_pacing.h"
#include <retcomm_rbengine/mono_ms.h>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

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
}
void gba_link_probe_netplay(gbarecomp::GbaMultiplayerSession& simulation,unsigned frames,
    const std::function<std::array<std::uint16_t,4>(std::uint32_t)>& input,const std::string& identity) {
    using namespace gbarecomp;
    const auto seats=static_cast<unsigned>(simulation.input_count());
    const auto seat=number("GBA_LINK_PROBE_NET_SEAT",seats-1);
    const auto nonce=number("GBA_LINK_PROBE_NET_SESSION",UINT32_MAX);
    if (!nonce || frames<2) throw std::invalid_argument("network probe needs a nonzero session ID and at least two frames");
    const auto* mode=std::getenv("GBA_LINK_PROBE_NET_ROLLBACK");
    const auto timeout=std::getenv("GBA_LINK_PROBE_NET_TIMEOUT_MS") ?
        number("GBA_LINK_PROBE_NET_TIMEOUT_MS",3600000) : 300000;
    if (timeout<1000) throw std::invalid_argument("network probe simulation deadline must be 1 second to 1 hour");
    GbaNetplayMatchOptions options;
    options.network.local_slot=seat; options.network.session_id=nonce;
    options.network.slot_count=static_cast<rnet_u8>(seats); options.network.occupied_mask=(1u<<seats)-1;
    if (std::getenv("GBA_LINK_PROBE_NET_DELAY"))
        options.network.input_delay=number("GBA_LINK_PROBE_NET_DELAY",20);
    options.identity=identity; options.build_fingerprint=0x47424102;
    options.rollback=!mode || std::string_view(mode)!="0";
    options.restored_pair=simulation.cycle()!=0;
    GbaNetplayMatch match(simulation,std::move(options));
    match.sample_local=[&](std::uint32_t tick) { return input(tick)[seat]; };
    // Three or more seats without GBA_LINK_PROBE_NET_PEER on seat 0: that
    // seat is the LAN star hub every other seat dials.
    const auto* peer=std::getenv("GBA_LINK_PROBE_NET_PEER");
    const bool hub=seats>2 && seat==0 && (!peer || !*peer);
    if (hub ? rnet_session_start_lan_hub(match.transport(),required("GBA_LINK_PROBE_NET_BIND")) :
              rnet_session_start_lan(match.transport(),required("GBA_LINK_PROBE_NET_BIND"),required("GBA_LINK_PROBE_NET_PEER")))
        throw std::runtime_error("cannot start network probe transport");
    match.finish_at(frames); // harness has agreed the same target on both peers
    std::uint64_t started=0;
    GbaNetplayPacer pacer;
    double work_ms=0;
    std::uint64_t work_frames=0, target_ms=0;
    GbaNetplayOutput output;
    while (match.phase()!=GbaNetplayMatch::Phase::CheckpointReady) {
        const auto poll_start=GbaNetplayPacer::Clock::now();
        const auto step=match.poll(pacer.ready(poll_start));
        if (step==GbaNetplayMatch::Step::Forward) pacer.forwarded(poll_start);
        if (step!=GbaNetplayMatch::Step::Idle) {
            work_ms+=std::chrono::duration<double,std::milli>(GbaNetplayPacer::Clock::now()-poll_start).count();
            ++work_frames;
            if (!target_ms && match.next_tick()>=frames) target_ms=rbe_mono_ms();
        }
        if (match.phase()==GbaNetplayMatch::Phase::Failed) throw std::runtime_error(match.error());
        if (match.phase()==GbaNetplayMatch::Phase::Running) {
            if (!started) started=rbe_mono_ms();
            if (rbe_mono_ms()-started>=timeout)
                throw std::runtime_error("network probe did not reach its confirmed target");
        }
        if (step!=GbaNetplayMatch::Step::Idle) {
            const bool replay=step==GbaNetplayMatch::Step::Replay;
            const bool presented=match.take_output(output);
            if (presented==replay || (presented && output.tick+1!=match.next_tick()))
                throw std::runtime_error("invalid forward/replay output boundary");
            if (!replay && match.next_tick()%30==0) {
                std::printf("network tick=%u hash=%08x\n",match.next_tick(),simulation.state_hash()); std::fflush(stdout);
            }
        }
        if (step==GbaNetplayMatch::Step::Idle) pacer.idle();
    }
    std::string error;
    if (!simulation.load_state(match.checkpoint().state,&error)) throw std::runtime_error(error);
    std::printf("network agreed tick=%u hash=%08x replay=%llu bytes=%zu\n",frames,simulation.state_hash(),
        static_cast<unsigned long long>(match.replay_ticks()),match.checkpoint().state.size());
    std::printf("network performance forward_fps=%.2f work_fps=%.2f work_frames=%llu\n",
        target_ms>started ? frames*1000.0/(target_ms-started) : 0.0,
        work_ms>0 ? work_frames*1000.0/work_ms : 0.0,static_cast<unsigned long long>(work_frames));
    std::fflush(stdout);
    const auto finish=rbe_mono_ms();
    while (rbe_mono_ms()-finish<1000) { match.poll(); pacer.idle(); }
}
