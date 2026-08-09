#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "mod_state.h"

namespace {
int failures = 0;
void check(bool value, const char* what) {
    if (!value) { std::printf("FAIL %s\n", what); ++failures; }
}

struct ProviderValue { uint32_t value = 0; int restores = 0; };
bool save(void* user, gbarecomp::debug::SnapshotWriter& out, std::string*) {
    out.u32(static_cast<ProviderValue*>(user)->value); return true;
}
bool preflight(void*, gbarecomp::debug::SnapshotReader& in, std::string*) {
    (void)in.u32(); return in.ok() && in.remaining() == 0;
}
void restore(void* user, gbarecomp::debug::SnapshotReader& in) {
    auto* value = static_cast<ProviderValue*>(user);
    value->value = in.u32(); ++value->restores;
}

gbarecomp::debug::ModStateProvider provider(ProviderValue* value, uint32_t schema = 1) {
    return {"test.provider", schema, value, save, preflight, restore};
}

void test_registry_mismatch_preflights_without_restore() {
    ProviderValue writer_value{0x12345678u};
    gbarecomp::debug::ModStateRegistry writer;
    std::string error;
    std::string dynamic_id = "test.provider";
    auto dynamic_provider = provider(&writer_value);
    dynamic_provider.id = dynamic_id.c_str();
    check(writer.register_provider(dynamic_provider, &error), "register writer provider");
    dynamic_id.assign("temporary-id-was-not-retained");
    gbarecomp::debug::SnapshotWriter saved;
    check(writer.serialize(saved, &error), "serialize provider catalog");
    check(!writer.register_provider(provider(&writer_value), &error),
          "registration closes after first snapshot");

    ProviderValue reader_value{0xDEADBEEFu};
    gbarecomp::debug::ModStateRegistry mismatch;
    check(mismatch.register_provider(provider(&reader_value, 2), &error), "register mismatch provider");
    gbarecomp::debug::SnapshotReader mismatch_reader(saved.buffer().data(), saved.size());
    check(!mismatch.preflight(mismatch_reader, &error), "schema mismatch rejects preflight");
    check(reader_value.value == 0xDEADBEEFu && reader_value.restores == 0,
          "schema mismatch did not restore provider");

    gbarecomp::debug::SnapshotReader restore_reader(saved.buffer().data(), saved.size());
    check(writer.preflight(restore_reader, &error), "matching catalog preflight");
    gbarecomp::debug::SnapshotReader apply_reader(saved.buffer().data(), saved.size());
    check(writer.restore(apply_reader, &error), "matching catalog restore");
    check(writer_value.restores == 1, "matching catalog restored exactly once");
}

void test_slot_canonicalization_and_recovery() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
        ("gbarecomp-mod-state-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::string key, error;
    check(gbarecomp::debug::canonical_mod_slot_key("Slot_01", &key, &error) && key == "slot_01",
          "slot key canonicalizes case");
    check(!gbarecomp::debug::canonical_mod_slot_key("../escape", &key, &error),
          "path-like slot key rejected");

    const std::vector<uint8_t> drive_payload = {0};
    check(!gbarecomp::debug::write_mod_state_slot("F:relative-path", "slot_01",
                                                   "rom+catalog-a", drive_payload, &error),
          "drive-relative directory rejected before creation");

    const std::vector<uint8_t> first = {1, 2, 3};
    const std::vector<uint8_t> second = {9, 8, 7, 6};
    check(gbarecomp::debug::write_mod_state_slot(root, "Slot_01", "rom+catalog-a", first, &error),
          "write first sidecar");
    std::vector<uint8_t> loaded;
    check(gbarecomp::debug::read_mod_state_slot(root, "slot_01", "rom+catalog-a", &loaded, &error) && loaded == first,
          "read primary sidecar");
    check(gbarecomp::debug::write_mod_state_slot(root, "slot_01", "rom+catalog-a", second, &error),
          "atomic replacement writes second sidecar");
    check(fs::exists(root / "slot_01.bak"), "replacement retains backup");
    { std::ofstream corrupt(root / "slot_01.mods", std::ios::binary | std::ios::trunc); corrupt << "bad"; }
    loaded.clear();
    check(gbarecomp::debug::read_mod_state_slot(root, "slot_01", "rom+catalog-a", &loaded, &error) && loaded == first,
          "corrupt primary recovers backup");
    check(!gbarecomp::debug::read_mod_state_slot(root, "slot_01", "wrong-identity", &loaded, &error),
          "identity mismatch rejects recovered backup");
    std::error_code ec;
    fs::remove_all(root, ec);
}
}  // namespace

int main() {
    test_registry_mismatch_preflights_without_restore();
    test_slot_canonicalization_and_recovery();
    if (failures) { std::printf("mod_state_tests: %d failure(s)\n", failures); return 1; }
    std::printf("mod_state_tests: OK\n");
    return 0;
}
