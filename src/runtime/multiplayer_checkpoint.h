#pragma once
#include "multiplayer_netplay.h"
#include <filesystem>

namespace gbarecomp {
// An application barrier. Each peer must independently hold the same exact confirmed
// snapshot; no host snapshot is accepted to paper over a desync. Only metadata
// and receipts travel over recomp-net. Seat zero first waits for both peers
// to reach the boundary: receiving a STATE transfer stalls input admission.
class GbaNetplayCheckpointAgreement {
public:
    enum class Status { Waiting, Ready, Failed };
    GbaNetplayCheckpointAgreement(const GbaNetplayHost&, RNetSession*, unsigned seat,
        std::string identity, std::uint32_t confirmed_through,
        std::uint32_t proposed_next_tick=0,bool wait_for_confirmation=false);
    // Live agreement keeps rollback running until BOTH peers have accepted
    // the boundary. Supply its current watermark before each poll.
    void update_confirmation(std::uint32_t through) { through_=through; }
    Status poll(std::uint64_t monotonic_ms);
    const std::string& error() const { return error_; }
    // Unavailable until both peers have explicitly acknowledged the exact
    // identity, boundary and full state digest. Still no filesystem effects.
    const GbaConfirmedCheckpoint& checkpoint() const;
    // A single paired archive for app-owned durable storage/recovery. It
    // contains both cartridges' save hardware, not separate speculative .sav
    // exports. The app must write it atomically and recover it as a pair.
    std::vector<std::uint8_t> archive() const;
private:
    enum class Phase { SendArrival, SendProposal, ReceiveProposal, SendReceipt, ReceiveReceipt,
                       SendReady, ReceiveReady, Done, Failed };
    const GbaNetplayHost& host_;
    RNetSession* network_;
    unsigned seat_;
    std::string identity_,error_;
    std::uint32_t through_;
    std::uint32_t proposed_tick_=0;
    bool wait_for_confirmation_=false;
    Phase phase_;
    bool sending_=false,started_=false,arrived_=false;
    std::uint64_t started_ms_=0;
    GbaConfirmedCheckpoint checkpoint_;
    std::vector<std::uint8_t> proposal_;
    std::uint32_t ready_hash_=0;
};

// Bounded decode and digest validation; does not mutate the live session.
// Caller verifies its image/build identity, then session.load_state validates
// the complete deterministic state before installing it atomically.
bool gba_decode_checkpoint_archive(std::span<const std::uint8_t>, const std::string& identity,
                                   GbaConfirmedCheckpoint&, std::string* error);
// Write one complete agreed pair via an exclusive temporary file in the same
// directory, flush, then replace atomically. Never exports independent saves.
// Paths are application-owned; this layer neither chooses nor overwrites a
// player's ordinary single-player save path. Parent directory must exist.
bool gba_store_agreed_checkpoint(const GbaNetplayCheckpointAgreement&, const std::filesystem::path&,
                                 std::string* error);
bool gba_load_checkpoint_archive(const std::filesystem::path&, const std::string& identity,
                                 GbaConfirmedCheckpoint&, std::string* error);
}
