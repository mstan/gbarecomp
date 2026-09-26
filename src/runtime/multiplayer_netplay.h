#pragma once
#include "multiplayer_session.h"
#include <recomp_net/rb_driver.h>
#include <retcomm_rbengine/snap_ring.h>
#include <functional>

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
// Engine callbacks for the shared episode driver. The runner owns transport,
// lobby/startup agreement, and the driver lifetime. No serial data crosses this
// interface. Construct after agreed ROMs, individual saves and RTC seeds load.
class GbaNetplayHost {
public:
    static constexpr std::uint32_t kSnapshotDepth = 120;
    explicit GbaNetplayHost(GbaMultiplayerSession&);
    ~GbaNetplayHost();
    GbaNetplayHost(const GbaNetplayHost&) = delete;
    GbaNetplayHost& operator=(const GbaNetplayHost&) = delete;
    RNetRbHost callbacks();
    RNetHostVTable delay_callbacks();
    // Active-high A/B/Select/Start/Right/Left/Up/Down/R/L, little endian.
    std::function<std::uint16_t(std::uint32_t)> sample_local;
    bool run_published_tick();
    bool try_delay_frame(RNetSession*);
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
    // Only after the old driver/transport has stopped. A new match must agree
    // on this exact checkpoint and start its input timeline again at zero.
    bool restore_confirmed_for_restart();
    // No active driver; call after bootstrap has installed the agreed state.
    void begin_match();
private:
    GbaMultiplayerSession& simulation_;
    RbeSnapRing* snapshots_ = nullptr;
    std::vector<std::uint16_t> inputs_;
    std::uint32_t next_tick_ = 0;
    bool published_ = false, replaying_ = false, lobby_requested_ = false;
    std::string error_;
    GbaConfirmedCheckpoint confirmed_;
    std::vector<std::uint32_t> snapshot_ticks_;
    int save_snapshot(std::uint32_t);
    void publish(std::uint32_t,const RNetRbFrame*,int,bool);
    void fail(const char*);
    static int serialize(void*,std::uint32_t,std::uint8_t**,std::size_t*);
    static int deserialize(void*,std::uint32_t,const std::uint8_t*,std::size_t);
    RbeSnapVTable snapshot_callbacks();
};
} // namespace gbarecomp
