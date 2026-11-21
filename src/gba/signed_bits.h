#pragma once

#include <cstring>
#include <type_traits>

namespace gba {

// Interpret the complete unsigned register/sample representation as the matching
// signed integer. Unlike an out-of-range numeric cast this preserves every bit;
// memcpy also works with supported C++20 libraries that do not have bit_cast.
template<class Signed, class Unsigned>
inline Signed signed_bits(Unsigned bits) {
    static_assert(std::is_integral_v<Signed> && std::is_signed_v<Signed>);
    static_assert(std::is_integral_v<Unsigned> && std::is_unsigned_v<Unsigned>);
    static_assert(sizeof(Signed) == sizeof(Unsigned));
    Signed value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

}  // namespace gba
