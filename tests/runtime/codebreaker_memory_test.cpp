#include "codebreaker_memory.h"
#include <recomp_mod.h>
#include <cstdio>
#include <map>
#include <stdexcept>

namespace cb = gbarecomp::codebreaker;
namespace db = gbarecomp::debug;
using gbarecomp::CartridgeModState;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line%d: %s\n", __LINE__, #x); ++failures; } } while (0)
struct Bus {
    std::map<std::uint32_t, std::uint8_t> bytes;
    bool fail_once = false;
    std::uint16_t get(std::uint32_t a) { return bytes[a] | (std::uint16_t(bytes[a + 1]) << 8); }
    cb::Memory memory() {
        return {this,
            +[](void* p, std::uint32_t a) { return static_cast<Bus*>(p)->get(a); },
            +[](void* p, std::uint32_t a, std::uint8_t v) {
                auto& b = *static_cast<Bus*>(p); b.bytes[a] = v;
                if (b.fail_once) { b.fail_once = false; throw std::runtime_error("injected partial write"); }
            },
            +[](void* p, std::uint32_t a, std::uint16_t v) {
                auto& b = *static_cast<Bus*>(p); b.bytes[a] = v; b.bytes[a + 1] = v >> 8;
            }};
    }
};
cb::NamedProgram program(const char* id, std::vector<std::string> lines) {
    cb::NamedProgram p{id, id, {}};
    std::string error;
    if (!cb::parse(lines, &p.program, &error)) { std::printf("Parse failed: %s\n", error.c_str()); ++failures; }
    return p;
}
struct Fixture {
    const std::vector<std::uint8_t> original = std::vector<std::uint8_t>(65536, 0x31);
    std::vector<std::uint8_t> loaded = original;
    CartridgeModState::Identity identity{};
    RuntimeNativeImageRange range{0, 4};
    RuntimeNativeImageDependencies native{&range, 1, true};
    Bus bus;
    Fixture() { CHECK(rmod_sha256_bytes(original.data(), original.size(), identity.data())); }
    bool bind(cb::FrameMemory& m, std::vector<cb::NamedProgram> p, std::string& error) {
        return m.initialize(std::move(p), bus.memory(), loaded, &native, "test.cartridge", identity, &error);
    }
    std::uint16_t cart(std::size_t a) const { return loaded[a] | (std::uint16_t(loaded[a + 1]) << 8); }
};
void counter_and_state() {
    Fixture f; cb::FrameMemory m; std::string error;
    auto counter = program("counter", {"E80000A0 0001", "7A0000A0 3132", "82000000 BEEF"});
    CHECK(cb::FrameMemory::uses_cartridge(counter.program));
    CHECK(f.bind(m, {counter}, error));
    CHECK(!f.bind(m, {counter}, error)); // Cannot replace a plan after snapshot binding.
    CHECK(m.plan_fingerprint().size() == 64 && m.cartridge_state());
    CHECK(m.run(&error)); CHECK(f.cart(0xa0) == 0x3132 && f.bus.get(0x02000000) == 0xbeef);
    db::ModStateRegistry registry;
    CHECK(registry.register_provider(m.cartridge_state()->snapshot_provider(), &error));
    db::SnapshotWriter saved;
    CHECK(registry.serialize(saved, &error));
    CHECK(m.run(&error)); CHECK(f.cart(0xa0) == 0x3133);
    const auto before = f.loaded;
    db::SnapshotReader preflight(saved.buffer().data(), saved.size());
    CHECK(registry.preflight(preflight, &error)); CHECK(f.loaded == before);
    db::SnapshotReader restore(saved.buffer().data(), saved.size());
    CHECK(registry.restore(restore, &error)); CHECK(f.cart(0xa0) == 0x3132);
    CHECK(m.run(&error)); CHECK(f.cart(0xa0) == 0x3133);
    CHECK(f.original[0xa0] == 0x31 && f.original[0xa1] == 0x31);
}
void overlaps_and_guards() {
    std::string error;
    { Fixture f; cb::FrameMemory m;
      CHECK(f.bind(m, {program("a", {"880000A0 1234"}), program("b", {"8A0000A0 1234"})}, error));
      CHECK(m.run(&error)); CHECK(f.cart(0xa0) == 0x1234); }
    { Fixture f; cb::FrameMemory m;
      CHECK(!f.bind(m, {program("a", {"880000A0 1234"}), program("b", {"8C0000A0 5678"})}, error));
      CHECK(error.find("conflicts") != std::string::npos && f.loaded == f.original); }
    { Fixture f; cb::FrameMemory m;
      CHECK(!f.bind(m, {program("code", {"88000000 1234"})}, error));
      CHECK(error.find("compiled code") != std::string::npos); }
    { Fixture f; cb::FrameMemory m;
      CHECK(f.bind(m, {program("code-noop", {"88000000 3131", "82000000 2345"})}, error));
      CHECK(m.run(&error)); CHECK(f.bus.get(0x02000000) == 0x2345 && f.loaded == f.original); }
    { Fixture f; cb::FrameMemory m;
      CHECK(f.bind(m, {program("or-noop", {"28000000 0001"})}, error));
      CHECK(m.run(&error)); CHECK(!m.changed() && f.loaded == f.original); }
    { Fixture f; cb::FrameMemory m;
      CHECK(f.bind(m, {program("code-rmw", {"82000000 2345", "E8000000 0001"})}, error));
      CHECK(!m.run(&error)); CHECK(f.bus.get(0x02000000) == 0 && f.loaded == f.original); }
    { Fixture f; cb::FrameMemory m;
      CHECK(!f.bind(m, {program("past-image", {"88010000 0001"})}, error)); }
}
void aliases_and_rollback() {
    Fixture f; cb::FrameMemory m; std::string error;
    CHECK(f.bind(m, {program("all-spaces", {"82F00000 0123", "83FF8000 2345", "85FFF800 3456",
        "86FF8000 4567", "87FFF800 5678", "8C0000A0 6789"})}, error));
    CHECK(m.run(&error));
    CHECK(f.bus.get(0x02000000) == 0x0123 && f.bus.get(0x03000000) == 0x2345);
    CHECK(f.bus.get(0x05000000) == 0x3456 && f.bus.get(0x06010000) == 0x4567);
    CHECK(f.bus.get(0x07000000) == 0x5678 && f.cart(0xa0) == 0x6789);
    Fixture bad; cb::FrameMemory transaction;
    CHECK(bad.bind(transaction, {program("atomic", {"82000000 1234", "880000A0 2345"})}, error));
    bad.bus.fail_once = true;
    CHECK(!transaction.run(&error));
    CHECK(bad.bus.get(0x02000000) == 0 && bad.loaded == bad.original);
    CHECK(transaction.run(&error));
    CHECK(bad.bus.get(0x02000000) == 0x1234 && bad.cart(0xa0) == 0x2345);
}
void strided_plan() {
    Fixture f; cb::FrameMemory m; std::string error;
    CHECK(f.bind(m, {program("strided", {"48000100 1234", "00002000 0004"})}, error));
    CHECK(m.run(&error));
    for (std::size_t i = 0; i < 8192; ++i) {
        CHECK(f.cart(0x100 + i * 4) == 0x1234);
        CHECK(f.cart(0x102 + i * 4) == 0x3131);
    }
    CHECK(m.run(&error) && !m.changed());
}
void session_lifetime() {
    auto& registry = db::global_mod_state_registry();
    std::string error;
    db::SnapshotWriter saved;
    {
        Fixture f; cb::FrameMemory m;
        CHECK(f.bind(m, {program("session", {"E80000A0 0001"})}, error));
        CHECK(m.bind_snapshots(&error)); CHECK(m.run(&error));
        CHECK(registry.serialize(saved, &error));
        CHECK(m.run(&error));
        db::SnapshotReader check(saved.buffer().data(), saved.size());
        CHECK(registry.preflight(check, &error));
        db::SnapshotReader restore(saved.buffer().data(), saved.size());
        CHECK(registry.restore(restore, &error)); CHECK(f.cart(0xa0) == 0x3132);
    }
    db::SnapshotWriter absent;
    CHECK(!registry.serialize(absent, &error)); // No dangling image/provider pointer.
    {
        cb::FrameMemory no_cart; CHECK(no_cart.bind_snapshots(&error));
        CHECK(registry.size() == 1);
        db::SnapshotReader wrong(saved.buffer().data(), saved.size());
        CHECK(!registry.preflight(wrong, &error));
        db::SnapshotWriter empty; CHECK(registry.serialize(empty, &error));
        db::SnapshotReader check(empty.buffer().data(), empty.size());
        CHECK(registry.preflight(check, &error));
    }
}
}
int main() {
    counter_and_state(); overlaps_and_guards(); aliases_and_rollback(); strided_plan(); session_lifetime();
    if (failures) return 1;
    std::puts("CodeBreaker cartridge mirrors, native guards, atomic RAM/image writes, state restore and strided plans PASS");
    return 0;
}
