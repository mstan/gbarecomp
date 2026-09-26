#pragma once
#include "save_config.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gba { class GbaBios; }
namespace gbarecomp {
struct GbaInstance;
struct GbaNetplayLaunch;

// The ordinary runtime resolves and verifies assets once, then hands them to
// the optional netplay runner before activating mods or single-player saves.
// All borrowed image memory outlives the synchronous run call.
struct GbaNetplayBoot {
    const gba::GbaBios* bios = nullptr;
    const std::vector<std::uint8_t>* rom = nullptr;
    std::string expected_rom_sha1, title, screen;
    SaveConfiguration save;
    std::filesystem::path local_save, config_directory;
    std::filesystem::path rom_path, bios_path;
    int scale=3, fullscreen=0, volume=100;
    std::uint32_t finish_tick=0; // explicit --frames, must agree at startup
    bool linear_filter=false, sharp_filter=false, show_fps=false;
};

// Launcher-neutral data: shared UI types stay in launcher_seam.h, networking
// types stay in the adapter. Seats name controllers, not cable positions.
struct GbaNetplayLaunch {
    bool enabled=false, rollback=true, force_turn=false;
    unsigned local_seat=0, input_delay=6, prediction=6;
    std::uint32_t session_id=0;
    std::array<unsigned,2> seat_machine{0,1};
    std::string bind_endpoint, peer_endpoint;
    std::filesystem::path checkpoint_path;
    std::filesystem::path resume_path;
    std::string program_id, build_identity, content_sha256;
    void (*setup_instance)(GbaInstance&)=nullptr;
    std::function<void()> pump_lobby;
    // Installed by make_gba_netplay_launch. This dependency boundary keeps
    // single-player games free of a recomp-net link requirement.
    int (*run)(const GbaNetplayLaunch&,const GbaNetplayBoot&)=nullptr;
};

// build_identity must fingerprint the native game + runtime sources/options;
// display/version names alone are not a compatibility identity.
std::shared_ptr<GbaNetplayLaunch> make_gba_netplay_launch(
    std::string program_id,std::string build_identity,std::string content_sha256,
    void (*setup_instance)(GbaInstance&)=nullptr);
// Shared by the UI adapter and runner. Throws on unsupported/malformed policy.
void validate_gba_netplay_launch(const GbaNetplayLaunch&);
// Optional direct-IP entry; strips only --netplay-* options. Both endpoints
// choose the same nonzero session ID. All validation precedes runtime startup.
void parse_gba_netplay_arguments(std::vector<std::string>&,GbaNetplayLaunch&);
}
