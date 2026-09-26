#pragma once

#include "multiplayer_checkpoint.h"
#include "multiplayer_startup.h"
#include <memory>

namespace gbarecomp {
struct GbaNetplayMatchOptions {
    RNetConfig network{};
    // Exact application identity: native program/runtime build, verified BIOS,
    // cartridge images and deterministic options. Not a display name.
    std::string identity;
    std::uint32_t build_fingerprint = 0;
    std::int64_t rtc_seed_seconds = 0;
    int prediction = 6;
    bool rollback = true;
    // The caller loaded an agreed paired archive; compare it byte-for-byte
    // instead of exchanging the individual cartridge saves at cold startup.
    bool restored_pair = false;
    GbaNetplayMatchOptions() { rnet_config_init_defaults(&network); }
};

// One application-thread match. The launcher chooses the transport and the
// window owns pacing/input/presentation. poll() never sleeps or waits for a
// peer: it runs at most one native frame (forward OR replay). Continue polling
// during input stalls/reconnects and after checkpoint agreement for receipts.
// Do not run another rollback driver concurrently in this process: the pinned
// shared driver uses a process-global admission scheduler.
class GbaNetplayMatch {
public:
    enum class Phase { Starting, Running, AgreeingCheckpoint, CheckpointReady, Failed };
    enum class Step { Idle, Forward, Replay };
    GbaNetplayMatch(GbaMultiplayerSession&, GbaNetplayMatchOptions);
    ~GbaNetplayMatch();
    GbaNetplayMatch(const GbaNetplayMatch&) = delete;
    GbaNetplayMatch& operator=(const GbaNetplayMatch&) = delete;

    // Start LAN / the shared lobby's transport on this handle before poll().
    // The match owns it; it stays valid until destruction, including failure.
    RNetSession* transport() const { return network_.get(); }
    std::function<std::uint16_t(std::uint32_t)> sample_local;
    Step poll();
    bool take_output(GbaNetplayOutput&);
    Phase phase() const { return phase_; }
    GbaConnectionStatus connection() const { return connection_; }
    const std::string& error() const { return error_; }
    std::uint32_t next_tick() const { return host_.next_tick(); }
    std::uint64_t replay_ticks() const;

    // Both peers must agree this boundary through application session control
    // before calling. This is NOT a unilateral save/disconnect request. Keeps
    // running until the target's inputs confirm, drains the driver, then
    // independently agrees the exact whole-session snapshot with the peer.
    void finish_at(std::uint32_t next_tick);
    const GbaConfirmedCheckpoint& checkpoint() const;
    bool store_checkpoint(const std::filesystem::path&, std::string* error) const;
private:
    void start_driver();
    void fail(std::string);
    GbaMultiplayerSession& simulation_;
    GbaNetplayMatchOptions options_;
    GbaNetplayHost host_;
    std::unique_ptr<RNetSession,decltype(&rnet_session_destroy)> network_{nullptr,rnet_session_destroy};
    RNetSession* session_ = nullptr; // stable storage for the shared driver's ABI
    std::unique_ptr<RNetRbDriver,decltype(&rnet_rb_driver_destroy)> driver_{nullptr,rnet_rb_driver_destroy};
    std::unique_ptr<GbaNetplayStartup> startup_;
    std::unique_ptr<GbaNetplayCheckpointAgreement> agreement_;
    Phase phase_ = Phase::Starting;
    GbaConnectionStatus connection_{GbaConnectionPhase::Connecting,60000};
    std::string error_;
    int seat_ = 0, slots_ = 2, delay_ = 2, prediction_ = 6;
    std::uint32_t finish_tick_ = 0;
    std::uint64_t replay_ticks_ = 0;
};
}
