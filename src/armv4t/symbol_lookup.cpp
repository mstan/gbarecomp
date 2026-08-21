// symbol_lookup.cpp — see symbol_lookup.h.
#include "symbol_lookup.h"

namespace {

const GbaSymbol* g_cart = nullptr;
unsigned         g_cart_n = 0u;
const GbaSymbol* g_bios = nullptr;
unsigned         g_bios_n = 0u;
const GbaDataSymbol* g_data = nullptr;
unsigned             g_data_n = 0u;

// How far past an unsized data symbol (size 0) a lookup still resolves.
// A part-disassembled decomp declares many of its RAM globals as absolute
// symbols with no size; refusing them entirely would name almost nothing,
// while treating them as unbounded would attribute every unnamed address to
// whichever global happens to precede it. One 32-byte GBA cache line's worth
// keeps struct-field accesses attributable without inventing coverage.
constexpr uint32_t kUnsizedWindow = 32u;

// Largest entry with addr <= pc (upper_bound - 1). Table is sorted ascending.
const char* search(const GbaSymbol* tab, unsigned n, uint32_t pc,
                   uint32_t* off) {
    if (!tab || n == 0u) return nullptr;
    unsigned lo = 0u, hi = n;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2u;
        if (tab[mid].addr <= pc) lo = mid + 1u;
        else hi = mid;
    }
    if (lo == 0u) return nullptr;  // pc precedes the first entry
    const GbaSymbol& e = tab[lo - 1u];
    if (off) *off = pc - e.addr;
    return e.name;
}

}  // namespace

extern "C" void gba_symbol_register_cart(const GbaSymbol* tab, unsigned count) {
    g_cart = tab;
    g_cart_n = count;
}

extern "C" void gba_symbol_register_bios(const GbaSymbol* tab, unsigned count) {
    g_bios = tab;
    g_bios_n = count;
}

extern "C" void gba_data_symbol_register_cart(const GbaDataSymbol* tab,
                                               unsigned count) {
    g_data = tab;
    g_data_n = count;
}

extern "C" const char* gba_data_symbol_lookup(uint32_t addr,
                                               uint32_t* out_offset) {
    if (out_offset) *out_offset = 0u;
    if (!g_data || g_data_n == 0u) return nullptr;
    // Largest entry with addr <= query (upper_bound - 1), then a containment
    // test. Only the candidate immediately below can contain the address:
    // the generated table is unique per address and sorted, and guest data
    // symbols do not nest.
    unsigned lo = 0u, hi = g_data_n;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2u;
        if (g_data[mid].addr <= addr) lo = mid + 1u;
        else hi = mid;
    }
    if (lo == 0u) return nullptr;
    const GbaDataSymbol& e = g_data[lo - 1u];
    const uint32_t off = addr - e.addr;
    const uint32_t extent = e.size != 0u ? e.size : kUnsizedWindow;
    if (off >= extent) return nullptr;
    if (out_offset) *out_offset = off;
    return e.name;
}

extern "C" const char* gba_symbol_lookup(uint32_t pc, uint32_t* out_offset) {
    if (out_offset) *out_offset = 0u;
    // BIOS region (PC < 0x4000) resolves against the BIOS map first.
    if (pc < 0x00004000u) {
        const char* nm = search(g_bios, g_bios_n, pc, out_offset);
        if (nm) return nm;
    }
    return search(g_cart, g_cart_n, pc, out_offset);
}
