// Real launcher/provider selection with synthetic manifests; no recorder,
// cartridge, native picker, or desktop window is needed for this contract.
#include "../../src/runtime/mod_runtime.cpp"
#include <iostream>

namespace {
int presentation_activations = 0;
void presentation_reset() {
    gba_mod_set_adaptive_view_enabled(0);
    gba_mod_set_diagnostic_replay_enabled(0); // an old coupled presentation mod
}
void presentation_activate() {
    ++presentation_activations;
    gba_mod_set_adaptive_view_enabled(1);
    gba_mod_set_diagnostic_replay_enabled(0);
}
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void write_file(const fs::path& path, const std::string& body) {
    std::ofstream file(path, std::ios::binary);
    file << body;
    check(static_cast<bool>(file), "fixture write failed");
}
std::string read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
}

int main() {
    fs::path sandbox;
    try {
        sandbox = fs::temp_directory_path() / ("gbarecomp-diagnostics-ui-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const auto root = sandbox / "mods";
        const auto state_root = sandbox / "player" / "mods";
        const auto diagnostic = root / "packages" /
            gbarecomp::kDiagnosticReplayPackage / "1.0.0";
        const auto presentation = root / "packages/test.presentation/1.0.0";
        fs::create_directories(diagnostic);
        fs::create_directories(presentation);
        const std::string target(40, '1');
        auto manifest = [&](const std::string& id, const std::string& feature) {
            return "format_version = 1\nid = \""+id+"\"\nversion = \"1.0.0\"\n"
                "name = \"Gameplay Diagnostics\"\nauthor = \"gbarecomp\"\n"
                "description = \"Synthetic host feature.\"\nlicense = \"MIT\"\n"
                "resolver = \"declarative\"\nsave_compatibility = \"shared\"\n"
                "[[target]]\ngame_id = \"test-game\"\nrom_sha1 = \""+target+"\"\n"
                "[[feature]]\nid = \""+feature+"\"\nname = \"Record Gameplay\"\n"
                "group = \"Diagnostics\"\ndefault_enabled = ";
        };
        write_file(diagnostic / "manifest.toml", manifest(
            gbarecomp::kDiagnosticReplayPackage, gbarecomp::kDiagnosticReplayFeature) + "true\n");
        write_file(presentation / "manifest.toml",
            manifest("test.presentation", "view") + "false\n"
            "[[plugin]]\nfeature = \"view\"\nid = \"test.presentation\"\n");
        check(gba_mod_register_reset_callback(presentation_reset) &&
              gba_mod_register_activation_plugin("test.presentation", presentation_activate),
              "presentation fixture registration failed");
        std::string error;
        auto initialize = [&] { return gbarecomp::mod_runtime_initialize(
            root, "test-game", target, &error, state_root); };
        check(initialize(), error.c_str());
        const auto* provider = gbarecomp::mod_runtime_launcher_provider();
        check(provider && provider->commit_netplay, "host-only lobby commit unavailable");
        const char* package = gbarecomp::kDiagnosticReplayPackage;
        const char* feature = gbarecomp::kDiagnosticReplayFeature;
        auto get = [&](const char* id, const char* wanted) {
            RecompLauncherCModFeature out{};
            for (int i = 0; i < provider->feature_count(nullptr); ++i) {
                check(provider->feature_get(nullptr, i, &out), "feature read failed");
                if (std::string(out.package_id) == id && std::string(out.id) == wanted)
                    return out;
            }
            throw std::runtime_error("visible feature missing");
        };
        auto commit = [&] {
            check(gbarecomp::mod_runtime_commit({}, &error), error.c_str());
            gbarecomp::mod_runtime_activate_plugins();
        };
        auto row = get(package, feature);
        check(row.enabled && !row.hidden &&
              std::string(row.group) == "Diagnostics", "visible default-On control missing");
#if defined(RECOMP_LAUNCHER_HAS_QUICK_MODS)
        check(!row.quick, "recording control must not opt into Home Quick Mods");
#endif
        commit();
        check(gba_mod_diagnostic_replay_enabled() == 1 && presentation_activations == 0,
              "recording default depends on presentation activation");
        check(provider->feature_enable(nullptr, "test.presentation", "view", 1), "view enable");
        commit();
        check(gba_mod_adaptive_view_enabled() && gba_mod_diagnostic_replay_enabled() == 1,
              "presentation activation overrode the independent recorder");
        check(provider->feature_enable(nullptr, "test.presentation", "view", 0), "view disable");
        commit();
        check(!gba_mod_adaptive_view_enabled() && gba_mod_diagnostic_replay_enabled() == 1,
              "presentation Off disabled recording");

        const auto config = state_root.parent_path() / "config.ini";
        const auto saved_state = state_root / "state.toml";
        struct MigrationCase {
            const char* package;
            const char* feature;
            std::optional<bool> old_choice;
            bool new_row;
            std::optional<bool> new_choice;
            bool config_choice;
            int read_only;
            bool expected;
        };
        const MigrationCase migrations[] = {
            {"gba.diagnostics", "recording", false, false, {}, true, 0, false},
            {"gba.diagnostics", "recording", true, false, {}, false, 1, true},
            {"gba.diagnostics", "recording", true, true, false, true, 0, false},
            {"gba.diagnostics", "recording", false, true, true, false, 1, true},
            {"gba.diagnostics", "recording", false, true, {}, true, 0, false},
            {"gba.diagnostics", "recording", {}, false, {}, false, -1, false},
            {"gba.diagnostics", nullptr, {}, false, {}, false, -1, false},
            {"other.diagnostics", "recording", false, false, {}, true, -1, true},
            {"gba.diagnostics", "other", false, false, {}, true, -1, true},
        };
        for (const auto& migration : migrations) {
            std::string seed = "format_version = 1\n[[package]]\n"
                "id = \"test.unrelated\"\nversion = \"2.0.0\"\n"
                "[[feature]]\npackage_id = \"test.unrelated\"\nid = \"keep\"\n"
                "enabled = false\n[feature.values]\nmarker = \"unchanged\"\n";
            seed += "[[package]]\nid = \"" + std::string(migration.package) +
                "\"\nversion = \"1.0.0\"\n";
            if (migration.feature) {
                seed += "[[feature]]\npackage_id = \"" + std::string(migration.package) +
                    "\"\nid = \"" + migration.feature + "\"\n";
                if (migration.old_choice.has_value())
                    seed += std::string("enabled = ") +
                        (*migration.old_choice ? "true\n" : "false\n");
            }
            if (migration.new_row) {
                seed += "[[package]]\nid = \"" + std::string(package) +
                    "\"\nversion = \"1.0.0\"\n[[feature]]\npackage_id = \"" +
                    package + "\"\nid = \"" + feature + "\"\n";
                if (migration.new_choice.has_value())
                    seed += std::string("enabled = ") +
                        (*migration.new_choice ? "true\n" : "false\n");
            }
            const std::string old_config = std::string("[Diagnostics]\nreplay_capture=") +
                (migration.config_choice ? "1\n" : "0\n");
            write_file(saved_state, seed);
            write_file(config, old_config);
            const int before_lookup = presentation_activations;
            check(gbarecomp::mod_runtime_saved_diagnostic_replay_enabled(state_root) ==
                      migration.read_only && read_file(saved_state) == seed &&
                      presentation_activations == before_lookup,
                  "read-only recording choice lookup changed state or ignored priority");
            check(initialize() && get(package, feature).enabled == migration.expected,
                  "legacy package recording choice/default/config priority was lost");
            if (migration.new_choice.has_value())
                check(read_file(saved_state) == seed,
                      "explicit current recording choice was rewritten during import");
            commit();
            check(gba_mod_diagnostic_replay_enabled() == migration.expected &&
                      read_file(config) == old_config,
                  "recording choice was not applied or legacy config was rewritten");
            gbarecomp::Runtime reopened;
            reopened.state_root = state_root;
            check(gbarecomp::load_state(reopened, &error), "reopen imported selections");
            const auto& unrelated = reopened.selections.at("test.unrelated");
            check(unrelated.version == "2.0.0" &&
                      unrelated.features.at("keep").has_enabled &&
                      !unrelated.features.at("keep").enabled &&
                      unrelated.features.at("keep").values.at("marker") == "unchanged",
                  "recording import damaged an unrelated saved selection");
            if (migration.old_choice.has_value()) {
                const auto& retained = reopened.selections.at(migration.package)
                    .features.at(migration.feature);
                check(retained.has_enabled && retained.enabled == *migration.old_choice,
                      "recording import changed the legacy selection");
            }
            const std::string imported = read_file(saved_state);
            check(initialize() && get(package, feature).enabled == migration.expected &&
                      read_file(saved_state) == imported,
                  "cold reopen changed an already imported recording choice");
        }
        write_file(saved_state, "format_version = 1\n");
        const std::string legacy_off = "[Other]\nkeep=this\n[Diagnostics]\nreplay_capture=0\n";
        write_file(config, legacy_off);
        check(initialize() && !get(package, feature).enabled,
              "legacy Off was not imported into the visible control");
        commit();
        check(gba_mod_diagnostic_replay_enabled() == 0 && read_file(config) == legacy_off,
              "legacy Off was lost or the old profile was rewritten");
        check(provider->feature_enable(nullptr, package, feature, 1), "record On");
        commit();
        check(initialize() && get(package, feature).enabled, "saved On lost to legacy Off");
        commit();
        check(gba_mod_diagnostic_replay_enabled() == 1, "saved On not applied");
        write_file(config, "[Diagnostics]\nreplay_capture=1\n");
        check(provider->feature_enable(nullptr, package, feature, 0), "record Off");
        commit();
        check(initialize() && !get(package, feature).enabled, "saved Off lost to legacy On");
        commit();
        check(gba_mod_diagnostic_replay_enabled() == 0, "saved Off not applied");

        check(provider->feature_enable(nullptr, "test.presentation", "view", 1) &&
              provider->feature_enable(nullptr, package, feature, 1), "offline choices");
        commit();
        const int before_lobby = presentation_activations;
        check(provider->feature_enable(nullptr, "test.presentation", "view", 0) &&
              provider->feature_enable(nullptr, package, feature, 0), "lobby choices");
        check(provider->commit_netplay(nullptr, nullptr), "host-only lobby persistence failed");
        gbarecomp::mod_runtime_activate_plugins();
        check(presentation_activations == before_lobby && !gba_mod_adaptive_view_enabled() &&
              gba_mod_diagnostic_replay_enabled() == 0,
              "netplay activated gameplay mods or lost the recording choice");
        check(initialize() && get("test.presentation", "view").enabled &&
              !get(package, feature).enabled,
              "lobby changed offline presentation choice or failed to save recording Off");
        const auto saved_bytes = read_file(state_root / "state.toml");
        write_file(presentation / "manifest.toml", "invalid catalog for read-only lookup");
        check(gbarecomp::mod_runtime_saved_diagnostic_replay_enabled(state_root) == 0 &&
              read_file(state_root / "state.toml") == saved_bytes &&
              presentation_activations == before_lobby,
              "host-only lookup scanned/activated a catalog or changed state");
        check(!fs::exists(root / "state.toml"), "catalog root acquired player state");
        // Only these synthetic files exist beneath this unique owned root.
        check(fs::weakly_canonical(sandbox.parent_path()) ==
              fs::weakly_canonical(fs::temp_directory_path()), "fixture containment");
        fs::remove_all(sandbox);
        std::cout << "diagnostics UI: PASS default On, saved On/Off, legacy package/config precedence, "
                     "presentation independence, host-only netplay persistence\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << " (fixture " << sandbox << ")\n";
        return 1;
    }
}
