#pragma once

#include <stdint.h>
#include "../gba/foreign_obj_focus.h"
#include "../gba/foreign_screen_overlay.h"

#ifdef __cplusplus
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "codebreaker.h"

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

// Declarative cheats ([[cheat]] records, docs/MOD_PACKAGES.md). The committed
// set is the enabled cheat features of the last successful commit; it is
// empty until a commit, after initialize, and whenever the catalog is not
// loaded at all (netplay never loads it). The runner calls
// mod_runtime_run_cheats once per guest frame only while the count is
// non-zero, so a session without enabled cheats is byte-identical to stock.
std::size_t mod_runtime_committed_cheat_count();
std::vector<std::string> mod_runtime_committed_cheat_names();
void mod_runtime_run_cheats(const codebreaker::Memory& memory);

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

// A trusted diagnostics feature may select replay capture for this launch.
// -1 means no catalog request: retain the title/config default. Activation
// clears the request before reset callbacks run. GBARECOMP_DIAGNOSTICS still
// overrides the selection for automated runs and explicit player requests.
int gba_mod_set_diagnostic_replay_enabled(int enabled);
int gba_mod_diagnostic_replay_enabled(void);
// Optional presentation density, 1..16; 0 clears the request. Reset on each
// activation pass. This selects pixels, never HLE/LLE simulation backends.
int gba_mod_set_internal_resolution(int scale);
int gba_mod_internal_resolution(void);

// Optional initial/fixed logical width; 0 means no plugin request. The runner
// still clamps this to the game's capability. Reset on every activation pass.
int gba_mod_set_view_width(int width);
int gba_mod_view_width(void);

// Immutable values from the last successful commit, only for enabled features
// of matching packages. Null for unavailable values. Valid until next commit
// or initialize; editing staged launcher choices does not change this snapshot.
const char* gba_mod_option_value(const char* package_id, const char* feature_id,
                                 const char* option_id);

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

// Publish one bounded, immutable BGR555/Q4-alpha rectangle over native guest
// backgrounds. Guest OBJ composition remains above it. This presentation is
// automatically suppressed while a full foreign background is active.
int gba_mod_publish_foreign_screen_overlay(
    const char* plugin_id, const GbaForeignScreenOverlay* overlay);
void gba_mod_clear_foreign_screen_overlay(void);

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
