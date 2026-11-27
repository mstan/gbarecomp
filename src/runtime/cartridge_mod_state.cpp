#include "cartridge_mod_state.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace gbarecomp {
namespace {
constexpr std::size_t kHeaderBytes = 32 + 32 + 4;
bool fail(std::string* error, const char* text) {
    if (error) *error = text;
    return false;
}
}

bool CartridgeModState::bind(std::span<std::uint8_t> image,
        std::span<const std::uint32_t> writable_offsets,
        std::span<const CartridgeNativeRange> native_ranges,
        bool native_map_complete, Identity target, Identity plan,
        std::string* error) {
    if (bound_) return fail(error, "The cartridge mod plan is already bound");
    if (!native_map_complete)
        return fail(error, "Native cartridge dependencies are unavailable; regenerate this game");
    if (image.empty() || image.size() > std::numeric_limits<std::uint32_t>::max())
        return fail(error, "Invalid loaded cartridge size");
    std::vector<std::uint32_t> offsets(writable_offsets.begin(), writable_offsets.end());
    std::sort(offsets.begin(), offsets.end());
    offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
    // The session provider adds a one-byte active marker around this payload.
    if (offsets.size() > debug::ModStateRegistry::kMaxProviderPayload - kHeaderBytes - 1)
        return fail(error, "The cartridge write footprint exceeds the mod-state payload limit");
    std::vector<CartridgeNativeRange> ranges(native_ranges.begin(), native_ranges.end());
    std::sort(ranges.begin(), ranges.end(), [](auto a, auto b) { return a.offset < b.offset; });
    std::uint64_t end = 0;
    for (auto range : ranges) {
        if (!range.size || range.offset < end || range.offset > image.size() ||
                range.size > image.size() - range.offset)
            return fail(error, "Invalid native cartridge dependency range");
        end = std::uint64_t(range.offset) + range.size;
    }
    std::vector<Slot> slots;
    slots.reserve(offsets.size());
    auto range = ranges.begin();
    for (auto offset : offsets) {
        if (offset >= image.size()) return fail(error, "Cartridge write exceeds the loaded image");
        while (range != ranges.end() && std::uint64_t(range->offset) + range->size <= offset) ++range;
        const bool native = range != ranges.end() && range->offset <= offset;
        slots.push_back({offset, image[offset], native});
    }
    image_ = image;
    slots_.swap(slots);
    target_ = target;
    plan_ = plan;
    bound_ = true;
    return true;
}

const CartridgeModState::Slot* CartridgeModState::slot(std::uint32_t offset) const noexcept {
    const auto it = std::lower_bound(slots_.begin(), slots_.end(), offset,
        [](const Slot& value, std::uint32_t key) { return value.offset < key; });
    return it != slots_.end() && it->offset == offset ? &*it : nullptr;
}

bool CartridgeModState::read(std::uint64_t offset, std::span<std::uint8_t> out) const noexcept {
    if (!bound_ || offset > image_.size() || out.size() > image_.size() - offset) return false;
    if (!out.empty()) std::memmove(out.data(), image_.data() + offset, out.size());
    return true;
}

bool CartridgeModState::can_write(std::uint64_t offset,
        std::span<const std::uint8_t> bytes, std::string* error) const {
    if (!bound_ || offset > image_.size() || bytes.size() > image_.size() - offset)
        return fail(error, "Cartridge write exceeds the loaded image");
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const auto* entry = slot(static_cast<std::uint32_t>(offset + i));
        if (!entry) {
            if (bytes[i] == image_[std::size_t(offset) + i]) continue;
            return fail(error, "Cartridge write is outside the resolved mod footprint");
        }
        if (entry->native && bytes[i] != entry->original)
            return fail(error, "This write changes compiled native code; an exact native program profile is required");
    }
    return true;
}

bool CartridgeModState::write(std::uint64_t offset,
        std::span<const std::uint8_t> bytes) noexcept {
    if (!can_write(offset, bytes)) return false;
    if (!bytes.empty()) std::memmove(image_.data() + offset, bytes.data(), bytes.size());
    return true;
}

bool CartridgeModState::save(debug::SnapshotWriter& out, std::string* error) const {
    if (!bound_) return fail(error, "Cartridge mod state is unbound");
    out.bytes(target_.data(), target_.size());
    out.bytes(plan_.data(), plan_.size());
    out.u32(static_cast<std::uint32_t>(slots_.size()));
    for (const auto& entry : slots_) out.u8(image_[entry.offset]);
    return true;
}

bool CartridgeModState::preflight(debug::SnapshotReader& in, std::string* error) const {
    Identity target{}, plan{};
    in.bytes(target.data(), target.size());
    in.bytes(plan.data(), plan.size());
    const auto count = in.u32();
    if (!bound_ || !in.ok() || target != target_ || plan != plan_ ||
            count != slots_.size() || in.remaining() != count)
        return fail(error, "Cartridge snapshot target or mod plan differs");
    for (const auto& entry : slots_) {
        const auto value = in.u8();
        if (entry.native && value != entry.original)
            return fail(error, "Cartridge snapshot would change compiled native code");
    }
    return in.ok();
}

void CartridgeModState::restore(debug::SnapshotReader& in) noexcept {
    in.skip(kHeaderBytes);
    for (const auto& entry : slots_) image_[entry.offset] = in.u8();
}

debug::ModStateProvider CartridgeModState::snapshot_provider() {
    return {"gbarecomp.codebreaker.cartridge", 1, this,
        +[](void* user, debug::SnapshotWriter& out, std::string* error) {
            return static_cast<CartridgeModState*>(user)->save(out, error);
        },
        +[](void* user, debug::SnapshotReader& in, std::string* error) {
            return static_cast<CartridgeModState*>(user)->preflight(in, error);
        },
        +[](void* user, debug::SnapshotReader& in) {
            static_cast<CartridgeModState*>(user)->restore(in);
        }};
}
} // namespace gbarecomp
