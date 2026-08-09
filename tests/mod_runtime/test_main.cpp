#include "mod_runtime.h"
#include "mod_function_hooks.h"
#include "sha1.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

bool g_active = false;
int g_declines = 0;
int g_handles = 0;

int decline_entry(uint32_t, int, ArmCpuState*) {
    ++g_declines;
    return 0;
}

int mutate_then_decline_entry(uint32_t, int, ArmCpuState* cpu) {
    ++g_declines;
    cpu->R[1] = 0xBAD0BAD0u;
    return 0;
}

int handle_entry(uint32_t addr, int thumb, ArmCpuState* cpu) {
    ++g_handles;
    cpu->R[0] = 0xC0DEC0DEu;
    cpu->R[15] = cpu->R[14] & ~1u;
    return addr == 0x08003000u && thumb == 1;
}

int fail(const std::string& message) {
    std::cerr << "FAIL: " << message << "\n";
    return 1;
}

void reset_view() {
    g_active = false;
    (void)gba_mod_set_adaptive_view_enabled(0);
}

void activate_view() {
    g_active = true;
    (void)gba_mod_set_adaptive_view_enabled(1);
}

bool write_text(const fs::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
    return static_cast<bool>(file);
}

}  // namespace

int main() {
    // The low-level registry is always available, independent of the package
    // catalog. Registration is inert, decline falls through, and a handler
    // owns the callback-authored CPU state.
    ArmCpuState hook_cpu{};
    hook_cpu.R[14] = 0x08001235u;
    hook_cpu.R[1] = 0x11223344u;
    if (gba_mod_register_function_entry_plugin("test.hook.bad-thumb",
                                                0x08003001u, 1,
                                                decline_entry) ||
        gba_mod_register_function_entry_plugin("test.hook.bad-arm",
                                                0x08003002u, 0,
                                                decline_entry) ||
        !gba_mod_register_function_entry_plugin("test.hook.decline",
                                                 0x08003000u, 1,
                                                 mutate_then_decline_entry) ||
        !gba_mod_register_function_entry_plugin("test.hook.handle",
                                                 0x08003000u, 1,
                                                 handle_entry) ||
        gba_mod_function_entry(0x08003000u, 1, &hook_cpu) ||
        g_declines != 0 || g_handles != 0) {
        return fail("function hook registration was not disabled by default");
    }
    if (!gba_mod_set_function_hook_enabled("test.hook.decline", 1) ||
        gba_mod_function_entry(0x08003000u, 1, &hook_cpu) ||
        g_declines != 1 || g_handles != 0 || hook_cpu.R[1] != 0x11223344u) {
        return fail("declining function hook did not fall through");
    }
    if (!gba_mod_set_function_hook_enabled("test.hook.handle", 1) ||
        !gba_mod_function_entry(0x08003000u, 1, &hook_cpu) ||
        g_declines != 2 || g_handles != 1 ||
        hook_cpu.R[0] != 0xC0DEC0DEu || hook_cpu.R[15] != 0x08001234u ||
        gba_mod_function_hook_hits() != 1u) {
        return fail("handled function hook did not own guest CPU state");
    }
    gba_mod_disable_all_function_hooks();
    if (gba_mod_function_hook_enabled("test.hook.handle") ||
        gba_mod_function_entry(0x08003000u, 1, &hook_cpu)) {
        return fail("disable-all did not restore stock hook behavior");
    }

    const auto nonce = std::chrono::steady_clock::now()
                           .time_since_epoch().count();
    const fs::path sandbox = fs::temp_directory_path() /
        ("gbarecomp-mod-runtime-" + std::to_string(nonce));
    const fs::path mods = sandbox / "mods";
    const fs::path package = mods / "packages" /
        "test.adaptive-view" / "1.0.0";
    const fs::path rom_path = sandbox / "test.gba";
    std::error_code ec;
    fs::create_directories(package, ec);
    if (ec) return fail("could not create sandbox: " + ec.message());

    const std::vector<unsigned char> rom = {
        'g', 'b', 'a', 'r', 'e', 'c', 'o', 'm', 'p', '-', 'm', 'o', 'd'
    };
    {
        std::ofstream file(rom_path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(rom.data()),
                   static_cast<std::streamsize>(rom.size()));
        if (!file) return fail("could not write test ROM");
    }
    const std::string sha1 = gba::sha1(rom.data(), rom.size()).hex();
    const std::string manifest =
        "format_version = 1\n"
        "id = \"test.adaptive-view\"\n"
        "version = \"1.0.0\"\n"
        "name = \"Adaptive view test\"\n"
        "author = \"gbarecomp\"\n"
        "description = \"Trusted plugin vertical slice.\"\n"
        "license = \"MIT\"\n"
        "resolver = \"declarative\"\n"
        "save_compatibility = \"shared\"\n\n"
        "[[target]]\n"
        "game_id = \"test-game\"\n"
        "rom_sha1 = \"" + sha1 + "\"\n\n"
        "[[feature]]\n"
        "id = \"adaptive-view\"\n"
        "name = \"Adaptive view\"\n"
        "group = \"Display\"\n"
        "default_enabled = false\n\n"
        "[[plugin]]\n"
        "feature = \"adaptive-view\"\n"
        "id = \"test.adaptive-view\"\n";
    if (!write_text(package / "manifest.toml", manifest))
        return fail("could not write manifest");

    if (!gba_mod_register_reset_callback(reset_view) ||
        !gba_mod_register_activation_plugin(
            "test.adaptive-view", activate_view)) {
        return fail("could not register trusted plugin");
    }

    const auto write_state = [&](bool enabled) {
        return write_text(
            mods / "state.toml",
            "format_version = 1\n\n"
            "[[package]]\n"
            "id = \"test.adaptive-view\"\n"
            "version = \"1.0.0\"\n\n"
            "[[feature]]\n"
            "package_id = \"test.adaptive-view\"\n"
            "id = \"adaptive-view\"\n"
            "enabled = " + std::string(enabled ? "true\n" : "false\n"));
    };

    std::string error;
    if (!write_state(true) ||
        !gbarecomp::mod_runtime_initialize(
            mods, "test-game", sha1, &error) ||
        !gbarecomp::mod_runtime_commit(rom_path, &error)) {
        return fail("enabled plan failed: " + error);
    }
    gbarecomp::mod_runtime_activate_plugins();
    if (!g_active || !gba_mod_adaptive_view_enabled())
        return fail("enabled plugin did not activate");

    if (!write_state(false) ||
        !gbarecomp::mod_runtime_initialize(
            mods, "test-game", sha1, &error) ||
        !gbarecomp::mod_runtime_commit(rom_path, &error)) {
        return fail("disabled plan failed: " + error);
    }
    gbarecomp::mod_runtime_activate_plugins();
    if (g_active || gba_mod_adaptive_view_enabled())
        return fail("disabled plugin did not restore native view");

    const fs::path wrong_rom = sandbox / "wrong.gba";
    if (!write_text(wrong_rom, "wrong") ||
        gbarecomp::mod_runtime_commit(wrong_rom, &error)) {
        return fail("ROM identity guard accepted a mismatched image");
    }

    fs::remove_all(sandbox, ec);
    std::cout << "GBA mod runtime: target gate, persisted feature toggle, "
                 "trusted activation, and reset passed\n";
    return 0;
}
