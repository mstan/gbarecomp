#include "codebreaker_memory.h"
#include <recomp_mod.h>
#include <algorithm>
#include <cstring>
#include <map>
#include <optional>

namespace gbarecomp::codebreaker {
namespace {
using Key = std::uint64_t;
constexpr Key kCart = Key{1} << 63;
FrameMemory* snapshot_session = nullptr;
bool snapshot_registered = false;
bool cartridge(std::uint32_t address) { return address >= 0x08000000u && address < 0x0e000000u; }
std::optional<Key> key(std::uint32_t address) {
    switch (address >> 24) {
        case 2: return 0x02000000u | (address & 0x3ffffu);
        case 3: return 0x03000000u | (address & 0x7fffu);
        case 5: return 0x05000000u | (address & 0x3ffu);
        case 6: {
            auto off = address & 0x1ffffu;
            if (off >= 0x18000u) off -= 0x8000u;
            return 0x06000000u | off;
        }
        case 7: return 0x07000000u | (address & 0x3ffu);
        default: return cartridge(address) ? std::optional<Key>(kCart | (address & 0x1ffffffu)) : std::nullopt;
    }
}
bool conditional(OpKind op) { return op >= OpKind::IfEqual; }
std::string hex(const CartridgeModState::Identity& value) {
    const char* digits = "0123456789abcdef";
    std::string result;
    for (auto byte : value) { result += digits[byte >> 4]; result += digits[byte & 15]; }
    return result;
}
void word(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) out.push_back(std::uint8_t(value >> shift));
}
template<class F> bool writes(const Op& op, F emit) {
    auto half = [&](std::uint32_t address, std::optional<std::uint16_t> value) {
        return emit(address, value ? std::optional<std::uint8_t>(*value & 255) : std::nullopt) &&
            emit(address + 1, value ? std::optional<std::uint8_t>(*value >> 8) : std::nullopt);
    };
    switch (op.kind) {
        case OpKind::Write8: return emit(op.address, std::uint8_t(op.operand));
        case OpKind::Write16: return half(op.address, std::uint16_t(op.operand));
        case OpKind::Slide16:
            for (std::uint32_t i = 0; i < op.count; ++i)
                if (!half(op.address + i * op.address_step, std::uint16_t(op.operand + i * op.operand_step))) return false;
            return true;
        case OpKind::FillList16:
            for (std::size_t i = 0; i < op.values.size(); ++i)
                if (!half(op.address + std::uint32_t(i * 2), op.values[i])) return false;
            return true;
        case OpKind::Or16: case OpKind::And16: case OpKind::Add16:
            return half(op.address, std::nullopt);
        default: return true;
    }
}
}

struct FrameMemory::Impl {
    struct Byte { std::uint8_t before, after; };
    std::vector<NamedProgram> programs;
    Memory bus{};
    std::span<std::uint8_t> image;
    std::unique_ptr<CartridgeModState> cart;
    std::unique_ptr<rmod_context, decltype(&rmod_destroy)> core{nullptr, rmod_destroy};
    std::string game, original, plan, failure;
    std::map<Key, Byte> frame;
    std::map<Key, std::uint8_t> local;
    bool did_change = false;

    std::uint8_t raw_read(Key address) const {
        if (address & kCart) return image[std::size_t(address & ~kCart)];
        const auto a = std::uint32_t(address);
        return std::uint8_t(bus.read16(bus.ctx, a & ~1u) >> ((a & 1u) * 8));
    }
    bool valid(Key address) const { return !(address & kCart) || (address & ~kCart) < image.size(); }
    std::uint8_t read(std::uint32_t address) {
        const auto mapped = key(address);
        if (!mapped || !valid(*mapped)) { failure = "Cheat read exceeds its mapped image or memory region"; return 0; }
        if (const auto it = local.find(*mapped); it != local.end()) return it->second;
        if (const auto it = frame.find(*mapped); it != frame.end()) return it->second.after;
        return raw_read(*mapped);
    }
    void write(std::uint32_t address, std::uint8_t value) {
        const auto mapped = key(address);
        if (!mapped || !valid(*mapped)) { failure = "Cheat write exceeds its mapped image or memory region"; return; }
        local[*mapped] = value;
    }
};

