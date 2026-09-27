#include "multiplayer_config.h"
#include <algorithm>
#include <unordered_set>

namespace gbarecomp {
namespace {
bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}
bool sha1(const std::string& value) {
    return value.size() == 40 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
}
bool validate_session_config(const GbaSessionConfig& config, std::string* error) {
    if (config.machines.empty()) return fail(error, "session has no machines");
    std::unordered_set<GbaMachineId> ids;
    for (const auto& machine : config.machines) {
        if (!ids.insert(machine.id).second) return fail(error, "duplicate machine ID");
        if (machine.program_id.empty()) return fail(error, "missing program identity");
        if (machine.boot != GbaBootSource::Cartridge && machine.boot != GbaBootSource::Multiboot)
            return fail(error, "unknown boot source");
        if (machine.boot == GbaBootSource::Cartridge && !sha1(machine.rom_sha1))
            return fail(error, "invalid cartridge identity");
    }
    std::unordered_set<GbaMachineId> attached;
    for (const auto& link : config.links) {
        if (link.medium != GbaLinkMedium::Cable && link.medium != GbaLinkMedium::Wireless)
            return fail(error, "unknown link medium");
        if (link.machines.size() < 2) return fail(error, "link needs at least two machines");
        if (link.medium == GbaLinkMedium::Cable && link.machines.size() > 4)
            return fail(error, "a GBA cable has at most four ports");
        for (auto id : link.machines) {
            if (!ids.contains(id)) return fail(error, "link names a missing machine");
            if (!attached.insert(id).second) return fail(error, "serial port attached twice");
        }
    }
    std::unordered_set<GbaMachineId> controlled;
    for (auto id : config.input_machines) {
        if (!ids.contains(id)) return fail(error, "input seat names a missing machine");
        if (!controlled.insert(id).second) return fail(error, "machine has duplicate input seats");
    }
    return true;
}
bool validate_multiplayer_mvp(const GbaSessionConfig& config, std::string* error) {
    if (!validate_session_config(config, error)) return false;
    if (config.machines.size() != 2 || config.input_machines.size() != 2 || config.links.size() != 1)
        return fail(error, "initial multiplayer session requires two machines and two input seats");
    if (config.links[0].machines.size() != 2)
        return fail(error, "both machines must share the selected serial medium");
    for (const auto& machine : config.machines)
        if (machine.boot != GbaBootSource::Cartridge)
            return fail(error, "Single-Pak multiboot is not implemented");
    if (config.machines[0].program_id != config.machines[1].program_id ||
        config.machines[0].rom_sha1 != config.machines[1].rom_sha1)
        return fail(error, "cross-version cartridge linking is not implemented");
    return true;
}
bool validate_cable_mvp(const GbaSessionConfig& config, std::string* error) {
    if (!validate_multiplayer_mvp(config,error)) return false;
    if (config.links[0].medium!=GbaLinkMedium::Cable)
        return fail(error,"session is not a link cable");
    return true;
}
} // namespace gbarecomp
