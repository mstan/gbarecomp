#pragma once

#include <stdint.h>

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
