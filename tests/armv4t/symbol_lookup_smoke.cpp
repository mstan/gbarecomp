// symbol_lookup_smoke — exercise the runtime address→name resolvers in
// src/armv4t/symbol_lookup.cpp.
//
// Two resolvers with deliberately different semantics:
//
//   gba_symbol_lookup      nearest FUNCTION entry at or below the PC. A PC is
//                          always inside some function, so "nearest below" is
//                          the right answer and an unbounded offset is fine.
//
//   gba_data_symbol_lookup only resolves INSIDE a data symbol's extent. Guest
//                          memory is sparse: the global 700 KB below an
//                          address says nothing about it, so a nearest-below
//                          answer would be actively misleading. Symbols with
//                          unknown size (size 0 — what a part-disassembled
//                          decomp declares for its absolute symbols) resolve
//                          only within a small window.
//
// Both must degrade to nullptr when no map is registered, because a binary
// built without an imported symbol set links no generated map at all.

#include <cstdio>
#include <cstring>

#include "symbol_lookup.h"

namespace {

int g_failures = 0;

void expect_name(const char* what, const char* got, const char* want) {
    const bool ok = (want == nullptr)
        ? (got == nullptr)
        : (got != nullptr && std::strcmp(got, want) == 0);
    if (!ok) {
        std::printf("FAIL %s: got %s, want %s\n", what,
                    got ? got : "(null)", want ? want : "(null)");
        ++g_failures;
    }
}

void expect_off(const char* what, uint32_t got, uint32_t want) {
    if (got != want) {
        std::printf("FAIL %s: offset 0x%X, want 0x%X\n", what, got, want);
        ++g_failures;
    }
}

// Sorted ascending, as the generated maps are.
const GbaSymbol kFuncs[] = {
    {0x08000100u, "AgbMain"},
    {0x08000200u, "VBlankHandler"},
    {0x08001000u, "SoundMain"},
};

const GbaDataSymbol kData[] = {
    {0x02000000u, 0x400u, "gEwramBuffer"},   // sized, 1 KiB
    {0x03004F30u, 0x18u,  "gKeyBuf"},        // sized, 24 bytes
    {0x03005CA0u, 0x0u,   "gSoundInfo"},     // size unknown
};

}  // namespace

int main() {
    // ── Nothing registered: both resolvers must stay silent. ─────────
    uint32_t off = 0xDEADu;
    expect_name("unregistered func", gba_symbol_lookup(0x08000100u, &off),
                nullptr);
    expect_off("unregistered func offset", off, 0u);
    off = 0xDEADu;
    expect_name("unregistered data",
                gba_data_symbol_lookup(0x03004F30u, &off), nullptr);
    expect_off("unregistered data offset", off, 0u);

    // ── Function map: nearest entry at or below. ──────────────────────
    gba_symbol_register_cart(kFuncs, 3u);

    expect_name("exact entry", gba_symbol_lookup(0x08000100u, &off), "AgbMain");
    expect_off("exact entry offset", off, 0u);

    expect_name("interior", gba_symbol_lookup(0x08000110u, &off), "AgbMain");
    expect_off("interior offset", off, 0x10u);

    // A PC in the gap between two entries belongs to the earlier one: the
    // finder's function extents are contiguous by construction.
    expect_name("gap", gba_symbol_lookup(0x08000900u, &off), "VBlankHandler");
    expect_off("gap offset", off, 0x700u);

    expect_name("last entry", gba_symbol_lookup(0x08009999u, &off),
                "SoundMain");

    // Below the first entry there is nothing to attribute it to.
    expect_name("before first", gba_symbol_lookup(0x08000000u, &off), nullptr);

    // ── Data map: containment, not proximity. ────────────────────────
    gba_data_symbol_register_cart(kData, 3u);

    expect_name("data exact", gba_data_symbol_lookup(0x03004F30u, &off),
                "gKeyBuf");
    expect_off("data exact offset", off, 0u);

    expect_name("data interior", gba_data_symbol_lookup(0x03004F42u, &off),
                "gKeyBuf");
    expect_off("data interior offset", off, 0x12u);

    // One past the end is NOT gKeyBuf, even though gKeyBuf is the nearest
    // symbol below it. This is the whole point of storing extents.
    expect_name("data one past end",
                gba_data_symbol_lookup(0x03004F48u, &off), nullptr);
    expect_off("data one past end offset", off, 0u);

    expect_name("data last byte", gba_data_symbol_lookup(0x03004F47u, &off),
                "gKeyBuf");
    expect_off("data last byte offset", off, 0x17u);

    // A big sized symbol resolves across its whole extent.
    expect_name("data big interior",
                gba_data_symbol_lookup(0x020003FFu, &off), "gEwramBuffer");
    expect_off("data big interior offset", off, 0x3FFu);
    expect_name("data big one past end",
                gba_data_symbol_lookup(0x02000400u, &off), nullptr);

    // Unknown size: resolves for a bounded window only (32 bytes), so an
    // unsized symbol can neither be useless nor swallow all of memory.
    expect_name("unsized start", gba_data_symbol_lookup(0x03005CA0u, &off),
                "gSoundInfo");
    expect_name("unsized in window",
                gba_data_symbol_lookup(0x03005CBFu, &off), "gSoundInfo");
    expect_off("unsized in window offset", off, 0x1Fu);
    expect_name("unsized past window",
                gba_data_symbol_lookup(0x03005CC0u, &off), nullptr);

    // Below the first data symbol, and in a region with no symbols at all.
    expect_name("data before first",
                gba_data_symbol_lookup(0x01FFFFFFu, &off), nullptr);
    expect_name("data unmapped region",
                gba_data_symbol_lookup(0x08000100u, &off), nullptr);

    // A null out_offset must be accepted (callers that only want the name).
    expect_name("null offset arg",
                gba_data_symbol_lookup(0x03004F30u, nullptr), "gKeyBuf");

    if (g_failures != 0) {
        std::printf("symbol_lookup_smoke: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("symbol_lookup_smoke: OK\n");
    return 0;
}
