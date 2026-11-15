#include "mod_rom_data.h"
#include "../gba/sha1.h"
#include <array>
#include <cstring>

bool gbarecomp::mod_rom_data_internal::prepare(
        const RunOptions& options, std::vector<std::uint8_t>& image,
        std::string* effective_sha1, std::string* error,
        const ModNativeProgramProfile** selected) {
    if (selected) *selected = nullptr;
    if (!options.prepare_mod_rom_data) return true;
    auto fail = [&](const char* reason) { if (error) *error = reason; return false; };
    std::array<std::uint8_t, 0xC0> original_header;
    if (!effective_sha1 || image.size() < original_header.size())
        return fail("Invalid cartridge image for ROM preparation");
    std::memcpy(original_header.data(), image.data(), original_header.size());
    const char* preparation_error = nullptr;
    const auto result = options.prepare_mod_rom_data(
        image.data(), image.size(), &preparation_error);
    if ((result != ModRomDataResult::Unchanged &&
         result != ModRomDataResult::Prepared &&
         result != ModRomDataResult::PreparedNativeProgram) ||
        std::memcmp(original_header.data(), image.data(), original_header.size()))
        return fail(preparation_error ? preparation_error :
                    "Invalid preparation result or modified cartridge header");
    if (result == ModRomDataResult::Unchanged) return true;
    const auto prepared_sha1 = gba::sha1(image.data(), image.size()).hex();
    if (result == ModRomDataResult::PreparedNativeProgram) {
        if (!selected || !options.mod_native_programs)
            return fail("Prepared image has no compiled native program catalog");
        const ModNativeProgramProfile* match = nullptr;
        for (std::size_t i = 0; i < options.mod_native_program_count; ++i) {
            const auto& p = options.mod_native_programs[i];
            if (!p.rom_sha1 || prepared_sha1 != p.rom_sha1) continue;
            if (match || !p.program || !p.program->dispatch || !p.program->dispatch_count)
                return fail("Invalid or ambiguous compiled native program profile");
            if (!p.save_namespace || !*p.save_namespace)
                return fail("Compiled native program requires a battery-save namespace");
            for (const char* c = p.save_namespace; *c; ++c)
                if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-'))
                    return fail("Invalid compiled native program save namespace");
            match = &p;
        }
        if (!match) return fail("Prepared cartridge has no exact compiled native program");
        *selected = match;
    }
    *effective_sha1 = prepared_sha1;
    return true;
}
