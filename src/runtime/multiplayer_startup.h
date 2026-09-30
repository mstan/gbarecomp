#pragma once
#include "multiplayer_session.h"
#include <recomp_net/session.h>

namespace gbarecomp {
// Startup application barrier above recomp-net's reliable STATE transfer, for
// two to four seats. Every peer builds the same deterministic cold system
// locally (ROM/BIOS/native code never leave the process). Each seat
// contributes only its own save and civil RTC seed ("owner packet"):
//   1. every guest uploads its packet to seat 0 (MEMCARD, concurrently);
//   2. seat 0 broadcasts the ASSEMBLY of all packets in seat order (BOOT) --
//      it doubles as each upload's receipt, and every guest checks its own
//      packet byte-for-byte;
//   3. every peer applies every packet to its machine; seat 0 then runs an
//      N-party ready probe on the assembled state's digest.
// Caller supplies an identity covering runtime/build/BIOS/mods and must
// verify all local image bytes before constructing the simulation. Call on
// the simulation thread before constructing/starting a rollback driver.
class GbaNetplayStartup {
public:
    enum class Status { Waiting, Ready, Failed };
    GbaNetplayStartup(GbaMultiplayerSession&, RNetSession*, unsigned local_seat,
                      std::string identity, std::int64_t rtc_seed_seconds);
    Status poll(std::uint64_t monotonic_ms);
    const std::string& error() const { return error_; }
private:
    enum class Phase { Upload, ReceiveUploads, SendAssembly, ReceiveAssembly, SendReady, ReceiveReady, Done, Failed };
    GbaMultiplayerSession& simulation_;
    RNetSession* network_;
    unsigned seat_, seats_;
    std::string identity_, error_;
    Phase phase_;
    std::vector<std::vector<std::uint8_t>> packets_; // by seat; host collects, guest keeps its own
    std::vector<std::uint8_t> assembly_;
    bool sending_=false, started_=false;
    std::uint64_t started_ms_=0;
    void apply_assembly(std::span<const std::uint8_t>);
    void finish();
};
// "player N" list of seats in `mask`, for diagnostics.
std::string gba_netplay_seat_names(std::uint32_t mask);
}
