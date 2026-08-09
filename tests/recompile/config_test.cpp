#include "config.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

int fail(const char* message) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

bool write_file(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    return static_cast<bool>(out);
}

std::string base_config(const std::string& hooks) {
    return
        "[program]\n"
        "load_address = 0x08000000\n"
        "size = 0x100\n"
        "entry_pc = 0x08000000\n\n"
        "[identity]\n"
        "sha1 = \"0000000000000000000000000000000000000000\"\n\n" + hooks;
}

}  // namespace

int main() {
    const fs::path path = fs::temp_directory_path() /
        ("gbarecomp-config-hook-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) +
         ".toml");
    std::error_code ec;
    gbarecomp::Config cfg;

    const std::string valid = base_config(
        "[[mod_function_hook]]\n"
        "addr = 0x08000020\n"
        "mode = \"thumb\"\n");
    if (!write_file(path, valid) || !gbarecomp::load_config(path.string(), cfg) ||
        cfg.mod_function_hooks.size() != 1 ||
        cfg.mod_function_hooks[0].mode != gbarecomp::CpuMode::Thumb) {
        fs::remove(path, ec);
        return fail("valid mod_function_hook did not parse");
    }

    const std::string unaligned = base_config(
        "[[mod_function_hook]]\n"
        "addr = 0x08000021\n"
        "mode = \"thumb\"\n");
    if (!write_file(path, unaligned) || gbarecomp::load_config(path.string(), cfg)) {
        fs::remove(path, ec);
        return fail("unaligned mod_function_hook was accepted");
    }

    const std::string mismatch = base_config(
        "[[extra_func]]\n"
        "addr = 0x08000020\n"
        "mode = \"arm\"\n\n"
        "[[mod_function_hook]]\n"
        "addr = 0x08000020\n"
        "mode = \"thumb\"\n");
    if (!write_file(path, mismatch) || gbarecomp::load_config(path.string(), cfg)) {
        fs::remove(path, ec);
        return fail("mode-conflicting mod_function_hook was accepted");
    }
    fs::remove(path, ec);
    return 0;
}
