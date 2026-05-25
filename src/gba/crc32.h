// crc32.h — IEEE 802.3 CRC32 (poly 0xEDB88320), the same flavor PNG /
// zip / Ethernet use. Pairs with sha1.h for asset validation in
// release builds. Used by the BIOS / ROM pickers — release runtimes
// quote both CRC32 and SHA-1 for the user when an asset doesn't match.

#pragma once

#include <cstddef>
#include <cstdint>

namespace gba {

// Hash a byte buffer with CRC32-IEEE. Result matches `crc32` from
// `zlib` and the `crc32` field in PNG / zip headers.
uint32_t crc32(const void* data, std::size_t len);

}  // namespace gba