FrameMemory::FrameMemory() : impl_(std::make_unique<Impl>()) {}
FrameMemory::~FrameMemory() {
    if (snapshot_session == this) snapshot_session = nullptr;
}
bool FrameMemory::bind_snapshots(std::string* error) {
    if (snapshot_session && snapshot_session != this) {
        if (error) *error = "Another cheat memory session owns save/load";
        return false;
    }
    if (!snapshot_registered) {
        const debug::ModStateProvider provider{"gbarecomp.codebreaker.session", 1, nullptr,
            +[](void*, debug::SnapshotWriter& out, std::string* e) {
                if (!snapshot_session) { if (e) *e = "Cheat memory session is not active"; return false; }
                auto* cart = snapshot_session->cartridge_state(); out.boolean(cart != nullptr);
                if (!cart) return true;
                const auto p = cart->snapshot_provider(); return p.save(p.user, out, e);
            },
            +[](void*, debug::SnapshotReader& in, std::string* e) {
                auto* cart = snapshot_session ? snapshot_session->cartridge_state() : nullptr;
                const auto active = in.u8();
                if (!snapshot_session || active > 1 || bool(active) != (cart != nullptr) || !in.ok()) {
                    if (e) *e = "Snapshot requires a different cartridge cheat session";
                    return false;
                }
                if (!cart) return in.remaining() == 0;
                const auto p = cart->snapshot_provider(); return p.preflight(p.user, in, e);
            },
            +[](void*, debug::SnapshotReader& in) {
                if (in.u8()) {
                    auto* cart = snapshot_session->cartridge_state();
                    const auto p = cart->snapshot_provider(); p.restore(p.user, in);
                }
            }};
        if (!debug::register_mod_state_provider(provider, error)) return false;
        snapshot_registered = true;
    }
    snapshot_session = this;
    return true;
}
bool FrameMemory::uses_cartridge(const Program& program) {
    for (const auto& op : program.ops) {
        if (cartridge(op.address)) return true;
        bool found = false;
        writes(op, [&](auto address, auto) { found |= cartridge(address); return !found; });
        if (found) return true;
    }
    return false;
}

bool FrameMemory::initialize(std::vector<NamedProgram> programs, Memory bus,
        std::span<std::uint8_t> image, const RuntimeNativeImageDependencies* dependencies,
        const std::string& game_id, CartridgeModState::Identity original,
        std::string* error) {
    auto p = std::make_unique<Impl>();
    auto fail = [&](const std::string& text) { if (error) *error = text; return false; };
    if (impl_->core) return fail("The cheat execution plan is already bound");
    if (!bus.read16 || !bus.write8 || !bus.write16 || image.empty()) return fail("Incomplete mod memory binding");
    p->programs = std::move(programs); p->bus = bus; p->image = image;
    p->game = game_id; p->original = hex(original);
    std::vector<std::uint8_t> description;
    word(description, std::uint32_t(game_id.size()));
    description.insert(description.end(), game_id.begin(), game_id.end());
    CartridgeModState::Identity baseline{}, identity{};
    if (!rmod_sha256_bytes(image.data(), image.size(), baseline.data())) return fail("Cannot hash loaded image");
    description.insert(description.end(), baseline.begin(), baseline.end());
    struct Claim { std::optional<std::uint8_t> value; std::string owner; };
    std::map<Key, Claim> claimed;
    std::vector<std::uint32_t> cart_offsets;
    for (const auto& named : p->programs) {
        word(description, std::uint32_t(named.id.size()));
        description.insert(description.end(), named.id.begin(), named.id.end());
        word(description, std::uint32_t(named.program.ops.size()));
        std::map<Key, std::optional<std::uint8_t>> own;
        for (std::size_t i = 0; i < named.program.ops.size(); ++i) {
            const auto& op = named.program.ops[i];
            word(description, std::uint32_t(op.kind)); word(description, op.address); word(description, op.operand);
            word(description, op.count); word(description, op.address_step); word(description, op.operand_step);
            word(description, std::uint32_t(op.values.size()));
            for (auto value : op.values) word(description, value);
            const bool guarded = i && conditional(named.program.ops[i - 1].kind);
            if (!writes(op, [&](std::uint32_t address, std::optional<std::uint8_t> value) {
                auto mapped = key(address);
                if (!mapped || !p->valid(*mapped)) return false;
                auto it = own.find(*mapped);
                if (it == own.end()) own.emplace(*mapped, value);
                else if (!guarded) it->second = value;
                else if (it->second != value) it->second.reset();
                return true;
            })) return fail(named.label + ": write exceeds its mapped image or memory region");
        }
        for (const auto& [address, value] : own) {
            if (const auto old = claimed.find(address); old != claimed.end()) {
                if (!value || !old->second.value || value != old->second.value)
                    return fail(named.label + " conflicts with " + old->second.owner + "; select one in Mods");
            } else claimed.emplace(address, Claim{value, named.label});
        }
    }
    for (const auto& [address, claim] : claimed)
        if (address & kCart) cart_offsets.push_back(std::uint32_t(address & ~kCart));
    if (!rmod_sha256_bytes(description.data(), description.size(), identity.data())) return fail("Cannot hash resolved cheat plan");
    p->plan = hex(identity);
    if (!cart_offsets.empty()) {
        p->cart = std::make_unique<CartridgeModState>();
        const auto ranges = dependencies ? std::span(dependencies->ranges, dependencies->count)
                                         : std::span<const RuntimeNativeImageRange>{};
        if (!p->cart->bind(image, cart_offsets, ranges, dependencies && dependencies->complete, original, identity, error)) return false;
        for (const auto& [address, claim] : claimed) if (address & kCart) {
            const auto offset = address & ~kCart;
            const auto original_byte = image[std::size_t(offset)];
            const auto probe = std::uint8_t(original_byte ^ 1);
            if (!p->cart->can_write(offset, std::span(&probe, 1))) {
                if (claim.value && *claim.value != original_byte)
                    return fail("A selected cartridge operation changes compiled code; an exact native program profile is required");
            }
        }
    }
    p->core.reset(rmod_create());
    if (!p->core || !rmod_bind_plan(p->core.get(), p->game.c_str(), p->original.c_str(), p->plan.c_str()))
        return fail("Cannot bind the guarded cheat transaction");
    impl_.swap(p);
    return true;
}

