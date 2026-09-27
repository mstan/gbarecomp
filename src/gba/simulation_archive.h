#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace gba {
// Bounded little-endian archive shared by device/session state. No ABI layout,
// pointers, or unchecked lengths appear on the wire or in state hashes.
template<bool Reading> class SimulationArchive {
public:
    explicit SimulationArchive(std::span<const std::uint8_t> input = {}) : input_(input) {}
    template<class T> void value(T& item) {
        using U = std::remove_cv_t<T>;
        if constexpr (std::is_enum_v<U>) {
            auto v = static_cast<std::underlying_type_t<U>>(item);
            value(v);
            if constexpr (Reading) item = static_cast<U>(v);
        } else if constexpr (std::is_same_v<U, bool>) {
            std::uint8_t v = item ? 1 : 0;
            value(v);
            if (v > 1) throw std::invalid_argument("invalid state boolean");
            if constexpr (Reading) item = v != 0;
        } else if constexpr (std::is_integral_v<U>) {
            using Bits = std::make_unsigned_t<U>;
            Bits bits = static_cast<Bits>(item);
            if constexpr (Reading) {
                if (remaining() < sizeof(U)) throw std::invalid_argument("truncated simulation state");
                bits = 0;
                for (unsigned i = 0; i < sizeof(U); ++i)
                    bits |= static_cast<Bits>(input_[cursor_++]) << (8 * i);
                item = std::bit_cast<U>(bits);
            } else {
                for (unsigned i = 0; i < sizeof(U); ++i) output_.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
            }
        } else {
            for (auto& element : item) value(element); // fixed arrays only
        }
    }
    template<class... T> void operator()(T&... items) { (value(items), ...); }
    template<class T> void blob(T& bytes, std::size_t count) {
        if (count > bytes.size()) throw std::invalid_argument("invalid state region");
        if constexpr (Reading) {
            if (remaining() < count) throw std::invalid_argument("truncated state region");
            std::copy_n(input_.data() + cursor_, count, bytes.data());
            cursor_ += count;
        } else {
            output_.insert(output_.end(), bytes.data(), bytes.data() + count);
        }
    }
    template<class T> void vector(T& bytes, std::uint32_t maximum) {
        if constexpr (!Reading)
            if (bytes.size() > maximum) throw std::invalid_argument("state vector exceeds bound");
        std::uint32_t count = static_cast<std::uint32_t>(bytes.size());
        value(count);
        if (count > maximum) throw std::invalid_argument("state vector exceeds bound");
        if constexpr (Reading) {
            if (remaining() < count) throw std::invalid_argument("truncated state vector");
            bytes.resize(count);
        }
        blob(bytes, count);
    }
    template<class T> void identity(const T& expected) {
        auto actual = expected;
        value(actual);
        if constexpr (Reading)
            if (actual != expected) throw std::invalid_argument("simulation state identity mismatch");
    }
    void text_identity(const std::string& expected) {
        std::vector<std::uint8_t> value(expected.begin(), expected.end());
        vector(value, 1024);
        if constexpr (Reading)
            if (std::string(value.begin(), value.end()) != expected)
                throw std::invalid_argument("program identity mismatch");
    }
    std::size_t remaining() const { return input_.size() - cursor_; }
    const std::vector<std::uint8_t>& bytes() const { return output_; }
    std::vector<std::uint8_t> take() { return std::move(output_); }
private:
    std::span<const std::uint8_t> input_;
    std::size_t cursor_ = 0;
    std::vector<std::uint8_t> output_;
};
} // namespace gba
