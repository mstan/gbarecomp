#pragma once

#include <stdint.h>
#include "../gba/foreign_obj_focus.h"

#ifdef __cplusplus
#include <filesystem>
#include <string>

struct RecompLauncherCModProvider;

namespace gbarecomp {

// Load the package catalog rooted at <exe>/mods for one verified game.
bool mod_runtime_initialize(const std::filesystem::path& root,
                            const std::string& game_id,
                            const std::string& rom_sha1,
                            std::string* error = nullptr);

// Validate and persist the staged feature selections for the selected ROM.
bool mod_runtime_commit(const std::filesystem::path& rom_path = {},
                        std::string* error = nullptr);

// Reset game-owned mod state, then invoke the committed trusted plugins.
void mod_runtime_activate_plugins();

const RecompLauncherCModProvider* mod_runtime_launcher_provider();

// Returns the locally-selected, hash-verified path for an asset declared by
// an enabled package after a successful commit. The pointer is owned by the
// mod runtime and remains valid until its next initialize/commit cycle.
const char* mod_runtime_required_asset_path(const std::string& package_id,
                                            const std::string& asset_id);

}  // namespace gbarecomp
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*GBAModActivationCallback)(void);

int gba_mod_register_activation_plugin(const char* id,
                                       GBAModActivationCallback callback);
int gba_mod_register_reset_callback(GBAModActivationCallback callback);

// Trusted presentation plugins may request the engine's adaptive view. The
// game's RunOptions capability gate remains authoritative.
int gba_mod_set_adaptive_view_enabled(int enabled);
int gba_mod_adaptive_view_enabled(void);

// Publish an immutable native-sized BGR555 foreign background for a plugin
// that is present in the currently committed, hash-validated feature set.
// The pointer is consumed only by the PPU compositor; nullptr is rejected so
// clearing cannot accidentally become authorization. Activation passes clear
// the presentation endpoint before callbacks run.
int gba_mod_publish_foreign_background(const char* plugin_id,
                                       const uint16_t* pixels);
void gba_mod_clear_foreign_background(void);

// Publish a stable, immutable foreign OBJ focus descriptor. The selected
// plugin is authorized exactly as it is for the foreign background. A null,
// incompatible, or out-of-bounds descriptor is rejected; clearing is a
// separate explicit operation and is performed automatically on reactivation.
int gba_mod_publish_foreign_obj_focus(const char* plugin_id,
                                      const GbaForeignObjFocusTransform* focus);
void gba_mod_clear_foreign_obj_focus(void);

// Trusted game plugins use this after their package has committed. It returns
// null for disabled, wrong-target, uncommitted, missing, or stale assets.
const char* gba_mod_required_asset_path(const char* package_id,
                                        const char* asset_id);

int gba_mod_runtime_initialize_c(const char* root,
                                 const char* game_id,
                                 const char* rom_sha1);
int gba_mod_runtime_commit_c(const char* rom_path);
void gba_mod_runtime_activate_plugins_c(void);
const char* gba_mod_runtime_last_error_c(void);
const struct RecompLauncherCModProvider*
gba_mod_runtime_launcher_provider_c(void);

#if defined(_MSC_VER)
#pragma section(".CRT$XCU", read)
#define GBA_MOD_CONSTRUCTOR(name)                                           \
    static void __cdecl name(void);                                         \
    __declspec(allocate(".CRT$XCU"))                                        \
    static void (__cdecl* name##_constructor)(void) = name;                 \
    static void __cdecl name(void)
#elif defined(__GNUC__) || defined(__clang__)
#define GBA_MOD_CONSTRUCTOR(name)                                           \
    static void name(void) __attribute__((constructor));                    \
    static void name(void)
#else
#error "GBA mod plugin registration needs a supported constructor mechanism"
#endif

#ifdef __cplusplus
}
#endif
