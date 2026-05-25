// asset_picker.h — Resolve user-supplied assets (BIOS, ROM, save) by
// CLI path -> sidecar cache -> OS file dialog (Win32 only), validating
// each with size + SHA-1 + CRC32. Hashes are WARN-and-try so the user
// can boot with a region/revision we haven't catalogued yet, but a
// wrong size hard-fails.
//
// Mirrors the psxrecomp / TombaRecomp pattern (CRC32 with warn) and
// adds SHA-1 since the existing BIOS / ROM configs already carry it.
// Used by release builds so end users don't have to type --bios /
// --rom every launch; the first validated pick is cached next to the
// executable.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gbarecomp {

struct AssetSpec {
    // Human-facing label, e.g. "GBA BIOS" or "Minish Cap ROM".
    const char* display_name = "asset";
    // Win32 OPENFILENAMEA filter, a double-null-terminated pair list:
    // "GBA BIOS\0*.bin;*.BIN\0All Files\0*.*\0".
    const char* dialog_filter = nullptr;
    // Title bar text for the dialog.
    const char* dialog_title = "Select a file";
    // Sidecar cache filename (lives next to the .exe), e.g. "bios.cfg".
    const char* cache_filename = "asset.cfg";
    // Hard size check, in bytes. 0 = any size accepted.
    std::size_t expected_size = 0;
    // Expected SHA-1, 40-char lowercase hex. nullptr or "" = no check.
    const char* expected_sha1 = nullptr;
    // Expected CRC32 (IEEE 802.3). 0 = no check.
    std::uint32_t expected_crc32 = 0;
};

struct AssetResult {
    bool ok = false;
    std::string path;                  // validated path
    std::vector<unsigned char> bytes;  // file contents (already read)
    std::string sha1_hex;
    std::uint32_t crc32 = 0;
    std::string warning;  // non-empty on hash mismatch (user warned, proceeding)
    std::string error;    // non-empty on hard failure (size / unreadable)
};

// Resolve an asset using the following precedence:
//   1. `argv_path` if non-empty (validate, cache, return).
//   2. Path stored in `<exe_dir>/<spec.cache_filename>` from a prior
//      successful pick (validate, refresh, return).
//   3. Win32 OPENFILENAMEA dialog (loop until cancelled or validated).
//
// `argv0` is the running executable's argv[0]; used to locate the
// sidecar cache. If argv0 is empty, the current working directory is
// used as the cache root.
AssetResult resolve_asset(const std::string& argv_path,
                          const AssetSpec& spec,
                          const std::string& argv0);

}  // namespace gbarecomp
