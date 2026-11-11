#include "launcher_input_cache.h"
#include <chrono>
#include <cstdio>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace gbarecomp_seam;

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
static void put(const fs::path& path, const std::string& value) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path); file << value;
    require(bool(file), "create synthetic fixture");
}
struct Scratch {
    fs::path path;
    explicit Scratch(const fs::path& parent) : path(fs::absolute(parent) /
        ("gba-cache-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        require(!fs::exists(path), "fixture already exists");
        fs::create_directories(path);
    }
    ~Scratch() { std::error_code ec; fs::remove_all(path, ec); }
};

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    try {
        Scratch source{fs::path(argv[1])};
        Scratch target{fs::path(argv[2])};
        const auto root = source.path / "package";
        for (const char* name : {"rom.cfg", "bios.cfg"}) {
            const auto cache = root / name;
            put(root / "inputs" / name, "synthetic input");
            write_single_line(cache.string(), fs::canonical(root / "inputs" / name).string());
            require(read_single_line(cache.string()) == std::string("inputs/") + name,
                    "first internal selection was not portable");
            write_single_line(cache.string(), fs::canonical(root / "inputs" / name).string());
            require(read_single_line(cache.string()) == std::string("inputs/") + name,
                    "unchanged relative input became absolute");
        }
        // Move the complete package to another parent (G: -> C: in Windows QA).
        const auto moved = target.path / "package";
        fs::copy(root, moved, fs::copy_options::recursive);
        for (const char* name : {"rom.cfg", "bios.cfg"}) {
            const auto cache = moved / name;
            const auto input = moved / fs::path(read_single_line(cache.string()));
            require(fs::exists(input), "relocated selection does not resolve");
            write_single_line(cache.string(), fs::canonical(input).string());
            require(read_single_line(cache.string()) == std::string("inputs/") + name,
                    "relocated unchanged selection became absolute");
        }
        const auto cache = root / "rom.cfg";
        const auto other = root / "other.bin"; put(other, "new input");
        write_single_line(cache.string(), other.string());
        require(read_single_line(cache.string()) == "other.bin", "new internal selection was not portable");
        write_single_line(cache.string(), other.string());
        require(read_single_line(cache.string()) == "other.bin", "unchanged internal choice was discarded");
        write_single_line(cache.string(), "");
        require(read_single_line(cache.string()) == "other.bin", "empty choice erased cache");
        put(cache, "missing.bin\n");
        write_single_line(cache.string(), other.string());
        require(read_single_line(cache.string()) == "other.bin", "stale relative selection won");
        const auto external = source.path / "external.bin"; put(external, "outside package");
        write_single_line(cache.string(), external.string());
        require(fs::path(read_single_line(cache.string())).is_absolute() &&
                fs::equivalent(read_single_line(cache.string()), external), "external input lost its selected location");
#if defined(_WIN32)
        put(cache, "INPUTS/ROM.CFG\n");
        write_single_line(cache.string(), (root / "inputs" / "rom.cfg").string());
        require(read_single_line(cache.string()) == "INPUTS/ROM.CFG", "case-insensitive equivalent selection lost spelling");
#endif
        std::puts("input caches preserve relative selections across relocation");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "launcher_input_cache_test: %s\n", error.what());
        return 1;
    }
}
