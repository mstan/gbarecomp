// Exercise the production provider and its private filter builder without an
// OS dialog, a ROM, a game window, or an exported testing-only runtime API.
#ifndef GBA_MOD_RUNTIME_SOURCE
#define GBA_MOD_RUNTIME_SOURCE "../../src/runtime/mod_runtime.cpp"
#endif
#include GBA_MOD_RUNTIME_SOURCE
#include <iostream>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void write_file(const fs::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary);
    file << text;
    check(static_cast<bool>(file), "fixture write failed");
}
}

int main(int argc, char** argv) {
    fs::path sandbox;
    try {
        if (argc < 2 || std::string(argv[1]) != "--provider-only") {
            gbarecomp::RequiredAsset asset;
            asset.name = "Test patch";
            asset.extensions = {"ips", "bps"};
            const auto filter = gbarecomp::required_asset_spec(asset);
            constexpr char expected[] = "Test patch (*.ips;*.IPS;*.bps;*.BPS)\0"
                "*.ips;*.IPS;*.bps;*.BPS\0All Files (*.*)\0*.*\0";
            check(filter.dialog_filter == std::string(expected, sizeof(expected)-1),
                  "required-asset filter lost embedded NUL pairs/terminator");
        }
        const auto* provider = gbarecomp::mod_runtime_launcher_provider();
        check(provider && provider->feature_resource_count &&
              provider->feature_resource_get && provider->feature_resource_set_path,
              "launcher cannot enumerate/select required assets on feature enable");
        sandbox = fs::temp_directory_path() / ("gbarecomp-resource-ui-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const auto root = sandbox / "mods";
        const auto package = root / "packages/test.resource/1.0.0";
        fs::create_directories(package);
        const auto good = sandbox / "good.ips", bad = sandbox / "bad.ips";
        write_file(good, "valid-patch");
        write_file(bad, "wrong-patch"); // same size; fails identity, not size
        const std::string body = "valid-patch";
        const auto sha = gba::sha1(reinterpret_cast<const uint8_t*>(body.data()),
                                  body.size()).hex();
        const std::string target(40, '1');
        write_file(package / "manifest.toml",
            "format_version = 1\nid = \"test.resource\"\nversion = \"1.0.0\"\n"
            "name = \"Resource fixture\"\nauthor = \"gbarecomp\"\n"
            "description = \"Synthetic required file.\"\nlicense = \"MIT\"\n"
            "resolver = \"declarative\"\nsave_compatibility = \"shared\"\n"
            "[[target]]\ngame_id = \"test-game\"\nrom_sha1 = \""+target+"\"\n"
            "[[feature]]\nid = \"patch\"\nname = \"Patch\"\ndefault_enabled = false\n"
            "[[feature]]\nid = \"other\"\nname = \"Other\"\ndefault_enabled = false\n"
            "[[asset]]\nfeature = \"patch\"\nid = \"source\"\nname = \"Test patch\"\n"
            "sha1 = \""+sha+"\"\nsize = 11\nextensions = [\"ips\"]\n"
            "purpose = \"Supplies the optional feature.\"\n");
        std::string error;
        auto initialize = [&] { return gbarecomp::mod_runtime_initialize(root,
                                                "test-game", target, &error); };
        check(initialize(), error.c_str());
        const char* id = "test.resource";
        check(provider->feature_resource_count(nullptr, id, "patch") == 1 &&
              provider->feature_resource_count(nullptr, id, "other") == 0,
              "resource enumeration did not retain feature ownership");
        check(provider->feature_enable(nullptr, id, "patch", 1), "enable failed");
        RecompLauncherCModResource resource{};
        check(provider->feature_resource_get(nullptr, id, "patch", 0, &resource) &&
              resource.required && !resource.path[0] && !resource.verified &&
              std::string(resource.file_patterns) == "*.ips",
              "missing resource was not exposed to the enable picker");
        check(!gbarecomp::mod_runtime_commit({}, &error),
              "noninteractive commit accepted a missing required asset");
        auto set = [&](const char* feature, const char* asset, const std::string& path) {
            return provider->feature_resource_set_path(nullptr, id, feature, asset,
                                                       path.c_str());
        };
        check(!set("other", "source", good.string()) &&
              !set("patch", "unknown", good.string()) &&
              !set("patch", "source", (sandbox/"missing.ips").string()) &&
              !set("patch", "source", bad.string()),
              "resource selection accepted unknown ownership/missing/wrong-hash file");
        check(!gbarecomp::mod_runtime_required_asset_path(id, "source"),
              "a rejected or uncommitted selection leaked to a plugin");
        check(set("patch", "source", good.string()), "valid resource rejected");
        check(provider->feature_resource_get(nullptr, id, "patch", 0, &resource) &&
              resource.verified && resource.path == good.string(),
              "validated selection did not reach the launcher");
        check(initialize(), "resource selection did not reload");
        check(provider->feature_resource_get(nullptr, id, "patch", 0, &resource) &&
              resource.verified && resource.path == good.string(),
              "selected path/feature choice did not persist");
        check(gbarecomp::mod_runtime_commit({}, &error), error.c_str());
        const char* committed = gbarecomp::mod_runtime_required_asset_path(id, "source");
        check(committed && committed == good.string(), "verified commit path unavailable");
        write_file(good, "wrong-patch");
        check(!gbarecomp::mod_runtime_commit({}, &error) &&
              !gbarecomp::mod_runtime_required_asset_path(id, "source"),
              "commit trusted the UI verdict after the file changed");
        check(provider->feature_resource_get(nullptr, id, "patch", 0, &resource) &&
              !resource.verified, "failed commit left a stale verified UI verdict");
        write_file(good, body);
        check(set("patch", "source", ""), "clear failed");
        check(!gbarecomp::mod_runtime_commit({}, &error), "cleared asset passed commit");
        check(provider->feature_enable(nullptr, id, "patch", 0) &&
              gbarecomp::mod_runtime_commit({}, &error) && initialize(),
              "disabled feature demanded an asset or lost its saved Off choice");
        check(gbarecomp::mod_runtime_commit({}, &error), "saved Off demanded asset");
        check(gbarecomp::mod_runtime_initialize(root, "other-game", target, &error) &&
              provider->feature_resource_count(nullptr, id, "patch") == 0 &&
              !set("patch", "source", good.string()),
              "wrong-target catalog exposed/accepted a resource");
        // Only these synthetic fixture files were created under the unique root.
        fs::remove(good); fs::remove(bad); fs::remove(root/"state.toml");
        fs::remove(package/"manifest.toml"); fs::remove(package);
        fs::remove(package.parent_path()); fs::remove(package.parent_path().parent_path());
        fs::remove(root); fs::remove(sandbox);
        std::cout << "mod resource UI: PASS filter, enable enumeration, strict selection, "
                     "persistence, commit revalidation, saved Off, target/ownership gates\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << " (fixture " << sandbox << ")\n";
        return 1;
    }
}
