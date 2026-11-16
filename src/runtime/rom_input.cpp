#include "rom_input.h"
#include "../gba/sha1.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>

#if defined(GBARECOMP_ROM_ARCHIVES)
#include <archive.h>
#include <archive_entry.h>
#endif

namespace gbarecomp {
namespace {
constexpr std::size_t max_rom = 32u * 1024u * 1024u;
constexpr std::size_t max_container = 128u * 1024u * 1024u;
constexpr std::size_t max_decoded = 256u * 1024u * 1024u;
constexpr unsigned max_entries = 1024;

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool is_archive(const unsigned char* b, std::size_t n, const std::string& path) {
    const auto ext = lower(std::filesystem::u8path(path).extension().string());
    return ext == ".zip" || ext == ".rar" || ext == ".7z" ||
        (n >= 4 && b[0] == 'P' && b[1] == 'K' &&
         ((b[2] == 3 && b[3] == 4) || (b[2] == 5 && b[3] == 6))) ||
        (n >= 7 && std::memcmp(b, "Rar!\x1a\x07", 6) == 0) ||
        (n >= 6 && std::memcmp(b, "7z\xbc\xaf\x27\x1c", 6) == 0);
}
}

bool rom_archives_available() {
#if defined(GBARECOMP_ROM_ARCHIVES)
    return true;
#else
    return false;
#endif
}

bool read_rom_input(const std::string& path, const char* expected_sha1,
                    std::vector<std::uint8_t>* bytes, std::string* error) {
    bytes->clear();
    if (error) error->clear();
    const auto fail = [&](const char* why) {
        bytes->clear();
        if (error) *error = why;
        return false;
    };
    std::error_code ec;
    const auto source = std::filesystem::u8path(path);
    if (!std::filesystem::is_regular_file(source, ec) || ec)
        return fail("The selected file could not be opened.");
    const auto size = std::filesystem::file_size(source, ec);
    if (ec || size == 0) return fail("The selected file is empty or could not be read.");
    if (size > max_container) return fail("This file is too large. Select a ROM or an archive smaller than 128 MB.");
    std::ifstream input(source, std::ios::binary);
    if (!input) return fail("The selected file could not be opened.");
    std::array<unsigned char, 8> magic{};
    input.read(reinterpret_cast<char*>(magic.data()), magic.size());
    const auto magic_size = static_cast<std::size_t>(input.gcount());
    input.clear(); input.seekg(0);
    const std::string expected = lower(expected_sha1 ? expected_sha1 : "");
    const bool archive = is_archive(magic.data(), magic_size, path);
    // A verified raw image wins over a misleading extension or archive magic.
    if (size <= max_rom) {
        bytes->resize(static_cast<std::size_t>(size));
        if (!input.read(reinterpret_cast<char*>(bytes->data()), bytes->size()))
            return fail("The selected file could not be read.");
        if (!archive || (!expected.empty() && gba::sha1(bytes->data(), bytes->size()).hex() == expected))
            return true;
    } else if (!archive) return fail("The ROM is too large. GBA ROMs must be 32 MB or smaller.");
    bytes->clear();
#if !defined(GBARECOMP_ROM_ARCHIVES)
    return fail("This build cannot open archives. Extract the .gba ROM first.");
#else
    if (expected.size() != 40 || !std::all_of(expected.begin(), expected.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
        return fail("This build does not identify a compatible ROM. Extract the ROM first.");
    std::unique_ptr<struct archive, decltype(&archive_read_free)> reader(archive_read_new(), archive_read_free);
    if (!reader) return fail("There is not enough memory to read this archive.");
    archive_read_support_filter_none(reader.get());
    archive_read_support_format_zip(reader.get());
    archive_read_support_format_7zip(reader.get());
    archive_read_support_format_rar(reader.get());
    archive_read_support_format_rar5(reader.get());
#if defined(_WIN32)
    const int opened = archive_read_open_filename_w(reader.get(), source.c_str(), 65536);
#else
    const int opened = archive_read_open_filename(reader.get(), source.c_str(), 65536);
#endif
    const auto archive_error = [&]() {
        return fail("This archive could not be read. Use an unencrypted, single-part ZIP, RAR or 7z file, or extract the ROM first.");
    };
    if (opened != ARCHIVE_OK) return archive_error();
    unsigned entries = 0;
    std::size_t decoded = 0;
    archive_entry* entry = nullptr;
    int rc;
    while ((rc = archive_read_next_header(reader.get(), &entry)) == ARCHIVE_OK) {
        if (++entries > max_entries) return fail("This archive contains too many files. Select an archive with 1024 entries or fewer.");
        if (archive_entry_is_encrypted(entry) > 0)
            return fail("This archive is encrypted. Extract the ROM first or use an archive without a password.");
        if (archive_entry_filetype(entry) != AE_IFREG || archive_entry_hardlink(entry) || archive_entry_symlink(entry)) {
            if (archive_read_data_skip(reader.get()) != ARCHIVE_OK) return archive_error();
            continue;
        }
        const auto declared = archive_entry_size(entry);
        if (declared < 0 || declared > static_cast<la_int64_t>(max_rom))
            return fail("An archive member is too large. Each ROM must be 32 MB or smaller.");
        std::vector<std::uint8_t> candidate;
        candidate.reserve(static_cast<std::size_t>(declared));
        std::array<std::uint8_t, 65536> block{};
        for (;;) {
            const auto count = archive_read_data(reader.get(), block.data(), block.size());
            if (count < 0) return archive_error();
            if (count == 0) break;
            if (candidate.size() + count > max_rom || decoded + count > max_decoded)
                return fail("This archive expands to too much data. Put just the game ROM in a smaller archive.");
            candidate.insert(candidate.end(), block.begin(), block.begin() + count);
            decoded += count;
        }
        if (candidate.size() != static_cast<std::size_t>(declared)) return archive_error();
        if (!candidate.empty() && gba::sha1(candidate.data(), candidate.size()).hex() == expected) {
            if (!bytes->empty() && *bytes != candidate)
                return fail("This archive contains different compatible images. Put the intended ROM in its own archive.");
            *bytes = std::move(candidate); // Identical duplicates are one choice.
        }
    }
    if (rc != ARCHIVE_EOF || archive_read_has_encrypted_entries(reader.get()) > 0) return archive_error();
    if (bytes->empty()) return fail("No compatible game ROM was found in this archive. Use the ROM revision required by this build.");
    return true;
#endif
}
} // namespace gbarecomp
