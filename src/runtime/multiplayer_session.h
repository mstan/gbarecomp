#pragma once

#include "multiplayer_config.h"
#include "runtime_bus_bridge.h"
#include "runtime_context.h"
#include "gba_bus.h"
#include "gba_ppu.h"
#include "gba_link_hub.h"
#include "gba_wireless.h"
#include <memory>
#include <span>

namespace gbarecomp {
struct GbaInstance {
    GbaMachineDescriptor descriptor;
    RuntimeArmContext execution;
    RuntimeTimingContext timing;
    gba::GbaBus bus;
    gba::GbaPpu ppu;
    explicit GbaInstance(GbaMachineDescriptor descriptor);
};

// Conservative local-link qualification backend. It uses recompiled dispatch
// with per-instruction suspension; batching must pass this same scheduler's
// traces before replacing it. No network/presentation/filesystem operations.
class GbaMultiplayerSession {
public:
    explicit GbaMultiplayerSession(GbaSessionConfig config);
    ~GbaMultiplayerSession();
    GbaMultiplayerSession(const GbaMultiplayerSession&) = delete;
    GbaMultiplayerSession& operator=(const GbaMultiplayerSession&) = delete;
    GbaInstance& machine(GbaMachineId);
    const GbaInstance& machine(GbaMachineId) const;
    void run_frame(std::span<const std::uint16_t> buttons);
    void run_until(std::uint64_t cycle);
    std::vector<std::uint8_t> save_state() const;
    // Successful load atomically replaces machines; reacquire machine(id)
    // afterwards. Failure leaves all live state and wiring untouched.
    bool load_state(std::span<const std::uint8_t>, std::string* error);
    std::uint32_t state_hash() const;
    std::array<std::uint32_t,3> state_hash_parts() const;
    void discard_audio_output();
    std::size_t input_count() const { return config_.input_machines.size(); }
    GbaInstance& input_machine(std::size_t seat) { return machine(config_.input_machines.at(seat)); }
    // Experimental event-bounded native batching; reference mode stays
    // available for differential qualification. Does not change saved state.
    void set_native_slices(bool enabled) { native_slices_ = enabled; }
    // Differential/performance reference: keep exception-based suspension
    // while retaining the same slice and device boundaries.
    void set_return_yields(bool enabled) { return_yields_ = enabled; }
    std::uint64_t cycle() const { return cycle_; }
    std::size_t machine_count() const { return machines_.size(); }
    gba::GbaLinkHub& cable();
private:
    void bind(GbaInstance&);
    void capture(GbaInstance&);
    GbaSessionConfig config_;
    std::vector<std::unique_ptr<GbaInstance>> machines_;
    std::unique_ptr<gba::GbaLinkHub> cable_;
    std::unique_ptr<gba::GbaWirelessDomain> wireless_;
    std::vector<std::uint8_t> link_state() const;
    std::uint64_t cycle_ = 0;
    bool native_slices_ = false;
    bool return_yields_ = true;
};
} // namespace gbarecomp
