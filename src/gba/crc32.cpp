// crc32.cpp — IEEE 802.3 CRC32. Table is computed on first use.

#include "crc32.h"

namespace gba {

namespace {

constexpr uint32_t kPoly = 0xEDB88320u;

struct Table {
    uint32_t v[256];
    constexpr Table() : v{} {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (kPoly ^ (c >> 1)) : (c >> 1);
            }
            v[i] = c;
        }
    }
};

constexpr Table kTable{};

}  // namespace

uint32_t crc32(const void* data, std::size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        c = kTable.v[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

}  // namespace gba
