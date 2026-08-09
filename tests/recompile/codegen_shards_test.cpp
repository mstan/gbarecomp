#include "codegen_shards.h"
#include "dispatch_table_entries.h"
#include "emit_function.h"
#include "function_finder.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace {

bool expect(std::size_t functions, std::uint32_t want) {
    const auto got = gbarecomp::choose_auto_codegen_shards(functions);
    if (got == want) return true;
    std::fprintf(stderr,
                 "choose_auto_codegen_shards(%zu): got %u, want %u\n",
                 functions, got, want);
    return false;
}

}  // namespace

int main() {
    bool ok = true;
    ok &= expect(0, 2);
    ok &= expect(1, 2);
    ok &= expect(6000, 2);
    ok &= expect(6001, 4);
    ok &= expect(12000, 4);
    ok &= expect(44584, 16);  // Current Minish Cap corpus.
    ok &= expect(192001, 64);
    ok &= expect(1000000, 64);

    // Static resume coverage can make a Thumb host's aliases cross a later
    // ARM host. The flattened table must still be globally address-sorted for
    // runtime_arm's lower-bound search, with real roots ahead of same-mode
    // aliases at an identical address.
    gbarecomp::Function thumb{};
    thumb.addr = 0x000002F8u;
    thumb.mode = gbarecomp::CpuMode::Thumb;
    thumb.alias_entries = {
        0x000002FAu, 0x000002FCu, 0x000002FEu,
        0x00000300u, 0x00000302u};
    gbarecomp::Function arm{};
    arm.addr = 0x00000300u;
    arm.mode = gbarecomp::CpuMode::Arm;
    arm.alias_entries = {0x00000304u};
    const auto entries =
        gbarecomp::build_dispatch_table_entries({thumb, arm});
    for (std::size_t i = 1; i < entries.size(); ++i) {
        if (entries[i - 1].addr > entries[i].addr) {
            std::fprintf(stderr,
                         "dispatch entries are not globally sorted at %zu\n",
                         i);
            ok = false;
        }
    }
    if (entries.size() != 8 ||
        entries[4].addr != 0x00000300u ||
        entries[4].thumb ||
        entries[4].resume ||
        entries[5].addr != 0x00000300u ||
        !entries[5].thumb ||
        !entries[5].resume) {
        std::fprintf(stderr,
                     "overlapping ARM/Thumb dispatch ordering is incorrect\n");
        ok = false;
    }

    // Config-fed entry allowlists must produce a handling guard only at the
    // reviewed ARM/THUMB root; the default emitter remains hook-free.
    gbarecomp::Function hooked{};
    hooked.addr = 0x08003000u;
    hooked.end_addr = 0x08003004u;
    hooked.mode = gbarecomp::CpuMode::Arm;
    const uint8_t rom[] = {0x00u, 0x00u, 0xA0u, 0xE1u};  // MOV r0,r0
    const std::unordered_map<uint64_t, std::string> names;
    const std::unordered_set<uint64_t> hooks = {
        static_cast<uint64_t>(hooked.addr) << 1u};
    const std::string guarded = gbarecomp::emit_function_body_str(
        hooked, rom, sizeof(rom), hooked.addr, names, nullptr, &hooks);
    const std::string stock = gbarecomp::emit_function_body_str(
        hooked, rom, sizeof(rom), hooked.addr, names);
    if (guarded.find("gba_mod_function_entry(0x08003000u, 0u, &g_cpu)") ==
            std::string::npos ||
        stock.find("gba_mod_function_entry") != std::string::npos) {
        std::fprintf(stderr, "mod function-hook codegen allowlist is incorrect\n");
        ok = false;
    }
    return ok ? 0 : 1;
}
