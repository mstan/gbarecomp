#pragma once

#include <filesystem>

namespace gbarecomp {

// The title owns its targeted visible manifest. The shared engine binds this
// host-only feature to the existing diagnostic recorder selection API.
inline constexpr const char* kDiagnosticReplayPackage =
    "gbarecomp.feature.gameplay-diagnostics";
inline constexpr const char* kDiagnosticReplayFeature = "record-gameplay";

// Read state only: never scan a catalog or activate gameplay mods, including
// when a host/netplay path needs the preference without an offline mod plan.
int mod_runtime_saved_diagnostic_replay_enabled(
    const std::filesystem::path& root);

}  // namespace gbarecomp
