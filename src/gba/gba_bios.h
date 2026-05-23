// gba_bios.h — GBA BIOS loader.
//
// Per PRINCIPLES.md "BIOS is sacred": this class loads the user's
// dump of the real GBA BIOS, hash-verifies it, and exposes the bytes
// for the bus to read. We never HLE SWIs, never stub BIOS code, and
// never skip the BIOS intro. The interpreter executes BIOS bytes the
// same way it executes cartridge bytes — that's the whole point.
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

    GbaBios();
    ~GbaBios();

    // Load the BIOS from `path`. If `expected_sha1` is non-empty,
    // refuses to load on hash mismatch. Returns true on success;
    // sets *err on failure.
    bool load_from_file(const std::string& path,
                        const std::string& expected_sha1,
                        std::string* err);

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
    bool loaded_ = false;
    std::array<uint8_t, kSize> bytes_{};
    std::string sha1_hex_;
};

}  // namespace gba
