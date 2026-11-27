#include "mod_rom_data.h"
#include "../gba/sha1.h"
#include <recomp_mod.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <memory>

namespace {
std::string hex(const std::uint8_t* bytes, std::size_t size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string text(size * 2, '0');
    for (std::size_t i = 0; i < size; ++i) {
        text[i * 2] = digits[bytes[i] >> 4];
        text[i * 2 + 1] = digits[bytes[i] & 15];
    }
    return text;
}

bool publish(const gbarecomp::RunOptions& options,
             std::vector<std::uint8_t>& image,
             const std::vector<std::uint8_t>& staged,
             gbarecomp::ModRomDataResult result,
             std::string* fingerprint, std::string* error) {
    std::unique_ptr<rmod_context, decltype(&rmod_destroy)>
        plan(rmod_create(), rmod_destroy);
    if (!plan) { if (error) *error = "Cannot create cartridge mod transaction"; return false; }
    std::array<std::uint8_t, 32> original_hash{}, resolved_hash{}, identity{};
    if (!rmod_sha256_bytes(image.data(), image.size(), original_hash.data())) {
        if (error) *error = "Cannot hash original cartridge identity";
        return false;
    }
    const std::string original = hex(original_hash.data(), original_hash.size());
    const std::string description = "gba.cartridge.v1;" + original + ";" +
        std::to_string(static_cast<int>(result)) + ";" +
        gba::sha1(staged.data(), staged.size()).hex();
    if (!rmod_sha256_bytes(description.data(), description.size(), resolved_hash.data())) {
        if (error) *error = "Cannot hash cartridge mod plan";
        return false;
    }
    const std::string resolved = hex(resolved_hash.data(), resolved_hash.size());
    const char* game = options.mod_game_id ? options.mod_game_id : "gba.loaded-image";
    if (!rmod_bind_plan(plan.get(), game, original.c_str(), resolved.c_str())) {
        if (error) *error = rmod_error(plan.get());
        return false;
    }
    // One bounded claim per changed chunk avoids both a whole-image write
    // and an unbounded list of tiny patches in a large prepared cartridge.
    const std::size_t chunk = std::max<std::size_t>(32768, (image.size() + 4095) / 4096);
    std::vector<rmod_patch> patches;
    for (std::size_t begin = 0; begin < image.size(); begin += chunk) {
        const auto end = std::min(image.size(), begin + chunk);
        auto first = begin;
        while (first < end && image[first] == staged[first]) ++first;
        if (first == end) continue;
        auto last = end;
        while (image[last - 1] == staged[last - 1]) --last;
        patches.push_back({"cartridge", first, image.data() + first,
                           staged.data() + first, last - first});
    }
    rmod_engine engine{};
    engine.user = &image;
    engine.game_id = game;
    engine.original_image_sha256 = original.c_str();
    // Preparation runs before the image is published to devices or execution.
    engine.begin = +[](void*) { return 1; };
    engine.read = +[](void* context, const char* space, std::uint64_t address,
                      std::uint8_t* data, std::size_t size) {
        auto& bytes = *static_cast<std::vector<std::uint8_t>*>(context);
        if (std::strcmp(space, "cartridge") || address > bytes.size() ||
            size > bytes.size() - address) return 0;
        std::memcpy(data, bytes.data() + address, size);
        return 1;
    };
    engine.write = +[](void* context, const char* space, std::uint64_t address,
                       const std::uint8_t* data, std::size_t size) {
        auto& bytes = *static_cast<std::vector<std::uint8_t>*>(context);
        if (std::strcmp(space, "cartridge") || address > bytes.size() ||
            size > bytes.size() - address) return 0;
        std::memcpy(bytes.data() + address, data, size);
        return 1;
    };
    engine.finish = +[](void*, int) {};
    if (rmod_apply(plan.get(), patches.data(), patches.size(), nullptr, 0,
                   &engine, identity.data()) != 1) {
        if (error) *error = rmod_error(plan.get());
        return false;
    }
    if (fingerprint) *fingerprint = hex(identity.data(), identity.size());
    return true;
}
}  // namespace

bool gbarecomp::mod_rom_data_internal::prepare(
        const RunOptions& options, std::vector<std::uint8_t>& image,
        std::string* effective_sha1, std::string* error,
        const ModNativeProgramProfile** selected, std::string* plan_fingerprint) {
    if (selected) *selected = nullptr;
    if (plan_fingerprint) plan_fingerprint->clear();
    if (!options.prepare_mod_rom_data) return true;
    auto fail = [&](const char* reason) { if (error) *error = reason; return false; };
    if (!effective_sha1 || image.size() < 0xC0)
        return fail("Invalid cartridge image for ROM preparation");
    // Original-file identity was validated before this call. The loaded
    // cartridge, including its header bytes, is a modifiable image. Publish
    // none of the callback's writes until its result and native coverage pass.
    auto staged = image;
    const char* preparation_error = nullptr;
    ModRomDataResult result;
    try {
        result = options.prepare_mod_rom_data(
            staged.data(), staged.size(), &preparation_error);
    } catch (const std::exception& e) {
        if (error) *error = std::string("Cartridge preparation failed: ") + e.what();
        return false;
    } catch (...) {
        return fail("Cartridge preparation failed");
    }
    if ((result != ModRomDataResult::Unchanged &&
         result != ModRomDataResult::Prepared &&
         result != ModRomDataResult::PreparedNativeProgram))
        return fail(preparation_error ? preparation_error :
                    "Invalid cartridge preparation result");
    if (result == ModRomDataResult::Unchanged) {
        if (staged != image)
            return fail("Cartridge preparation changed bytes but reported Unchanged");
        return true;
    }
    const auto prepared_sha1 = gba::sha1(staged.data(), staged.size()).hex();
    const ModNativeProgramProfile* resolved = nullptr;
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
        resolved = match;
    }
    if (!publish(options, image, staged, result, plan_fingerprint, error)) return false;
    *effective_sha1 = prepared_sha1;
    if (selected) *selected = resolved;
    return true;
}
