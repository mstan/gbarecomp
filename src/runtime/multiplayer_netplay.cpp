#include "multiplayer_netplay.h"
#include <retcomm_rbengine/mono_ms.h>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <limits>

namespace gbarecomp {
GbaConnectionStatus gba_netplay_connection_status(const RNetSession* session) {
    constexpr std::uint32_t grace=60000, show_after=2000;
    if (!session) return {GbaConnectionPhase::Connecting,grace};
    if (rnet_session_peer_disconnected(session,0)) return {GbaConnectionPhase::PeerLeft,0};
    if (!rnet_session_is_running(session)) return {GbaConnectionPhase::Connecting,grace};
    RNetSessionStats stats{}; rnet_session_get_stats(session,&stats);
    if (stats.last_peer_rx_age_ms>=grace) return {GbaConnectionPhase::TimedOut,0};
    return {stats.last_peer_rx_age_ms>=show_after ? GbaConnectionPhase::Reconnecting : GbaConnectionPhase::Connected,
        grace-static_cast<std::uint32_t>(stats.last_peer_rx_age_ms)};
}
namespace {
GbaNetplayHost& host(void* p) { return *static_cast<GbaNetplayHost*>(p); }
void sanitize(RNetRbFrame& row) {
    row.buttons &= 0x3ff;
    row.stick_x = row.stick_y = 0;
    row.analog = 0;
}
bool decode(const RNetInputSample& sample, RNetRbFrame& row) {
    if (!sample.valid) return false;
    // recomp-net seeds its delay prefix and empty seats with size-zero neutral.
    if (sample.size == 0) { row.buttons=0; sanitize(row); return true; }
    if (sample.size != 2 || (sample.bytes[1] & 0xfc)) return false;
    row.buttons = sample.bytes[0] | (sample.bytes[1] << 8);
    sanitize(row);
    return true;
}
}
GbaNetplayHost::GbaNetplayHost(GbaMultiplayerSession& simulation)
    : simulation_(simulation), inputs_(simulation.input_count()) {
    if (inputs_.size() > RNET_RB_MAX_SLOTS)
        throw std::invalid_argument("session exceeds this recomp-net version's seat capacity");
    confirmed_.state=simulation_.save_state(); // agreed cold state, before tick zero
    snapshots_ = rbe_snap_ring_create(kSnapshotDepth);
    if (!snapshots_) throw std::bad_alloc();
}
GbaNetplayHost::~GbaNetplayHost() { rbe_snap_ring_destroy(snapshots_); }
int GbaNetplayHost::save_snapshot(std::uint32_t tick) {
    auto v=snapshot_callbacks();
    if (!rbe_snap_ring_save(snapshots_,tick,&v)) return 0;
    try {
        std::erase_if(snapshot_ticks_,[&](auto t) { return !rbe_snap_ring_has(snapshots_,t); });
        if (std::find(snapshot_ticks_.begin(),snapshot_ticks_.end(),tick)==snapshot_ticks_.end())
            snapshot_ticks_.push_back(tick);
        return 1;
    } catch (const std::exception& e) { fail(e.what()); return 0; }
}
bool GbaNetplayHost::retain_confirmed(std::uint32_t through) {
    if (through==std::numeric_limits<std::uint32_t>::max() || replaying_ || published_) return false;
    if (through>=next_tick_) { fail("confirmation is ahead of the simulation"); return false; }
    const auto boundary=through+1;
    std::uint32_t best=confirmed_.next_tick;
    for (auto tick:snapshot_ticks_)
        if (tick>best && tick<=boundary && rbe_snap_ring_has(snapshots_,tick)) best=tick;
    try {
        if (next_tick_==boundary && next_tick_>best) {
            auto state=simulation_.save_state();
            confirmed_={next_tick_,std::move(state)};
            return true;
        }
        if (best<=confirmed_.next_tick) return false;
        std::size_t size=0;
        const auto* data=rbe_snap_ring_peek(snapshots_,best,&size);
        if (!data) throw std::logic_error("confirmed snapshot disappeared");
        std::vector<std::uint8_t> state(data,data+size);
        confirmed_={best,std::move(state)};
        return true;
    } catch (const std::exception& e) { fail(e.what()); return false; }
}
bool GbaNetplayHost::restore_confirmed_for_restart() {
    std::string error;
    if (!simulation_.load_state(confirmed_.state,&error)) { fail(error.c_str()); return false; }
    simulation_.discard_audio_output();
    begin_match();
    return true;
}
void GbaNetplayHost::begin_match() {
    auto state=simulation_.save_state();
    next_tick_=0; published_=replaying_=lobby_requested_=false;
    error_.clear(); snapshot_ticks_.clear(); rbe_snap_ring_clear(snapshots_);
    confirmed_={0,std::move(state)};
    std::fill(inputs_.begin(),inputs_.end(),0);
}
void GbaNetplayHost::fail(const char* message) {
    if (error_.empty()) error_ = message;
    lobby_requested_ = true;
}
RbeSnapVTable GbaNetplayHost::snapshot_callbacks() { return {this,serialize,deserialize}; }
int GbaNetplayHost::serialize(void* ctx,std::uint32_t tick,std::uint8_t** out,std::size_t* size) {
    auto& h = host(ctx);
    try {
        if (tick != h.next_tick_) throw std::logic_error("snapshot tick is not the simulation boundary");
        auto state = h.simulation_.save_state();
        auto* data = static_cast<std::uint8_t*>(std::malloc(state.size()));
        if (!data) return 0;
        std::memcpy(data,state.data(),state.size()); *out = data; *size = state.size();
        return 1;
    } catch (const std::exception& e) { h.fail(e.what()); return 0; }
}
int GbaNetplayHost::deserialize(void* ctx,std::uint32_t tick,const std::uint8_t* data,std::size_t size) {
    auto& h = host(ctx);
    std::string error;
    if (!h.simulation_.load_state({data,size},&error)) { h.fail(error.c_str()); return 0; }
    h.next_tick_ = tick; h.published_ = false;
    h.simulation_.discard_audio_output();
    return 1;
}
void GbaNetplayHost::publish(std::uint32_t tick,const RNetRbFrame* rows,int slots,bool replay) {
    if (tick != next_tick_ || slots != static_cast<int>(inputs_.size()) || !rows || published_ ||
        replay != replaying_) { fail("invalid netplay input publication boundary"); return; }
    for (int i = 0; i < slots; ++i) {
        if (!rows[i].is_valid) { fail("missing netplay input seat"); return; }
        inputs_[i] = rows[i].buttons & 0x3ff;
    }
    published_ = true;
}
bool GbaNetplayHost::run_published_tick() {
    if (!published_ || lobby_requested_) return false;
    try {
        simulation_.run_frame(inputs_);
        if (replaying_) simulation_.discard_audio_output();
        ++next_tick_; published_ = false;
        return true;
    } catch (const std::exception& e) { fail(e.what()); return false; }
}
RNetRbHost GbaNetplayHost::callbacks() {
    RNetRbHost result{}; result.ctx = this;
    result.snap_save = [](void* c,std::uint32_t t) { return host(c).save_snapshot(t); };
    result.snap_load = [](void* c,std::uint32_t t) { auto& h=host(c); auto v=h.snapshot_callbacks(); return rbe_snap_ring_load(h.snapshots_,t,&v); };
    result.snap_has = [](void* c,std::uint32_t t) { return rbe_snap_ring_has(host(c).snapshots_,t); };
    result.snap_oldest = [](void* c,std::uint32_t* t) {
        auto* ring=host(c).snapshots_; if (!rbe_snap_ring_count(ring)) return 0;
        *t=rbe_snap_ring_oldest_tick(ring); return 1;
    };
    result.snap_drop_after = [](void* c,std::uint32_t t) {
        auto& h=host(c); rbe_snap_ring_drop_after(h.snapshots_,t);
        std::erase_if(h.snapshot_ticks_,[&](auto tick) { return tick>t; });
    };
    result.publish = [](void* c,std::uint32_t t,const RNetRbFrame* r,int n,int replay) { host(c).publish(t,r,n,replay!=0); };
    result.run_tick = [](void* c,std::uint32_t t) -> int {
        auto& h=host(c); return t == h.next_tick_ && h.run_published_tick();
    };
    result.resim_begin = [](void* c) { auto& h=host(c); h.replaying_=true; h.simulation_.discard_audio_output(); };
    result.resim_end = [](void* c) { auto& h=host(c); h.simulation_.discard_audio_output(); h.replaying_=false; };
    result.digest_master = [](void* c) -> std::uint32_t {
        try { return host(c).simulation_.state_hash(); }
        catch (const std::exception& e) { host(c).fail(e.what()); return 0; }
    };
    result.digest_parts = [](void* c,RNetRbDigestParts* p) {
        *p = {};
        try { auto& s=host(c).simulation_; p->master=s.state_hash(); auto parts=s.state_hash_parts();
              for (unsigned i=0;i<3;++i) p->part[i]=parts[i]; }
        catch (const std::exception& e) { host(c).fail(e.what()); }
    };
    result.decode_sample = [](void* c,int,const RNetInputSample* s,RNetRbFrame* r) {
        if (!decode(*s,*r)) host(c).fail("invalid GBA controller packet");
    };
    result.sanitize_row = [](void*,int,RNetRbFrame* r) { sanitize(*r); };
    result.neutral_row = [](void*,int,RNetRbFrame* r) { r->buttons=0; sanitize(*r); };
    result.request_return_to_lobby = [](void* c) { host(c).lobby_requested_=true; };
    result.now_ms = [](void*) -> std::uint32_t { return rbe_mono_ms(); };
    return result;
}
RNetHostVTable GbaNetplayHost::delay_callbacks() {
    RNetHostVTable result{}; result.ctx=this;
    result.sample_local = [](rnet_u32 tick,RNetInputSample* s,void* c) {
        auto& h=host(c); *s={}; s->tick=tick; s->size=2; s->valid=1;
        try { const auto buttons=h.sample_local ? h.sample_local(tick)&0x3ff : 0;
              s->bytes[0]=buttons&0xff; s->bytes[1]=buttons>>8; }
        catch (const std::exception& e) { h.fail(e.what()); }
    };
    result.publish = [](rnet_u32 tick,const RNetInputSample* samples,int slots,void* c) {
        auto& h=host(c);
        if (slots < 1 || slots > RNET_RB_MAX_SLOTS) { h.fail("invalid GBA seat count"); return; }
        RNetRbFrame rows[RNET_RB_MAX_SLOTS]{};
        for (int i=0;i<slots;++i) {
            rows[i].tick=tick; rows[i].is_valid=1;
            if (!decode(samples[i],rows[i])) { h.fail("invalid GBA controller packet"); return; }
        }
        h.publish(tick,rows,slots,false);
    };
    return result;
}
bool GbaNetplayHost::try_delay_frame(RNetSession* session) {
    if (!session || lobby_requested_ || replaying_) return false;
    rnet_session_pump(session);
    if (!rnet_session_try_admit(session,next_tick_)) return false;
    if (!run_published_tick()) return false;
    rnet_session_advance(session);
    return true;
}
} // namespace gbarecomp