bool FrameMemory::run(std::string* error) {
    auto& p = *impl_;
    auto fail = [&](const std::string& text) { if (error) *error = text; return false; };
    if (!p.core) return fail("Cheat memory transaction is unbound");
    p.frame.clear(); p.local.clear(); p.failure.clear(); p.did_change = false;
    Memory shadow{&p,
        +[](void* raw, std::uint32_t address) -> std::uint16_t {
            auto& s = *static_cast<Impl*>(raw);
            if (address == kKeyInputAddress) return s.bus.read16(s.bus.ctx, address);
            const auto low = s.read(address), high = s.read(address + 1);
            return std::uint16_t(low | (std::uint16_t(high) << 8));
        },
        +[](void* raw, std::uint32_t address, std::uint8_t value) { static_cast<Impl*>(raw)->write(address, value); },
        +[](void* raw, std::uint32_t address, std::uint16_t value) {
            auto& s = *static_cast<Impl*>(raw); s.write(address, value & 255); s.write(address + 1, value >> 8);
        }};
    for (const auto& named : p.programs) {
        p.local.clear();
        try { codebreaker::run(named.program, shadow); }
        catch (...) { return fail("Cheat memory read failed before transaction commit"); }
        if (!p.failure.empty()) return fail(p.failure);
        for (const auto& [address, value] : p.local) {
            const auto old = p.frame.find(address);
            if (old != p.frame.end()) {
                if (old->second.after != value) return fail("Enabled cheat programs resolved conflicting writes");
            } else p.frame.emplace(address, Impl::Byte{p.raw_read(address), value});
        }
    }
    struct Batch { const char* space; std::uint64_t address; std::vector<std::uint8_t> before, after; };
    std::vector<Batch> batches;
    Key previous = ~Key{0};
    for (const auto& [address, bytes] : p.frame) {
        if (bytes.before == bytes.after) continue;
        // Bound the number of claims even for long strided codes. Unchanged
        // bytes between writes retain expected-byte guards in the same32KiB
        // chunk; no claim crosses a physical memory-region boundary.
        if (batches.empty() || (address >> 15) != (previous >> 15))
            batches.push_back({address & kCart ? "cartridge" : "ram", address & ~kCart, {}, {}});
        else for (Key gap = previous + 1; gap < address; ++gap) {
            const auto value = p.raw_read(gap);
            batches.back().before.push_back(value); batches.back().after.push_back(value);
        }
        batches.back().before.push_back(bytes.before); batches.back().after.push_back(bytes.after); previous = address;
    }
    std::vector<rmod_patch> patches;
    for (auto& batch : batches) {
        if (!std::strcmp(batch.space, "cartridge") &&
                (!p.cart || !p.cart->can_write(batch.address, batch.after, error))) return false;
        patches.push_back({batch.space, batch.address, batch.before.data(), batch.after.data(), batch.after.size()});
    }
    if (patches.empty()) return true;
    rmod_engine engine{};
    engine.user = &p; engine.game_id = p.game.c_str(); engine.original_image_sha256 = p.original.c_str();
    engine.begin = +[](void*) { return 1; }; // Caller owns the guest frame boundary.
    engine.read = +[](void* raw, const char* space, std::uint64_t address, std::uint8_t* data, std::size_t size) {
        auto& s = *static_cast<Impl*>(raw);
        if (!std::strcmp(space, "cartridge")) return int(s.cart && s.cart->read(address, {data, size}));
        if (std::strcmp(space, "ram")) return 0;
        try { for (std::size_t i = 0; i < size; ++i) data[i] = s.raw_read(address + i); }
        catch (...) { return 0; }
        return 1;
    };
    engine.write = +[](void* raw, const char* space, std::uint64_t address, const std::uint8_t* data, std::size_t size) {
        auto& s = *static_cast<Impl*>(raw);
        if (!std::strcmp(space, "cartridge")) return int(s.cart && s.cart->write(address, {data, size}));
        if (std::strcmp(space, "ram")) return 0;
        try { for (std::size_t i = 0; i < size; ++i) s.bus.write8(s.bus.ctx, std::uint32_t(address + i), data[i]); }
        catch (...) { return 0; }
        return 1;
    };
    engine.finish = +[](void*, int) {};
    if (rmod_apply(p.core.get(), patches.data(), patches.size(), nullptr, 0, &engine, nullptr) != 1)
        return fail(rmod_error(p.core.get()));
    p.did_change = true;
    return true;
}

CartridgeModState* FrameMemory::cartridge_state() { return impl_->cart.get(); }
const std::string& FrameMemory::plan_fingerprint() const { return impl_->plan; }
bool FrameMemory::changed() const { return impl_->did_change; }
} // namespace gbarecomp::codebreaker
