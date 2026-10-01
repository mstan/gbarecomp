#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gbarecomp {
// IDs name machines in a session, independently of controller seats, cable
// positions, RFU parent/child slots, or Union Room discovery entries.
using GbaMachineId = std::uint32_t;
// Current same-cartridge session ceiling: the four-port multiplayer cable.
inline constexpr std::size_t kGbaMaxSessionPlayers = 4;
enum class GbaBootSource : std::uint8_t { Cartridge, Multiboot };
enum class GbaLinkMedium : std::uint8_t { Cable, Wireless };

struct GbaMachineDescriptor {
    GbaMachineId id = 0;
    std::string program_id;
    std::string rom_sha1;
    GbaBootSource boot = GbaBootSource::Cartridge;
};
struct GbaLinkGroup {
    GbaLinkMedium medium = GbaLinkMedium::Cable;
    // Ordered cable positions, or (future) members of a wireless radio domain.
    // An RFU radio domain may contain several independent parent/child groups.
    std::vector<GbaMachineId> machines;
};
struct GbaSessionConfig {
    std::vector<GbaMachineDescriptor> machines;
    std::vector<GbaLinkGroup> links;
    // Ordered network/controller seats -> stable machine identities.
    std::vector<GbaMachineId> input_machines;
};

// Structural validation is deliberately independent of feature availability.
// RFU has 4 child links per parent, not a 4/5/8-machine global session limit.
bool validate_session_config(const GbaSessionConfig&, std::string* error);
// Current launch gate. Future hardware can extend capabilities independently
// from the shape of the manifest or the replicated snapshot container.
bool validate_cable_mvp(const GbaSessionConfig&, std::string* error);
bool validate_multiplayer_mvp(const GbaSessionConfig&, std::string* error);
} // namespace gbarecomp
