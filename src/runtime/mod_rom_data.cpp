#include "mod_rom_data.h"
#include "../gba/sha1.h"
#include <array>
#include <cstring>

bool gbarecomp::mod_rom_data_internal::prepare(
        const RunOptions& options, std::vector<std::uint8_t>& image,
        std::string* effective_sha1, std::string* error) {
    if (!options.prepare_mod_rom_data) return true;
    auto fail = [&](const char* reason) { if (error) *error = reason; return false; };
    std::array<std::uint8_t, 0xC0> original_header;
    if (!effective_sha1 || image.size() < original_header.size())
        return fail("Invalid cartridge image for ROM data preparation");
    std::memcpy(original_header.data(), image.data(), original_header.size());
    const char* preparation_error = nullptr;
    const auto result = options.prepare_mod_rom_data(
        image.data(), image.size(), &preparation_error);
    if ((result != ModRomDataResult::Unchanged &&
         result != ModRomDataResult::Prepared) ||
        std::memcmp(original_header.data(), image.data(), original_header.size()))
        return fail(preparation_error ? preparation_error :
                    "Invalid preparation result or modified cartridge header");
    if (result == ModRomDataResult::Prepared)
        *effective_sha1 = gba::sha1(image.data(), image.size()).hex();
    return true;
}
