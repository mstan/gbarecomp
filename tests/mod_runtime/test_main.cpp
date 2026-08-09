#include "mod_runtime.h"
#include "mod_function_hooks.h"
#include "asset_picker.h"
#include "foreign_presentation_internal.h"
#include "mod_audio.h"
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
bool g_reset_saw_foreign_presentation_clear = false;

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
    g_reset_saw_foreign_presentation_clear =
        gba::foreign_presentation_internal::background() == nullptr &&
        gba::foreign_presentation_internal::obj_focus() == nullptr &&
        gba::foreign_presentation_internal::screen_overlay() == nullptr;
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
    // One selected trusted plugin may own several source-proven hook points
    // (for example a RAM code-copy lifecycle seam plus a player-state gate).
    // Registration must remain idempotent per exact tuple, not per plugin ID.
    if (!gba_mod_register_function_entry_plugin("test.hook.multi",
                                                 0x08003010u, 1,
                                                 decline_entry) ||
        !gba_mod_register_function_entry_plugin("test.hook.multi",
                                                 0x03000020u, 0,
                                                 decline_entry) ||
        !gba_mod_set_function_hook_enabled("test.hook.multi", 1)) {
        return fail("plugin could not register and enable multiple hook points");
    }
    const int declines_before_multi = g_declines;
    if (gba_mod_function_entry(0x08003010u, 1, &hook_cpu) ||
        gba_mod_function_entry(0x03000020u, 0, &hook_cpu) ||
        g_declines != declines_before_multi + 2) {
        return fail("multi-hook plugin did not observe both entry points");
    }
    gba_mod_disable_all_function_hooks();

    const auto nonce = std::chrono::steady_clock::now()
                           .time_since_epoch().count();
    const fs::path sandbox = fs::temp_directory_path() /
        ("gbarecomp-mod-runtime-" + std::to_string(nonce));
    const fs::path mods = sandbox / "mods";
    const fs::path package = mods / "packages" /
        "test.adaptive-view" / "1.0.0";
    const fs::path rom_path = sandbox / "test.gba";
    const fs::path foreign_asset_path = sandbox / "zelda1.nes";
    const fs::path wrong_asset_path = sandbox / "wrong-zelda1.nes";
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
    const std::vector<unsigned char> foreign_asset = {
        'z', 'e', 'l', 'd', 'a', '-', 'o', 'w', 'n', 'e', 'd'
    };
    {
        std::ofstream file(foreign_asset_path,
                           std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(foreign_asset.data()),
                   static_cast<std::streamsize>(foreign_asset.size()));
        if (!file) return fail("could not write required foreign asset");
    }
    const std::string foreign_asset_sha1 =
        gba::sha1(foreign_asset.data(), foreign_asset.size()).hex();
    if (!write_text(wrong_asset_path, "wrong-owned"))
        return fail("could not write wrong required foreign asset");
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
        "[[feature]]\n"
        "id = \"other-feature\"\n"
        "name = \"Other feature\"\n"
        "group = \"Display\"\n"
        "default_enabled = false\n\n"
        "[[plugin]]\n"
        "feature = \"adaptive-view\"\n"
        "id = \"test.adaptive-view\"\n\n"
        "[[asset]]\n"
        "feature = \"adaptive-view\"\n"
        "id = \"zelda1-rom\"\n"
        "name = \"The Legend of Zelda ROM\"\n"
        "sha1 = \"" + foreign_asset_sha1 + "\"\n"
        "size = " + std::to_string(foreign_asset.size()) + "\n"
        "extensions = [\"nes\"]\n"
        "purpose = \"Used only by this trusted foreign-world feature.\"\n";
    if (!write_text(package / "manifest.toml", manifest))
        return fail("could not write manifest");

    if (!gba_mod_register_reset_callback(reset_view) ||
        !gba_mod_register_activation_plugin(
            "test.adaptive-view", activate_view)) {
        return fail("could not register trusted plugin");
    }
    // Registration does not authorize a presentation source. In particular,
    // an ID that will later be selected cannot publish before its feature and
    // external asset have passed commit validation.
    const std::uint16_t foreign_pixels[] = {0x001F, 0x03E0};
    const std::uint16_t replacement_pixels[] = {0x7C00, 0x7FFF};
    const std::uint16_t overlay_pixels[] = {0x001F, 0x03E0, 0x7C00, 0x7FFF};
    std::array<std::uint8_t, 4> overlay_alpha{0, 8, 16, 4};
    const GbaForeignScreenOverlay foreign_overlay{
        GBA_FOREIGN_SCREEN_OVERLAY_ABI_VERSION,
        overlay_pixels, overlay_alpha.data(),
        -1, 8, 2, 2, 2, 0, 0,
    };
    GbaForeignScreenOverlay invalid_overlay_abi = foreign_overlay;
    invalid_overlay_abi.abi_version++;
    GbaForeignScreenOverlay invalid_overlay_stride = foreign_overlay;
    invalid_overlay_stride.stride = 1;
    GbaForeignScreenOverlay invalid_overlay_width = foreign_overlay;
    invalid_overlay_width.width = GBA_FOREIGN_SCREEN_OVERLAY_MAX_WIDTH + 1;
    GbaForeignScreenOverlay invalid_overlay_wide_stride = foreign_overlay;
    invalid_overlay_wide_stride.stride =
        GBA_FOREIGN_SCREEN_OVERLAY_MAX_WIDTH + 1;
    GbaForeignScreenOverlay invalid_overlay_reserved = foreign_overlay;
    invalid_overlay_reserved.reserved32 = 1;
    auto invalid_overlay_alpha = overlay_alpha;
    invalid_overlay_alpha[2] = 17;
    GbaForeignScreenOverlay invalid_overlay_alpha_value = foreign_overlay;
    invalid_overlay_alpha_value.alpha_q4 = invalid_overlay_alpha.data();
    const GbaForeignObjFocusTransform foreign_focus{
        GBA_FOREIGN_OBJ_FOCUS_ABI_VERSION,
        48, 64, 72, 88,
        32, 32,
        GBA_FOREIGN_OBJ_FOCUS_PRESERVE_UNFOCUSED,
        0, 0,
        0, 0,
        0, 0, 0, 0,
    };
    GbaForeignObjFocusTransform invalid_focus = foreign_focus;
    invalid_focus.source_radius_x = 241;  // Native PPU width is 240.
    GbaForeignObjFocusTransform invalid_flags = foreign_focus;
    invalid_flags.flags = 0x80000000u;
    GbaForeignObjFocusTransform invalid_scale = foreign_focus;
    invalid_scale.source_obj_scale_q8_8 =
        GBA_FOREIGN_OBJ_FOCUS_SCALE_MIN_Q8_8 - 1;
    GbaForeignObjFocusTransform invalid_unscoped_scale = foreign_focus;
    invalid_unscoped_scale.source_obj_scale_q8_8 = 192;
    GbaForeignObjFocusTransform invalid_reserved = foreign_focus;
    invalid_reserved.reserved = 1;
    GbaForeignObjFocusTransform invalid_hud_bg_map = foreign_focus;
    invalid_hud_bg_map.hud_bg_layer_mask = 1;
    invalid_hud_bg_map.hud_bg_map_rect_count = 1;
    invalid_hud_bg_map.hud_bg_map_tile_x = 64;
    invalid_hud_bg_map.hud_bg_map_tile_y = 1;
    invalid_hud_bg_map.hud_bg_map_tile_width = 1;
    invalid_hud_bg_map.hud_bg_map_tile_height = 1;
    invalid_hud_bg_map.hud_bg_output_width = 8;
    invalid_hud_bg_map.hud_bg_output_height = 8;
    GbaForeignObjFocusTransform invalid_inactive_hud_bg = foreign_focus;
    invalid_inactive_hud_bg.hud_bg_layer_mask = 1;
    GbaForeignObjFocusTransform invalid_layerless_hud_bg = foreign_focus;
    invalid_layerless_hud_bg.hud_bg_map_rect_count = 1;
    invalid_layerless_hud_bg.hud_bg_map_tile_width = 1;
    invalid_layerless_hud_bg.hud_bg_map_tile_height = 1;
    invalid_layerless_hud_bg.hud_bg_output_width = 8;
    invalid_layerless_hud_bg.hud_bg_output_height = 8;
    GbaForeignObjFocusTransform invalid_global_hud = foreign_focus;
    invalid_global_hud.flags =
        GBA_FOREIGN_OBJ_FOCUS_SUPPRESS_NONMATCHING_EXCEPT_HUD_OAM;
    GbaForeignObjFocusTransform origin_only_focus = foreign_focus;
    origin_only_focus.flags |= GBA_FOREIGN_OBJ_FOCUS_ORIGIN_ONLY |
        GBA_FOREIGN_OBJ_FOCUS_SOURCE_TILE_RANGE;
    origin_only_focus.source_obj_tile_base = 100;
    origin_only_focus.source_obj_tile_count = 16;
    origin_only_focus.source_obj_scale_q8_8 = 192;
    origin_only_focus.flags |=
        GBA_FOREIGN_OBJ_FOCUS_SUPPRESS_NONMATCHING_EXCEPT_HUD_OAM;
    origin_only_focus.hud_oam_mask_lo = UINT64_C(1) << 7;
    gba_mod_clear_foreign_background();
    gba_mod_clear_foreign_obj_focus();
    gba_mod_clear_foreign_screen_overlay();
    if (gba_mod_publish_foreign_background("test.adaptive-view", foreign_pixels) ||
        gba_mod_publish_foreign_background("test.wrong-plugin", foreign_pixels) ||
        gba_mod_publish_foreign_obj_focus("test.adaptive-view", &foreign_focus) ||
        gba_mod_publish_foreign_obj_focus("test.wrong-plugin", &foreign_focus) ||
        gba_mod_publish_foreign_screen_overlay("test.adaptive-view", &foreign_overlay) ||
        gba_mod_publish_foreign_screen_overlay("test.wrong-plugin", &foreign_overlay) ||
        gba::foreign_presentation_internal::background() != nullptr ||
        gba::foreign_presentation_internal::obj_focus() != nullptr ||
        gba::foreign_presentation_internal::screen_overlay() != nullptr) {
        return fail("uncommitted or unknown plugin published foreign presentation");
    }
    // A source may be registered before the package selection is known, but
    // activation must reset it to its inert disabled state before a selected
    // plugin can deliberately enable/play it.
    const int16_t pcm[] = {1000};
    const GBAModAudioClip audio_clip = gba_mod_audio_register_pcm_s16_mono(
        pcm, 1, GBA_MOD_AUDIO_SAMPLE_RATE);
    if (audio_clip == GBA_MOD_AUDIO_CLIP_INVALID ||
        !gba_mod_audio_set_enabled(audio_clip, 1) ||
        !gba_mod_audio_play(audio_clip, 100, 1)) {
        return fail("could not prepare trusted PCM source");
    }

    const auto write_state = [&](bool asset_enabled, bool other_enabled,
                                 const fs::path& asset_path) {
        return write_text(
            mods / "state.toml",
            "format_version = 1\n\n"
            "[[package]]\n"
            "id = \"test.adaptive-view\"\n"
            "version = \"1.0.0\"\n\n"
            "[[feature]]\n"
            "package_id = \"test.adaptive-view\"\n"
            "id = \"adaptive-view\"\n"
            "enabled = " + std::string(asset_enabled ? "true\n\n" : "false\n\n") +
            "[[feature]]\n"
            "package_id = \"test.adaptive-view\"\n"
            "id = \"other-feature\"\n"
            "enabled = " + std::string(other_enabled ? "true\n\n" : "false\n\n") +
            "[[asset]]\n"
            "package_id = \"test.adaptive-view\"\n"
            "id = \"zelda1-rom\"\n"
            "path = \"" + asset_path.generic_string() + "\"\n");
    };

    std::string error;
    // An unrelated enabled feature must not require or expose this asset.
    if (!write_state(false, true, {}) ||
        !gbarecomp::mod_runtime_initialize(
            mods, "test-game", sha1, &error) ||
        !gbarecomp::mod_runtime_commit(rom_path, &error)) {
        return fail("unrelated-feature plan failed: " + error);
    }
    gbarecomp::mod_runtime_activate_plugins();
    if (g_active || gba_mod_adaptive_view_enabled() ||
        gba_mod_required_asset_path("test.adaptive-view", "zelda1-rom"))
        return fail("unrelated feature required or exposed a foreign asset");

    // Headless/noninteractive commits must refuse an absent asset rather than
    // trying to open a picker.
    if (!write_state(true, false, {}) ||
        !gbarecomp::mod_runtime_initialize(mods, "test-game", sha1, &error) ||
        gbarecomp::mod_runtime_commit(rom_path, &error))
        return fail("missing required asset did not fail noninteractive commit");

    if (!write_state(true, false, foreign_asset_path) ||
        !gbarecomp::mod_runtime_initialize(
            mods, "test-game", sha1, &error) ||
        !gbarecomp::mod_runtime_commit(rom_path, &error)) {
        return fail("enabled asset plan failed: " + error);
    }
    gbarecomp::mod_runtime_activate_plugins();
    if (!g_active || !gba_mod_adaptive_view_enabled())
        return fail("enabled plugin did not activate");
    if (gba_mod_audio_play(audio_clip, 100, 0))
        return fail("activation did not reset PCM source to disabled");
    const char* resolved = gba_mod_required_asset_path(
        "test.adaptive-view", "zelda1-rom");
    if (!resolved || fs::path(resolved) != foreign_asset_path ||
        fs::exists(package / "zelda1.nes"))
        return fail("required asset was not exposed as an external validated path");

    // Only the committed plugin can replace the PPU-owned presentation
    // pointer. Its address is not copied or transformed by the runtime.
    if (!gba_mod_publish_foreign_background("test.adaptive-view", foreign_pixels) ||
        gba::foreign_presentation_internal::background() != foreign_pixels ||
        gba_mod_publish_foreign_background("test.wrong-plugin", replacement_pixels) ||
        gba::foreign_presentation_internal::background() != foreign_pixels ||
        !gba_mod_publish_foreign_obj_focus("test.adaptive-view", &foreign_focus) ||
        gba::foreign_presentation_internal::obj_focus() != &foreign_focus ||
        gba_mod_publish_foreign_obj_focus("test.wrong-plugin", &invalid_focus) ||
        gba_mod_publish_foreign_obj_focus("test.adaptive-view", &invalid_focus) ||
        gba_mod_publish_foreign_obj_focus("test.adaptive-view", &invalid_flags) ||
        gba_mod_publish_foreign_obj_focus("test.adaptive-view", &invalid_scale) ||
        gba_mod_publish_foreign_obj_focus("test.adaptive-view", &invalid_unscoped_scale) ||
        gba_mod_publish_foreign_obj_focus("test.adaptive-view", &invalid_reserved) ||
        gba_mod_publish_foreign_obj_focus("test.adaptive-view", &invalid_hud_bg_map) ||
        gba_mod_publish_foreign_obj_focus("test.adaptive-view", &invalid_inactive_hud_bg) ||
        gba_mod_publish_foreign_obj_focus("test.adaptive-view", &invalid_layerless_hud_bg) ||
        gba_mod_publish_foreign_obj_focus("test.adaptive-view", &invalid_global_hud) ||
        gba::foreign_presentation_internal::obj_focus() != &foreign_focus) {
        return fail("foreign presentation publication was not plugin-authorized or stable");
    }
    if (!gba_mod_publish_foreign_obj_focus("test.adaptive-view", &origin_only_focus) ||
        gba::foreign_presentation_internal::obj_focus() != &origin_only_focus ||
        !gba_mod_publish_foreign_obj_focus("test.adaptive-view", &foreign_focus)) {
        return fail("committed plugin could not publish bounded origin-only focus");
    }
    if (!gba_mod_publish_foreign_screen_overlay("test.adaptive-view", &foreign_overlay) ||
        gba::foreign_presentation_internal::screen_overlay() != &foreign_overlay ||
        gba_mod_publish_foreign_screen_overlay("test.wrong-plugin", &foreign_overlay) ||
        gba_mod_publish_foreign_screen_overlay("test.adaptive-view", &invalid_overlay_abi) ||
        gba_mod_publish_foreign_screen_overlay("test.adaptive-view", &invalid_overlay_stride) ||
        gba_mod_publish_foreign_screen_overlay("test.adaptive-view", &invalid_overlay_width) ||
        gba_mod_publish_foreign_screen_overlay("test.adaptive-view", &invalid_overlay_wide_stride) ||
        gba_mod_publish_foreign_screen_overlay("test.adaptive-view", &invalid_overlay_reserved) ||
        gba_mod_publish_foreign_screen_overlay("test.adaptive-view", &invalid_overlay_alpha_value) ||
        gba::foreign_presentation_internal::screen_overlay() != &foreign_overlay) {
        return fail("foreign screen overlay was not authorized, bounded, or stable");
    }
    gba_mod_clear_foreign_background();
    gba_mod_clear_foreign_obj_focus();
    gba_mod_clear_foreign_screen_overlay();
    if (gba::foreign_presentation_internal::background() != nullptr ||
        gba::foreign_presentation_internal::obj_focus() != nullptr ||
        gba::foreign_presentation_internal::screen_overlay() != nullptr)
        return fail("explicit foreign presentation clear left a stale pointer");

    // Every activation pass clears the old provider before reset callbacks.
    // The activation callback deliberately does not republish, so null must
    // remain until a selected plugin explicitly supplies a fresh buffer.
    if (!gba_mod_publish_foreign_background("test.adaptive-view", foreign_pixels) ||
        !gba_mod_publish_foreign_obj_focus("test.adaptive-view", &foreign_focus) ||
        !gba_mod_publish_foreign_screen_overlay("test.adaptive-view", &foreign_overlay))
        return fail("committed plugin could not republish foreign presentation");
    g_reset_saw_foreign_presentation_clear = false;
    gbarecomp::mod_runtime_activate_plugins();
    if (!g_reset_saw_foreign_presentation_clear) {
        return fail("reactivation did not clear foreign presentation before callbacks");
    }
    // Explicit activation-boundary lifetime check for the bounded native
    // overlay. This must hold independently of the PPU reset/load tests.
    if (gba::foreign_presentation_internal::screen_overlay() != nullptr)
        return fail("plugin activation left a stale native screen overlay");
    if (gba::foreign_presentation_internal::background() != nullptr ||
        gba::foreign_presentation_internal::obj_focus() != nullptr) {
        return fail("reactivation left stale foreign presentation");
    }

    gbarecomp::AssetSpec strict_asset;
    strict_asset.display_name = "test required asset";
    strict_asset.expected_size = foreign_asset.size();
    strict_asset.expected_sha1 = foreign_asset_sha1.c_str();
    strict_asset.hash_mismatch_is_error = true;
    if (!gbarecomp::validate_asset_path(foreign_asset_path.string(),
                                        strict_asset).ok ||
        gbarecomp::validate_asset_path(wrong_asset_path.string(), strict_asset).ok)
        return fail("strict required-asset size/hash validation failed");

    // A remembered wrong-hash path is stale and cannot survive a fresh
    // initialize or noninteractive commit.
    if (!write_state(true, false, wrong_asset_path) ||
        !gbarecomp::mod_runtime_initialize(mods, "test-game", sha1, &error) ||
        gba_mod_required_asset_path("test.adaptive-view", "zelda1-rom") ||
        gbarecomp::mod_runtime_commit(rom_path, &error))
        return fail("stale wrong-hash asset was accepted or remained published");

    // The failed plan deliberately cleared the old committed catalog. Restore
    // the selected, asset-validated plan so we can model its old provider
    // while the following commit disables that feature.
    if (!write_state(true, false, foreign_asset_path) ||
        !gbarecomp::mod_runtime_initialize(
            mods, "test-game", sha1, &error) ||
        !gbarecomp::mod_runtime_commit(rom_path, &error)) {
        return fail("could not restore selected plan before disable: " + error);
    }
    // Stage the old selected provider before the new, disabled feature plan
    // replaces the committed plugin set.
    if (!gba_mod_publish_foreign_background("test.adaptive-view", foreign_pixels) ||
        !gba_mod_publish_foreign_obj_focus("test.adaptive-view", &foreign_focus))
        return fail("previously committed plugin could not stage stale presentation");
    if (!write_state(false, false, foreign_asset_path) ||
        !gbarecomp::mod_runtime_initialize(
            mods, "test-game", sha1, &error) ||
        !gbarecomp::mod_runtime_commit(rom_path, &error)) {
        return fail("disabled plan failed: " + error);
    }
    // Disabling the selected feature has the same pre-callback clearing
    // contract, even if the previously selected plugin left a stable pointer.
    g_reset_saw_foreign_presentation_clear = false;
    gbarecomp::mod_runtime_activate_plugins();
    if (!g_reset_saw_foreign_presentation_clear ||
        gba::foreign_presentation_internal::background() != nullptr ||
        gba::foreign_presentation_internal::obj_focus() != nullptr ||
        g_active || gba_mod_adaptive_view_enabled())
        return fail("disabled plugin did not restore native view");
    if (gba_mod_required_asset_path("test.adaptive-view", "zelda1-rom"))
        return fail("disabled package exposed a required asset");

    const fs::path wrong_rom = sandbox / "wrong.gba";
    if (!write_text(wrong_rom, "wrong") ||
        gbarecomp::mod_runtime_commit(wrong_rom, &error)) {
        return fail("ROM identity guard accepted a mismatched image");
    }

    const fs::path bad_package = mods / "packages" / "bad.asset" / "1.0.0";
    const std::string bad_manifest_prefix =
        "format_version = 1\n"
        "id = \"bad.asset\"\n"
        "version = \"1.0.0\"\n"
        "name = \"Bad asset\"\n"
        "resolver = \"declarative\"\n\n"
        "[[target]]\n"
        "game_id = \"test-game\"\n"
        "rom_sha1 = \"" + sha1 + "\"\n\n"
        "[[feature]]\n"
        "id = \"known-feature\"\n"
        "name = \"Known feature\"\n"
        "default_enabled = false\n\n";
    const std::string valid_asset =
        "[[asset]]\n"
        "feature = \"known-feature\"\n"
        "id = \"owned-rom\"\n"
        "name = \"Owned ROM\"\n"
        "sha1 = \"" + foreign_asset_sha1 + "\"\n"
        "size = " + std::to_string(foreign_asset.size()) + "\n"
        "extensions = [\"nes\"]\n"
        "purpose = \"Test asset.\"\n";
    const auto rejects_bad_manifest = [&](const std::string& suffix) {
        std::error_code local_ec;
        fs::create_directories(bad_package, local_ec);
        if (local_ec || !write_text(bad_package / "manifest.toml",
                                    bad_manifest_prefix + suffix))
            return false;
        std::string bad_error;
        const bool rejected = !gbarecomp::mod_runtime_initialize(
            mods, "test-game", sha1, &bad_error);
        fs::remove_all(mods / "packages" / "bad.asset", local_ec);
        return rejected;
    };
    const std::string malformed_asset =
        "[[asset]]\n"
        "feature = \"known-feature\"\n"
        "id = \"owned-rom\"\n"
        "name = \"Owned ROM\"\n"
        "sha1 = \"" + foreign_asset_sha1 + "\"\n"
        "extensions = [\"nes\"]\n"
        "purpose = \"Missing required size.\"\n";
    const std::string unknown_feature_asset =
        "[[asset]]\n"
        "feature = \"not-a-feature\"\n"
        "id = \"owned-rom\"\n"
        "name = \"Owned ROM\"\n"
        "sha1 = \"" + foreign_asset_sha1 + "\"\n"
        "size = " + std::to_string(foreign_asset.size()) + "\n"
        "extensions = [\"nes\"]\n"
        "purpose = \"Unknown owner.\"\n";
    if (!rejects_bad_manifest(malformed_asset) ||
        !rejects_bad_manifest(valid_asset + valid_asset) ||
        !rejects_bad_manifest(unknown_feature_asset))
        return fail("malformed, duplicate, or unknown-owner asset manifest was accepted");

    fs::remove_all(sandbox, ec);
    std::cout << "GBA mod runtime: target gate, strict external asset, "
                 "persisted feature toggle, trusted activation, and reset passed\n";
    return 0;
}
