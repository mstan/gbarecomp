#include "mod_rom_data.h"
#include "sha1.h"
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "line%d: %s\n", __LINE__, #c); std::exit(1); } } while(0)
using Result = gbarecomp::ModRomDataResult;
static unsigned calls;
static Result requested = Result::Unchanged;
static bool header_change;
static bool throw_after_write;
static Result callback(std::uint8_t* image, std::size_t size, const char** error) {
    ++calls; CHECK(size == 0x200);
    if (header_change) image[0xA0] ^= 1;  // loaded title text, not entry code
    if (throw_after_write) {
        image[0x101] ^= 0x55;
        throw std::runtime_error("Fixture callback failed after writing");
    }
    if (requested == Result::Prepared || requested == Result::PreparedNativeProgram) image[0x100] ^= 0x5a;
    if (requested == Result::Failed) {
        image[0x101] ^= 0x55;
        *error = "Fixture preparation rejected";
    }
    if (requested == static_cast<Result>(9)) image[0x101] ^= 0x55;
    return requested;
}
int main() {
    std::vector<std::uint8_t> original(0x200, 0x3c), image = original;
    auto digest = [](const auto& bytes) { return gba::sha1(bytes.data(), bytes.size()).hex(); };
    const std::string source = digest(original);
    std::string identity = source, error;
    gbarecomp::RunOptions options;
    CHECK(gbarecomp::mod_rom_data_internal::prepare(options, image, &identity, &error));
    CHECK(!calls && image == original && identity == source);
    options.prepare_mod_rom_data = callback;
    CHECK(gbarecomp::mod_rom_data_internal::prepare(options, image, &identity, &error));
    CHECK(calls == 1 && image == original && identity == source);
    requested = Result::Prepared;
    CHECK(gbarecomp::mod_rom_data_internal::prepare(options, image, &identity, &error));
    CHECK(calls == 2 && image != original && identity == digest(image) && identity != source);
    image = original; identity = source; requested = Result::Failed;
    CHECK(!gbarecomp::mod_rom_data_internal::prepare(options, image, &identity, &error));
    CHECK(calls == 3 && error == "Fixture preparation rejected" &&
          identity == source && image == original);
    requested = Result::Prepared; header_change = true;
    CHECK(gbarecomp::mod_rom_data_internal::prepare(options, image, &identity, &error));
    CHECK(image[0xA0] != original[0xA0] && identity == digest(image));
    CHECK(original[0xA0] == 0x3c);  // original source remains unchanged
    image = original; identity = source; header_change = false; requested = static_cast<Result>(9);
    CHECK(!gbarecomp::mod_rom_data_internal::prepare(options, image, &identity, &error));
    CHECK(image == original && identity == source);
    requested = Result::Unchanged; header_change = true;
    CHECK(!gbarecomp::mod_rom_data_internal::prepare(options, image, &identity, &error));
    CHECK(image == original && identity == source && error.find("Unchanged") != std::string::npos);
    header_change = false; throw_after_write = true;
    CHECK(!gbarecomp::mod_rom_data_internal::prepare(options, image, &identity, &error));
    CHECK(image == original && identity == source && error.find("after writing") != std::string::npos);
    throw_after_write = false;
    // A code-changing result cannot reuse the source cartridge table, nor
    // publish an identity before the exact native program is selected.
    requested = Result::PreparedNativeProgram; image = original;
    const gbarecomp::ModNativeProgramProfile* selected = nullptr;
    CHECK(!gbarecomp::mod_rom_data_internal::prepare(options, image, &identity, &error, &selected));
    CHECK(!selected && identity == source && image == original);
    const DispatchEntry entries[] = {{0x08000100, 1, 0, +[]() {}}};
    const RuntimeNativeProgram native = {entries, 1, nullptr, 0, nullptr, 0};
    auto candidate = original;
    candidate[0x100] ^= 0x5a;
    const auto prepared = digest(candidate);
    gbarecomp::ModNativeProgramProfile catalog[] = {
        {prepared.c_str(), &native, "definitive-1-4"},
        {prepared.c_str(), &native, "other"},
    };
    options.mod_native_programs = catalog;
    options.mod_native_program_count = 1;
    image = original;
    CHECK(gbarecomp::mod_rom_data_internal::prepare(options, image, &identity, &error, &selected));
    CHECK(selected == catalog && identity == prepared && image == candidate);
    image = original; identity = source; options.mod_native_program_count = 2;
    CHECK(!gbarecomp::mod_rom_data_internal::prepare(options, image, &identity, &error, &selected));
    CHECK(!selected && identity == source && image == original);
    options.mod_native_program_count = 1; catalog[0].save_namespace = "../stock"; image = original;
    CHECK(!gbarecomp::mod_rom_data_internal::prepare(options, image, &identity, &error, &selected));
    CHECK(!selected && identity == source && image == original);
    catalog[0].save_namespace = "definitive-1-4";
    catalog[0].rom_sha1 = source.c_str(); image = original;
    CHECK(!gbarecomp::mod_rom_data_internal::prepare(options, image, &identity, &error, &selected));
    CHECK(!selected && identity == source && image == original);
    requested = Result::Unchanged; image = original;
    CHECK(gbarecomp::mod_rom_data_internal::prepare(options, image, &identity, &error, &selected));
    CHECK(!selected && image == original && identity == source);
    // The shared patch seam produces a content/target-bound identity, not a
    // path- or pointer-dependent receipt. Equivalent plans repeat exactly.
    std::string plan, repeated, other_target;
    requested = Result::Prepared; image = original;
    CHECK(gbarecomp::mod_rom_data_internal::prepare(options, image, &identity,
        &error, &selected, &plan));
    CHECK(plan.size() == 64 && image == candidate && !selected);
    image = original; identity = source;
    CHECK(gbarecomp::mod_rom_data_internal::prepare(options, image, &identity,
        &error, &selected, &repeated));
    CHECK(repeated == plan);
    options.mod_game_id = "different-game"; image = original; identity = source;
    CHECK(gbarecomp::mod_rom_data_internal::prepare(options, image, &identity,
        &error, &selected, &other_target));
    CHECK(other_target.size() == 64 && other_target != plan);
    image.resize(0x80); const unsigned before = calls;
    CHECK(!gbarecomp::mod_rom_data_internal::prepare(options, image, &identity, &error));
    CHECK(calls == before);
    std::puts("Loaded cartridge/header writes, transactional failures, exact native coverage and bounds PASS");
}
