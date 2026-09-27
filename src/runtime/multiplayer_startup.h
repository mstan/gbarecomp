#pragma once
#include "multiplayer_session.h"
#include <recomp_net/session.h>

namespace gbarecomp {
// Startup application barrier above recomp-net's reliable STATE transfer.
// Two seats initially. Each contributes its own save and civil RTC seed; the
// host assembles the replicated system. ROM/BIOS/native code never leave the
// process. Caller supplies an identity covering runtime/build/BIOS/mods and
// must verify all local image bytes before constructing the simulation.
// Call on the simulation thread before constructing/starting a rollback driver.
class GbaNetplayStartup {
public:
    enum class Status { Waiting, Ready, Failed };
    GbaNetplayStartup(GbaMultiplayerSession&, RNetSession*, unsigned local_seat,
                      std::string identity, std::int64_t rtc_seed_seconds);
    Status poll(std::uint64_t monotonic_ms);
    const std::string& error() const { return error_; }
private:
    enum class Phase { Upload, ReceiveUpload, SendBoot, ReceiveBoot, SendReady, ReceiveReady, Done, Failed };
    GbaMultiplayerSession& simulation_;
    RNetSession* network_;
    unsigned seat_;
    std::string identity_, error_;
    Phase phase_;
    std::vector<std::uint8_t> owned_, boot_;
    std::array<std::uint8_t,20> guest_receipt_{};
    bool sending_=false, started_=false;
    std::uint64_t started_ms_=0;
    void finish();
};
}
