// gba_bios.cpp — see gba_bios.h.

#include "gba_bios.h"

#include <cstdio>
#include <cstring>

#include "sha1.h"

namespace gba {

GbaBios::GbaBios()  = default;
GbaBios::~GbaBios() = default;

bool GbaBios::load_from_file(const std::string& path,
                             const std::string& expected_sha1,
                             std::string* err) {
    loaded_ = false;
    sha1_hex_.clear();

    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (err) *err = "open failed: " + path;
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz < 0 || static_cast<std::size_t>(sz) != kSize) {
        if (err) {
            *err = "BIOS size mismatch: expected " +
                   std::to_string(kSize) + " bytes, got " +
                   (sz < 0 ? "<seek failed>" : std::to_string(sz));
        }
        std::fclose(f);
        return false;
    }
    std::size_t got = std::fread(bytes_.data(), 1, kSize, f);
    std::fclose(f);
    if (got != kSize) {
        if (err) *err = "short read from BIOS file: " + path;
        return false;
    }

    auto digest = sha1(bytes_.data(), kSize);
    sha1_hex_ = digest.hex();
    if (!expected_sha1.empty() && sha1_hex_ != expected_sha1) {
        if (err) {
            *err = "BIOS SHA-1 mismatch: got " + sha1_hex_ +
                   " expected " + expected_sha1;
        }
        return false;
    }

    loaded_ = true;
    return true;
}

uint8_t GbaBios::read8(uint32_t off) const {
    if (!loaded_ || off >= kSize) return 0;
    return bytes_[off];
}

uint16_t GbaBios::read16(uint32_t off) const {
    if (!loaded_ || off + 1 >= kSize) return 0;
    const uint8_t* p = &bytes_[off];
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t GbaBios::read32(uint32_t off) const {
    if (!loaded_ || off + 3 >= kSize) return 0;
    const uint8_t* p = &bytes_[off];
    return static_cast<uint32_t>(p[0] |
                                 (p[1] << 8) |
                                 (p[2] << 16) |
                                 (p[3] << 24));
}

}  // namespace gba
