#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace gba {

// Presentation-owned changes at raster observation boundaries. The live bus
// remains authoritative. Payloads own their bytes, so later stores cannot
// change an earlier draw's inputs. No device time, GPU handle or guest state
// belongs to this journal.
class GbaDrawMemory {
public:
    static constexpr std::size_t kVramBytes = 96u * 1024u;
    static constexpr std::size_t kBlockBytes = 32u;
    struct Range {
        std::uint32_t offset = 0, size = 0, payload_offset = 0;
    };
    struct Delta {
        std::uint64_t before = 0, after = 0;
        bool reset = false;
        std::vector<Range> ranges;
        std::vector<std::uint8_t> payload;
    };

    bool mark(std::size_t offset, std::size_t size) {
        if (offset > kVramBytes || size > kVramBytes - offset) return false;
        if (!size) return true;
        const auto last = (offset + size - 1u) / kBlockBytes;
        for (auto block = offset / kBlockBytes; block <= last; ++block)
            dirty_[block / 64u] |= std::uint64_t{1} << (block % 64u);
        return true;
    }

    void invalidate() { initialized_ = false; }
    std::uint64_t sequence() const { return sequence_; }

    Delta publish(std::span<const std::uint8_t> live) {
        if (live.size() != kVramBytes)
            throw std::invalid_argument("invalid draw-memory view");
        Delta delta;
        delta.before = delta.after = sequence_;
        if (!initialized_) {
            if (sequence_ == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("draw-memory sequence overflow");
            delta.reset = true;
            delta.ranges.push_back({0, std::uint32_t(kVramBytes), 0});
            delta.payload.assign(live.begin(), live.end());
            std::copy(live.begin(), live.end(), shadow_.begin());
        } else {
            for (std::size_t word = 0; word < dirty_.size(); ++word) {
                auto bits = dirty_[word];
                while (bits) {
                    const auto block = word * 64u + std::countr_zero(bits);
                    bits &= bits - 1u;
                    const auto offset = block * kBlockBytes;
                    if (!std::memcmp(shadow_.data() + offset,
                                     live.data() + offset, kBlockBytes)) continue;
                    if (sequence_ == std::numeric_limits<std::uint64_t>::max())
                        throw std::overflow_error("draw-memory sequence overflow");
                    if (!delta.ranges.empty() &&
                        delta.ranges.back().offset + delta.ranges.back().size == offset)
                        delta.ranges.back().size += std::uint32_t(kBlockBytes);
                    else
                        delta.ranges.push_back({std::uint32_t(offset),
                            std::uint32_t(kBlockBytes), std::uint32_t(delta.payload.size())});
                    delta.payload.insert(delta.payload.end(), live.begin() + offset,
                                         live.begin() + offset + kBlockBytes);
                }
            }
            // Commit only after the complete owned transaction has allocated.
            // If allocation throws, a later publication must still see every
            // dirty byte rather than silently losing a partially built delta.
            for (const auto& range : delta.ranges)
                std::memcpy(shadow_.data() + range.offset,
                            delta.payload.data() + range.payload_offset, range.size);
        }
        if (!delta.ranges.empty()) {
            delta.after = ++sequence_;
        }
        dirty_.fill(0);
        initialized_ = true;
        return delta;
    }

    // Validate a complete transaction before updating the portable consumer.
    // A fresh/context-lost consumer can accept a reset at any prior sequence.
    static bool apply(const Delta& delta, std::span<std::uint8_t> memory,
                      std::uint64_t& sequence) {
        if (memory.size() != kVramBytes || delta.payload.size() > kVramBytes)
            return false;
        if (delta.ranges.empty())
            return !delta.reset && delta.payload.empty() &&
                   delta.before == sequence && delta.after == sequence;
        if (delta.before == std::numeric_limits<std::uint64_t>::max() ||
            delta.after != delta.before + 1u ||
            (!delta.reset && delta.before != sequence)) return false;
        std::size_t end = 0, used = 0;
        for (const auto& range : delta.ranges) {
            if (!range.size || range.offset < end || range.offset > kVramBytes ||
                range.size > kVramBytes - range.offset ||
                range.payload_offset != used || range.size > delta.payload.size() - used)
                return false;
            end = std::size_t(range.offset) + range.size;
            used += range.size;
        }
        if (used != delta.payload.size()) return false;
        if (delta.reset && (delta.ranges.size() != 1 ||
            delta.ranges[0].offset != 0 || delta.ranges[0].size != kVramBytes)) return false;
        for (const auto& range : delta.ranges)
            std::memcpy(memory.data() + range.offset,
                        delta.payload.data() + range.payload_offset, range.size);
        sequence = delta.after;
        return true;
    }

private:
    std::array<std::uint8_t, kVramBytes> shadow_{};
    std::array<std::uint64_t, kVramBytes / kBlockBytes / 64u> dirty_{};
    std::uint64_t sequence_ = 0;
    bool initialized_ = false;
};

} // namespace gba
