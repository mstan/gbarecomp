#include "runtime/asset_picker.h"
#include "gba/gba_bios.h"
#include "gba/sha1.h"
#include "gba/crc32.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;
static void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
struct Fixture {
    fs::path dir = fs::temp_directory_path() / ("gbarecomp-boot-assets-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::path bios = dir / "bios.bin", rom = dir / "rom.gba";
    Fixture() { check(fs::create_directory(dir), "unique fixture directory"); }
    ~Fixture() {
        std::error_code error;
        fs::remove(bios, error); fs::remove(rom, error); fs::remove(dir, error);
    }
};
static void write(const fs::path& path, const std::vector<unsigned char>& bytes) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    check(bool(stream), "fixture write");
}
int main() {
    try {
        Fixture f;
        std::vector<unsigned char> bytes(gba::GbaBios::kSize);
        for (std::size_t i=0; i<bytes.size(); ++i) bytes[i]=(i*97u+(i>>4))&255u;
        write(f.bios, bytes);
        const auto digest = gba::sha1(bytes.data(), bytes.size()).hex();
        gbarecomp::AssetSpec spec;
        spec.expected_size=bytes.size(); spec.expected_sha1=digest.c_str();
        spec.persist_cache=false;
        auto selected=gbarecomp::resolve_asset(f.bios.string(), spec, "");
        check(selected.ok && selected.bytes==bytes && selected.sha1_hex==digest,
              "resolver retains validated BIOS snapshot");
        gba::GbaBios original, memory;
        std::string error;
        check(original.load_from_file(f.bios.string(), digest, &error), "original BIOS load");
        check(fs::remove(f.bios), "remove original path after resolution");
        check(memory.load_from_bytes(selected.bytes.data(), selected.bytes.size(), digest, &error),
              "consume validated snapshot after original path disappears");
        selected.bytes.assign(selected.bytes.size(), 0);
        check(memory.loaded() && memory.sha1_hex()==original.sha1_hex(), "owned BIOS digest");
        for (unsigned i=0; i<bytes.size(); ++i) {
            check(memory.read8(i)==original.read8(i), "BIOS byte differential");
            check(memory.read16(i)==original.read16(i), "BIOS halfword differential");
            check(memory.read32(i)==original.read32(i), "BIOS word differential");
        }
        check(!memory.load_from_bytes(nullptr, bytes.size(), "", &error) && !memory.loaded(),
              "null image rejected and previous load invalidated");
        check(!memory.load_from_bytes(bytes.data(), bytes.size()-1, "", &error), "size rejected");
        check(!memory.load_from_bytes(bytes.data(), bytes.size(), "wrong", &error) &&
              !memory.loaded() && memory.sha1_hex()==digest, "hash mismatch retained");
        check(memory.load_from_bytes(bytes.data(), bytes.size(), "", &error),
              "noncanonical BIOS warn-and-try loader policy");
        write(f.rom, bytes);
        spec.expected_sha1="wrong"; spec.expected_size=0;
        auto warning=gbarecomp::validate_asset_path(f.rom.string(), spec);
        check(warning.ok && !warning.warning.empty() && warning.sha1_hex==digest &&
              warning.crc32==gba::crc32(bytes.data(),bytes.size()), "picker warning metadata");
        spec.hash_mismatch_is_error=true;
        check(!gbarecomp::validate_asset_path(f.rom.string(), spec).ok, "strict asset policy");
        std::cout << "boot asset snapshot, file/memory BIOS differential, ownership and rejection contracts passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
