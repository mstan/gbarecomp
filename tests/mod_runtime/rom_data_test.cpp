#include "mod_rom_data.h"
#include "sha1.h"
#include <cstdio>
#include <cstdlib>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "line%d: %s\n", __LINE__, #c); std::exit(1); } } while(0)
using Result = gbarecomp::ModRomDataResult;
static unsigned calls;
static Result requested = Result::Unchanged;
static bool header_change;
static Result callback(std::uint8_t* image, std::size_t size, const char** error) {
    ++calls; CHECK(size == 0x200);
    if (header_change) image[0] ^= 1;
    if (requested == Result::Prepared) image[0x100] ^= 0x5a;
    if (requested == Result::Failed) *error = "Fixture preparation rejected";
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
    CHECK(calls == 3 && error == "Fixture preparation rejected" && identity == source);
    requested = Result::Prepared; header_change = true;
    CHECK(!gbarecomp::mod_rom_data_internal::prepare(options, image, &identity, &error));
    CHECK(identity == source && error.find("header") != std::string::npos);
    image = original; header_change = false; requested = static_cast<Result>(9);
    CHECK(!gbarecomp::mod_rom_data_internal::prepare(options, image, &identity, &error));
    CHECK(identity == source);
    image.resize(0x80); const unsigned before = calls;
    CHECK(!gbarecomp::mod_rom_data_internal::prepare(options, image, &identity, &error));
    CHECK(calls == before);
    std::puts("ROM data null/unchanged/prepared identity, failure/header/invalid-result and bounds PASS");
}
