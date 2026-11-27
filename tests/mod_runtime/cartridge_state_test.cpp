#include "cartridge_mod_state.h"
#include <recomp_mod.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>

using gbarecomp::CartridgeModState;
using gbarecomp::CartridgeNativeRange;
namespace db = gbarecomp::debug;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line%d: %s\n", __LINE__, #x); ++failures; } } while (0)
std::string hex(const CartridgeModState::Identity& id) {
    const char* digits = "0123456789abcdef";
    std::string out;
    for (auto v : id) { out += digits[v >> 4]; out += digits[v & 15]; }
    return out;
}
struct Engine {
    CartridgeModState& cart;
    std::array<std::uint8_t, 8> ram{};
    bool fail_once = false, committed = false;
    unsigned writes = 0;
    rmod_engine callbacks(const std::string& hash) {
        return {this, "test.cartridge", hash.c_str(),
            +[](void*) { return 1; },
            +[](void* p, const char* space, std::uint64_t offset, std::uint8_t* data, std::size_t size) {
                auto& e = *static_cast<Engine*>(p);
                if (!std::strcmp(space, "cartridge")) return int(e.cart.read(offset, {data, size}));
                if (std::strcmp(space, "ram") || offset > e.ram.size() || size > e.ram.size() - offset) return 0;
                std::memcpy(data, e.ram.data() + offset, size); return 1;
            },
            +[](void* p, const char* space, std::uint64_t offset, const std::uint8_t* data, std::size_t size) {
                auto& e = *static_cast<Engine*>(p); ++e.writes;
                if (!std::strcmp(space, "cartridge")) return int(e.cart.write(offset, {data, size}));
                if (std::strcmp(space, "ram") || offset > e.ram.size() || size > e.ram.size() - offset) return 0;
                if (e.fail_once) { e.fail_once = false; if (size) e.ram[offset] = data[0]; return 0; }
                std::memcpy(e.ram.data() + offset, data, size); return 1;
            }, nullptr,
            +[](void* p, int committed) { static_cast<Engine*>(p)->committed = committed != 0; }};
    }
};
}
int main() {
    const std::vector<std::uint8_t> original(512, 0x31);
    auto loaded = original;
    const std::array<std::uint32_t, 6> offsets{0xa1, 0, 0xa0, 0xa0, 1, 511};
    const std::array<CartridgeNativeRange, 1> code{{{0, 4}}};
    CartridgeModState::Identity target{}, plan{};
    CHECK(rmod_sha256_bytes(original.data(), original.size(), target.data()));
    CHECK(rmod_sha256_bytes("resolved.test", 13, plan.data()));
    const auto target_hex = hex(target), plan_hex = hex(plan);
    std::string error;
    CartridgeModState cart;
    CHECK(!cart.bind(loaded, offsets, code, false, target, plan, &error));
    CHECK(cart.bind(loaded, offsets, code, true, target, plan, &error));
    CHECK(cart.footprint() == 5);
    CHECK(!cart.bind(loaded, offsets, code, true, target, plan));
    CHECK(!cart.can_write(510, std::array<std::uint8_t, 1>{1}));
    CHECK(!cart.can_write(511, std::array<std::uint8_t, 2>{1, 2}));
    CHECK(cart.can_write(0, std::array<std::uint8_t, 1>{0x31}));
    CHECK(!cart.can_write(0, std::array<std::uint8_t, 1>{0x32}));

    db::ModStateRegistry registry;
    CHECK(registry.register_provider(cart.snapshot_provider(), &error));
    db::SnapshotWriter baseline;
    CHECK(registry.serialize(baseline, &error));
    auto context = std::unique_ptr<rmod_context, decltype(&rmod_destroy)>(rmod_create(), rmod_destroy);
    CHECK(context != nullptr);
    CHECK(rmod_bind_plan(context.get(), "test.cartridge", target_hex.c_str(), plan_hex.c_str()));
    Engine engine{cart};
    auto callbacks = engine.callbacks(target_hex);
    const std::array<std::uint8_t, 2> expected{0x31, 0x31}, replacement{0x48, 0x58};
    const rmod_patch patch{"cartridge", 0xa0, expected.data(), replacement.data(), 2};
    std::array<std::uint8_t, 32> first_fingerprint{}, repeated_fingerprint{};
    CHECK(rmod_apply(context.get(), &patch, 1, nullptr, 0, &callbacks, first_fingerprint.data()) == 1);
    CHECK(engine.committed && loaded[0xa0] == 0x48 && loaded[0xa1] == 0x58);
    CHECK(original[0xa0] == 0x31 && original[0xa1] == 0x31);

    db::SnapshotWriter changed;
    CHECK(registry.serialize(changed, &error));
    const auto before_preflight = loaded;
    db::SnapshotReader check(changed.buffer().data(), changed.size());
    CHECK(registry.preflight(check, &error));
    CHECK(loaded == before_preflight);
    CHECK(cart.write(0xa0, std::array<std::uint8_t, 2>{0x59, 0x5a}));
    db::SnapshotReader restore(changed.buffer().data(), changed.size());
    CHECK(registry.restore(restore, &error));
    CHECK(loaded[0xa0] == 0x48 && loaded[0xa1] == 0x58);

    // Both address spaces roll back even when the failed writer changed bytes.
    const std::array<std::uint8_t, 2> later{0x61, 0x62}, ram_before{}, ram_after{7, 9};
    const std::array<rmod_patch, 2> failing{{
        {"cartridge", 0xa0, replacement.data(), later.data(), 2},
        {"ram", 4, ram_before.data(), ram_after.data(), 2}}};
    engine.fail_once = true;
    CHECK(rmod_apply(context.get(), failing.data(), failing.size(), nullptr, 0, &callbacks, nullptr) == 0);
    CHECK(!engine.committed && loaded[0xa0] == 0x48 && loaded[0xa1] == 0x58);
    CHECK((engine.ram == std::array<std::uint8_t, 8>{}));
    db::SnapshotWriter after_failure;
    CHECK(registry.serialize(after_failure, &error));
    CHECK(after_failure.buffer() == changed.buffer());

    // Guard and overlapping-claim failures occur before any write.
    engine.writes = 0;
    CHECK(rmod_apply(context.get(), &patch, 1, nullptr, 0, &callbacks, nullptr) == 0);
    CHECK(engine.writes == 0);
    const std::array<rmod_patch, 2> overlap{failing[0], failing[0]};
    CHECK(rmod_apply(context.get(), overlap.data(), overlap.size(), nullptr, 0, &callbacks, nullptr) == 0);
    CHECK(engine.writes == 0);

    const std::array<std::uint8_t, 1> native_before{0x31}, native_after{0x32};
    const rmod_patch invalid_code{"cartridge", 0, native_before.data(), native_after.data(), 1};
    CHECK(rmod_apply(context.get(), &invalid_code, 1, nullptr, 0, &callbacks, nullptr) == 0);
    CHECK(loaded[0] == 0x31 && !engine.committed);

    // Invalid state cannot introduce code writes or cross a target/plan boundary.
    auto provider = cart.snapshot_provider();
    db::SnapshotWriter payload;
    CHECK(provider.save(provider.user, payload, &error));
    for (const auto index : {std::size_t{0}, std::size_t{32}, std::size_t{68}}) {
        auto invalid = payload.buffer(); invalid[index] ^= 1;
        db::SnapshotReader bad(invalid.data(), invalid.size());
        CHECK(!provider.preflight(provider.user, bad, &error));
        CHECK(loaded == before_preflight);
    }
    db::SnapshotReader truncated(payload.buffer().data(), payload.size() - 1);
    CHECK(!provider.preflight(provider.user, truncated, &error));
    db::SnapshotReader reset_check(baseline.buffer().data(), baseline.size());
    CHECK(registry.preflight(reset_check, &error));
    db::SnapshotReader reset(baseline.buffer().data(), baseline.size());
    CHECK(registry.restore(reset, &error));
    CHECK(loaded == original);
    CHECK(rmod_apply(context.get(), &patch, 1, nullptr, 0, &callbacks, repeated_fingerprint.data()) == 1);
    CHECK(first_fingerprint == repeated_fingerprint);
    if (failures) return 1;
    std::puts("Cartridge writes, original preservation, native guards, cross-space rollback and registered state restore PASS");
}
