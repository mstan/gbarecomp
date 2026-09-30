#pragma once
#include "multiplayer_session.h"
#include <recomp_net/rb_driver.h>
#include <retcomm_rbengine/snap_ring.h>
#include <functional>
#include <optional>

namespace gbarecomp {
enum class GbaConnectionPhase { Connecting, Connected, Reconnecting, TimedOut, PeerLeft };
struct GbaConnectionStatus {
    GbaConnectionPhase phase;
    std::uint32_t grace_remaining_ms;
};
// Host policy only: it never advances guest clocks or changes cable state.
// Pump the existing transport/rollback driver during Reconnecting; do not tear
// down an intact session merely because its latest input has not arrived.
GbaConnectionStatus gba_netplay_connection_status(const RNetSession*);
struct GbaConfirmedCheckpoint {
    // All inputs before next_tick are confirmed. The snapshot is the entire
    // paired machine/cable system, not independent cartridge files.
    std::uint32_t next_tick = 0;
    std::vector<std::uint8_t> state;
};
// Disposable host output. Never included in snapshots or persistent saves.
struct GbaNetplayOutput {
    std::uint32_t tick = 0;
    GbaMachineId machine = 0;
    std::array<std::uint8_t,gba::GbaPpu::kFramebufferBytes> rgb888{};
    std::vector<std::int16_t> audio;
};
// Engine callbacks for the shared episode driver. The runner owns transport,
// lobby/startup agreement, and the driver lifetime. No serial data crosses this
// interface. Construct after agreed ROMs, individual saves and RTC seeds load.
class GbaNetplayHost {
public:
    // Rollback reach in ticks (the driver snapshots every tick by default).
    // NOT derivable from delay+prediction: digest-detected mismatches load
    // well behind the tip (measured: a 26-tick rewind at P=6, D=5, 40 ms),
    // and an unreachable load tick silently refuses the correction. Reach is
    // a time budget (~2 s) independent of the machine count; memory is
    // depth x snapshot size (2.54 MB per four-console fixture snapshot).
    static constexpr std::uint32_t kSnapshotDepth = 120;
    static constexpr std::uint16_t kCheckpointRequest = 0x400;
    explicit GbaNetplayHost(GbaMultiplayerSession&);
    std::uint32_t snapshot_depth() const { return kSnapshotDepth; }
    // Retain the exact state at this future boundary however far the live
    // simulation runs past it. Replays re-capture it; a load before it drops
    // the copy until the corrected run reaches it again.
    void pin_checkpoint(std::uint32_t next_tick);
    ~GbaNetplayHost();
    GbaNetplayHost(const GbaNetplayHost&) = delete;
    GbaNetplayHost& operator=(const GbaNetplayHost&) = delete;
    RNetRbHost callbacks();
    RNetHostVTable delay_callbacks();
    // Active-high A/B/Select/Start/Right/Left/Up/Down/R/L, little endian.
    std::function<std::uint16_t(std::uint32_t)> sample_local;
    bool run_published_tick();
    bool try_delay_frame(RNetSession*);
    // Session-control bit carried in input rows, removed before guest KEYINPUT.
    // Only confirmed input history may initiate a paired save-and-leave.
    std::optional<std::uint32_t> checkpoint_request(std::uint32_t confirmed_through) const;
    // Consume the newest forward frame once, selecting the machine through the
    // manifest's input-seat mapping. Replay, stalls and repeated calls expose
    // no output. An unconsumed frame is replaced by the next forward frame;
    // its audio is discarded as well, so catch-up cannot build an old backlog.
    // Call after driver.finish_frame(); do not mutate the simulation directly.
    bool take_output(std::size_t local_seat, GbaNetplayOutput&);
    bool replaying() const { return replaying_; }
    bool return_to_lobby_requested() const { return lobby_requested_; }
    const std::string& error() const { return error_; }
    std::uint32_t next_tick() const { return next_tick_; }
    // Call outside an active episode after finish_frame/pump. At the pinned
    // driver, confirmed_through==0 is ambiguous; wait for a nonzero watermark.
    // UINT32_MAX means no confirmed tick. Keeps the most recent available
    // snapshot at or before that watermark, independently of ring eviction.
    // Delay-sync may supply next_tick()-1 after a successful admitted frame.
    // This is a recovery candidate. Durable persistence/reconnect must also
    // agree its exact tick and digest with peers: driver watermarks can demote
    // on NACK, and quiescence is not a persistence acknowledgement.
    bool retain_confirmed(std::uint32_t confirmed_through);
    const GbaConfirmedCheckpoint& confirmed_checkpoint() const { return confirmed_; }
    // Freeze an exact boundary for peer agreement. Supply the driver's CURRENT
    // confirmation watermark, not a cached high-water value. No replay or
    // published tick; live agreement must revalidate after draining.
    bool copy_checkpoint(std::uint32_t next_tick, std::uint32_t confirmed_through,
                         GbaConfirmedCheckpoint& out) const;
    // Revalidate an already bilaterally accepted candidate after draining.
    // This compares bytes only; it does not certify a new boundary.
    bool checkpoint_unchanged(const GbaConfirmedCheckpoint&) const;
    // Only after the old driver/transport has stopped. A new match must agree
    // on this exact checkpoint and start its input timeline again at zero.
    bool restore_confirmed_for_restart();
    // No active driver; call after bootstrap has installed the agreed state.
    void begin_match();
private:
    GbaMultiplayerSession& simulation_;
    RbeSnapRing* snapshots_ = nullptr;
    // Serialized current state shared by the driver's digest and the next
    // snapshot of the same boundary: one full save_state per tick, not two.
    // Keyed by a mutation generation AND the session clock.
    std::uint64_t generation_ = 0;
    mutable std::uint64_t cached_generation_ = UINT64_MAX, cached_cycle_ = 0;
    mutable std::vector<std::uint8_t> cached_state_;
    mutable std::uint32_t cached_hash_ = 0;
    const std::vector<std::uint8_t>& current_state() const;
    void mutated() { ++generation_; }
    std::uint32_t pinned_tick_ = UINT32_MAX;
    std::vector<std::uint8_t> pinned_state_;
    std::vector<std::uint16_t> inputs_;
    std::uint32_t next_tick_ = 0;
    bool published_ = false, replaying_ = false, lobby_requested_ = false;
    bool output_ready_ = false;
    std::string error_;
    GbaConfirmedCheckpoint confirmed_;
    std::vector<std::uint32_t> snapshot_ticks_;
    std::uint32_t checkpoint_request_tick_=UINT32_MAX;
    bool published_checkpoint_request_=false;
    std::uint32_t with_control_digest(std::uint32_t) const;
    bool copy_state_at(std::uint32_t,std::vector<std::uint8_t>&) const;
    int save_snapshot(std::uint32_t);
    void publish(std::uint32_t,const RNetRbFrame*,int,bool);
    void fail(const char*);
    static int serialize(void*,std::uint32_t,std::uint8_t**,std::size_t*);
    static int deserialize(void*,std::uint32_t,const std::uint8_t*,std::size_t);
    RbeSnapVTable snapshot_callbacks();
};
} // namespace gbarecomp
