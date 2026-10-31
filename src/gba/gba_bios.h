// gba_bios.h — GBA BIOS loader.
//
// Loads and hash-verifies the user's real BIOS dump and exposes its bytes to
// the bus. The maintained LLE configuration executes these bytes for BIOS
// services. Native replacements follow HLE.md; intro playback is a separate
// boot-presentation choice. This loader does not select an implementation.
//
// The expected SHA-1 of the canonical GBA BIOS is hardcoded as a
// default; callers can override it via `load_from_file(path, sha1,...)`
// if they want to point at a different (still hash-verified) image.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace gba {

class GbaBios {
public:
    static constexpr std::size_t kSize = 16 * 1024;
    static constexpr const char* kExpectedSha1 =
        "300c20df6731a33952ded8c436f7f186d25d3492";
    // CRC32 of the canonical GBA BIOS (per zlib / IEEE 802.3). Kept
    // as informational metadata — the picker computes CRC32 of any
    // supplied dump but the gate is SHA-1 alone (CRC32 is a 32-bit
    // hash and SHA-1 is already strictly stronger; carrying both as
    // hard checks just produces extra noise when one of our hardcoded
    // values is wrong).
    static constexpr std::uint32_t kCanonicalCrc32 = 0x81977335u;

    GbaBios();
    ~GbaBios();

    // Load the BIOS from `path`. If `expected_sha1` is non-empty,
    // refuses to load on hash mismatch. Returns true on success;
    // sets *err on failure.
    bool load_from_file(const std::string& path,
                        const std::string& expected_sha1,
                        std::string* err);

    // Load an owned copy of an already-read image, retaining the same size
    // and optional SHA-1 checks as the file loader. The caller may release
    // or modify its source buffer after this call.
    bool load_from_bytes(const uint8_t* bytes, std::size_t size,
                         const std::string& expected_sha1, std::string* err);

    // True if a valid BIOS image is loaded.
    bool loaded() const { return loaded_; }

    // SHA-1 hex of the loaded BIOS bytes (set after a successful
    // load).
    const std::string& sha1_hex() const { return sha1_hex_; }

    // Byte access. `off` is the offset within the 16 KB BIOS image
    // (0x0000..0x3FFF). Out-of-range offsets return 0; the bus is
    // responsible for region classification.
    uint8_t  read8 (uint32_t off) const;
    uint16_t read16(uint32_t off) const;
    uint32_t read32(uint32_t off) const;

private:
    bool finish_load(const std::string& expected_sha1, std::string* err);
    bool loaded_ = false;
    std::array<uint8_t, kSize> bytes_{};
    std::string sha1_hex_;
};

}  // namespace gba
