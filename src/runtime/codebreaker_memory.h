#pragma once
#include "codebreaker.h"
#include "cartridge_mod_state.h"
#include <memory>

namespace gbarecomp::codebreaker {
// Owns one frozen execution plan. Cartridge and RAM operations are evaluated
// against a shadow view, then committed together through recomp-mod-core.
class FrameMemory {
public:
    FrameMemory();
    ~FrameMemory();
    FrameMemory(const FrameMemory&) = delete;
    FrameMemory& operator=(const FrameMemory&) = delete;
    bool initialize(std::vector<NamedProgram> programs, Memory bus,
                    std::span<std::uint8_t> image,
                    const RuntimeNativeImageDependencies* dependencies,
                    const std::string& game_id, CartridgeModState::Identity original,
                    std::string* error);
    bool run(std::string* error);
    // The normal runtime binds once before any save/load. Callbacks resolve a
    // live session pointer; destruction clears it before the image is freed.
    bool bind_snapshots(std::string* error);
    CartridgeModState* cartridge_state();
    const std::string& plan_fingerprint() const;
    bool changed() const;
    static bool uses_cartridge(const Program&);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace gbarecomp::codebreaker
