#include "runtime/rom_input.h"
#include "runtime/asset_picker.h"
#include "runtime/launcher_input_cache.h"
#include "gba/sha1.h"
#include <archive.h>
#include <archive_entry.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;
static void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
static void write(const fs::path& path, const Bytes& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    check(bool(out), "write fixture");
}
struct Member { const char* name; Bytes bytes; bool link = false; };
static void pack(const fs::path& path, bool seven, const std::vector<Member>& members, bool encrypted = false) {
    archive* a = archive_write_new();
    check(a != nullptr, "archive writer");
    check((seven ? archive_write_set_format_7zip(a) : archive_write_set_format_zip(a)) == ARCHIVE_OK, "format writer");
    if (encrypted) {
        check(archive_write_set_passphrase(a, "fixture-password") == ARCHIVE_OK, "fixture passphrase");
        check(archive_write_set_options(a, "zip:encryption=aes256") == ARCHIVE_OK, "encrypted ZIP writer");
    }
    check(archive_write_open_filename(a, path.string().c_str()) == ARCHIVE_OK, "open writer");
    for (const auto& m : members) {
        archive_entry* e = archive_entry_new();
        archive_entry_set_pathname(e, m.name);
        archive_entry_set_size(e, m.link ? 0 : m.bytes.size());
        archive_entry_set_filetype(e, m.link ? AE_IFLNK : AE_IFREG);
        archive_entry_set_perm(e, 0644);
        if (m.link) archive_entry_set_symlink(e, "game.gba");
        check(archive_write_header(a, e) == ARCHIVE_OK, "write header");
        if (!m.link) check(archive_write_data(a, m.bytes.data(), m.bytes.size()) == static_cast<la_ssize_t>(m.bytes.size()), "write member");
        archive_entry_free(e);
    }
    check(archive_write_close(a) == ARCHIVE_OK, "close archive");
    archive_write_free(a);
}
int main(int argc, char** argv) {
    try {
        check(argc >= 2, "fixture directory argument");
        std::cout << "Decoder: " << archive_version_string() << '\n';
        check(archive_version_number() >= 3008011, "patched decoder required");
        const fs::path dir = fs::u8path(argv[1]);
        fs::create_directories(dir);
        Bytes body(16384);
        for (std::size_t i=0; i<body.size(); ++i) body[i] = (i*97u+(i>>4))&255u;
        const auto digest = gba::sha1(body.data(), body.size()).hex();
        const auto raw = dir / "raw.gba";
        write(raw, body);
        Bytes got; std::string error;
        check(gbarecomp::read_rom_input(raw.string(), digest.c_str(), &got, &error) && got == body, "raw regression");
        write(dir / "raw.zip", body);
        check(gbarecomp::read_rom_input((dir/"raw.zip").string(), digest.c_str(), &got, &error) && got == body, "raw identity wins over archive extension");
        Bytes wrong(800, 9);
        for (bool seven : {false, true}) {
            const auto archive = dir / (seven ? "games.7z" : "games.zip");
            pack(archive, seven, {{"other.gba", wrong}, {"nested/game.gba", body}, {"duplicate.gba", body}});
            check(gbarecomp::read_rom_input(archive.string(), digest.c_str(), &got, &error) && got == body, "matching member selected; duplicates deduplicated");
            check(!fs::exists(dir/"nested") && !fs::exists(dir/"duplicate.gba"), "no filesystem extraction");
            check(!gbarecomp::read_rom_input(archive.string(), "0000000000000000000000000000000000000000", &got, &error) && got.empty() && error.find("No compatible") != std::string::npos, "no matching member fails closed");
            check(!gbarecomp::read_rom_input(archive.string(), "", &got, &error) && got.empty(), "archive needs pinned identity");
            gbarecomp::AssetSpec spec;
            spec.expected_sha1=digest.c_str(); spec.allow_rom_archives=true;
            spec.hash_mismatch_is_error=true;
            auto r = gbarecomp::resolve_asset(archive.string(), spec, (dir/"game.exe").string());
            check(r.ok && r.bytes == body && r.path == archive.string() && r.sha1_hex == digest, "real asset resolver receives decoded ROM and original path");
            const auto cached = gbarecomp_seam::read_single_line((dir/"asset.cfg").string());
            check(cached == archive.filename().string(), "existing cache stores archive path");
            auto restored = gbarecomp::resolve_asset("", spec, (dir/"game.exe").string());
            check(restored.ok && restored.bytes == body && fs::equivalent(restored.path, archive), "cache survives a fresh resolution");
        }
        pack(dir/"links.zip", false, {{"game.gba", body, true}, {"../escape.gba", wrong}});
        check(!gbarecomp::read_rom_input((dir/"links.zip").string(), digest.c_str(), &got, &error) && got.empty(), "links never become ROMs");
        check(!fs::exists(dir.parent_path()/"escape.gba"), "traversal name never extracted");
        pack(dir/"large.zip", false, {{"game.gba", Bytes(32u*1024u*1024u+1,0)}});
        check(!gbarecomp::read_rom_input((dir/"large.zip").string(), digest.c_str(), &got, &error) && error.find("too large") != std::string::npos, "member size bound");
        std::vector<Member> many(1025, {"small", wrong});
        pack(dir/"many.zip", false, many);
        check(!gbarecomp::read_rom_input((dir/"many.zip").string(), digest.c_str(), &got, &error) && error.find("too many") != std::string::npos, "entry count bound");
        write(dir/"broken.rar", {'R','a','r','!',0x1a,7,0,1,2,3});
        check(!gbarecomp::read_rom_input((dir/"broken.rar").string(), digest.c_str(), &got, &error) && got.empty(), "corrupt archive fails closed");
        pack(dir/"encrypted.zip", false, {{"game.gba", body}}, true);
        check(!gbarecomp::read_rom_input((dir/"encrypted.zip").string(), digest.c_str(), &got, &error) && got.empty() && error.find("encrypted") != std::string::npos, "encrypted archive explained and rejected");
        // Optional upstream RAR4/RAR5 codec fixtures, supplied with matching
        // uncompressed fixture bytes. No game data belongs in these tests.
        for (int i=2; i+1<argc; i+=2) {
            std::ifstream f(fs::u8path(argv[i+1]), std::ios::binary);
            Bytes want((std::istreambuf_iterator<char>(f)), {});
            check(!want.empty(), "RAR expected fixture bytes");
            const auto sha=gba::sha1(want.data(),want.size()).hex();
            check(gbarecomp::read_rom_input(argv[i], sha.c_str(), &got, &error) && got==want, "real upstream RAR fixture matches");
        }
        std::cout << "PASS: raw/ZIP/7z, identity, duplicates, bounds, links, corruption, cache and RAR codec fixtures\n";
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
