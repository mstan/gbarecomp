// mod_state.h -- trusted native mod-state registry and durable slot sidecars.
//
// This is deliberately host state.  Providers must never reserve or write
// guest save padding: game save ownership remains entirely with the cartridge.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "snapshot.h"

namespace gbarecomp::debug {

// A provider is native code linked by the game, not data supplied by a mod
// package.  `preflight` must not mutate either guest or provider state.
struct ModStateProvider {
    const char* id = nullptr;       // copied at registration; stable syntax below
    uint32_t schema = 0;            // bump when this provider payload changes
    void* user = nullptr;
    bool (*save)(void* user, SnapshotWriter& out, std::string* error) = nullptr;
    bool (*preflight)(void* user, SnapshotReader& in, std::string* error) = nullptr;
    // Called only after an identical payload passed preflight.  It must be
    // infallible and consume the complete payload; a violation aborts because
    // guest state has already been restored at that point.
    void (*restore)(void* user, SnapshotReader& in) = nullptr;
};

// A bounded, explicit catalog.  Registration is expected during startup,
// before save/load becomes available; duplicate IDs and invalid callbacks fail.
class ModStateRegistry {
public:
    static constexpr std::size_t kMaxProviders = 32;
    static constexpr std::size_t kMaxProviderPayload = 4 * 1024 * 1024;

    bool register_provider(const ModStateProvider& provider, std::string* error = nullptr);
    std::size_t size() const { return providers_.size(); }
    const ModStateProvider& provider_at(std::size_t index) const {
        return providers_[index].callbacks;
    }
    const std::string& provider_id_at(std::size_t index) const {
        return providers_[index].id;
    }

    // MODS payload framing.  `preflight` checks the complete catalog and every
    // provider payload without calling restore.  It is safe to call before any
    // guest snapshot section is deserialized.
    bool serialize(SnapshotWriter& out, std::string* error = nullptr) const;
    bool preflight(SnapshotReader& in, std::string* error = nullptr) const;
    bool restore(SnapshotReader& in, std::string* error = nullptr) const;

private:
    struct Entry {
        ModStateProvider callbacks;
        std::string id;  // owns the caller's ID; callbacks.id is never retained
    };
    std::vector<Entry> providers_;
    mutable bool frozen_ = false;
};

// The process-wide catalog used by the normal runtime's F-key and TCP
// savestates.  Games register their linked, trusted providers during startup,
// before their first snapshot operation; the first save/load freezes it.
ModStateRegistry& global_mod_state_registry();
bool register_mod_state_provider(const ModStateProvider& provider,
                                 std::string* error = nullptr);

// Canonical slot keys lower-case ASCII and accept only [a-z0-9][a-z0-9_-]{0,47}.
// Invalid/path-like keys are rejected instead of being repaired ambiguously.
bool canonical_mod_slot_key(const std::string& input, std::string* output,
                            std::string* error = nullptr);

// Store an opaque native provider-state payload outside the guest save.  The
// envelope carries an exact identity string (normally ROM + mod catalog) and a
// CRC32.  Writes use <slot>.tmp and <slot>.bak in `directory`, then rename the
// temporary file into place.  Reads transparently fall back to .bak when the
// primary is missing or corrupt; an identity mismatch is never bypassed.
bool write_mod_state_slot(const std::filesystem::path& directory,
                          const std::string& slot_key,
                          const std::string& identity,
                          const std::vector<uint8_t>& payload,
                          std::string* error = nullptr);
bool read_mod_state_slot(const std::filesystem::path& directory,
                         const std::string& slot_key,
                         const std::string& expected_identity,
                         std::vector<uint8_t>* payload,
                         std::string* error = nullptr);

}  // namespace gbarecomp::debug
