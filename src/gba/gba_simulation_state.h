#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace gba {
class GbaBus;
class GbaPpu;
template<bool Reading> class SimulationArchive;
// Canonical, pointer-free device payload used by multiplayer snapshots.
// Unlike legacy user savestates this includes all simulation residue and
// omits consumed host audio. load_device_state is for a staging instance:
// the session validates all staged machines before replacing any live state.
std::vector<std::uint8_t> save_device_state(const GbaBus&, const GbaPpu&);
// Append live device bytes without retaining a second, temporary payload.
void append_device_state(SimulationArchive<false>&, const GbaBus&, const GbaPpu&);
bool load_device_state(GbaBus&, GbaPpu&, std::span<const std::uint8_t>);
} // namespace gba
