// ws_sidecar.cpp — see ws_sidecar.h.

#include "ws_sidecar.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "runtime_arm.h"            // g_cpu, g_runtime_fn_entry_hook
#include "runtime_bus_bridge.h"     // gbarecomp::active_bus()
#include "gba_bus.h"                // GbaBus raw region pointers, io()
#include "gba_io.h"                 // GbaIo::raw()

namespace gbarecomp {

namespace {

// ── extended cache geometry ──────────────────────────────────────────
// 64 tiles wide (512 px) is plenty for the central 256 px + widescreen margins
// without self-collision (a 288 px span is 36 tiles « 64). 32 tiles tall keeps
// vertical aligned with the guest ring (vertical expansion is deferred).
constexpr int kCacheW = 64;
constexpr int kCacheH = 32;
constexpr int kRing = 32;                 // guest ring is 32x32 tiles
constexpr uint32_t kRingMask = 0x3FFu;    // 1024 slots - 1

struct TileXY { int16_t wx; int16_t wy; uint8_t valid; };

TileXY   g_owner[kRing * kRing];          // ring slot -> exact world tile
uint16_t g_cache[3][kCacheW * kCacheH];   // per field BG (1..3) tilemap entry
uint8_t  g_cache_valid[kCacheW * kCacheH];

bool     g_enabled = false;
uint32_t g_dm_pc = 0;                      // DrawMetatileAt guest PC
uint32_t g_tilemap_ptrs = 0;              // IWRAM addr of gBGTilemapBuffers1
unsigned long long g_draws = 0;
unsigned long long g_syncs = 0;
unsigned long long g_cache_fills = 0;

uint32_t parse_hex_env(const char* name) {
    const char* v = std::getenv(name);
    if (!v || !v[0]) return 0;
    return static_cast<uint32_t>(std::strtoul(v, nullptr, 0));
}

// Side-effect-free guest reads via raw region pointers (NOT bus.read*, which
// model open-bus / prefetch). Covers the regions the field tilemaps live in.
uint16_t rd16(gba::GbaBus* bus, uint32_t addr) {
    const uint8_t* p = nullptr;
    uint32_t off = 0, size = 0;
    if (addr >= 0x02000000u && addr < 0x02040000u) {
        p = bus->ewram_ptr(); off = addr - 0x02000000u; size = 0x40000u;
    } else if (addr >= 0x03000000u && addr < 0x03008000u) {
        p = bus->iwram_ptr(); off = addr - 0x03000000u; size = 0x8000u;
    } else if (addr >= 0x06000000u && addr < 0x06018000u) {
        p = bus->vram_ptr(); off = addr - 0x06000000u; size = 0x18000u;
    }
    if (!p || off + 1u >= size) return 0;
    return static_cast<uint16_t>(p[off] | (p[off + 1] << 8));
}
uint32_t rd32(gba::GbaBus* bus, uint32_t addr) {
    return static_cast<uint32_t>(rd16(bus, addr)) |
           (static_cast<uint32_t>(rd16(bus, addr + 2u)) << 16);
}

// floor(v / 8) for signed v (margins make HOFS+x go negative).
inline int fd8(int v) { return (v >= 0) ? (v >> 3) : -(((-v) + 7) >> 3); }

// BG scroll from the IO shadow (side-effect free).
struct Scroll { int hofs; int vofs; };
Scroll bg_scroll(gba::GbaBus* bus, int bg) {
    const uint8_t* io = bus->io().raw();
    auto r16 = [&](uint32_t o) {
        return static_cast<int>(io[o] | (io[o + 1] << 8));
    };
    return { r16(0x10u + bg * 4u), r16(0x12u + bg * 4u) };
}

// World tile at screen (0,0) for BG `bg` from the live central ring owner.
// Returns false if the central slot isn't owned yet.
bool world_origin(gba::GbaBus* bus, int bg, int* w0x, int* w0y, Scroll* s) {
    *s = bg_scroll(bus, bg);
    const uint32_t cslot = ((static_cast<uint32_t>(s->vofs) >> 3) & 31u) * 32u +
                           ((static_cast<uint32_t>(s->hofs) >> 3) & 31u);
    const TileXY& o = g_owner[cslot];
    if (!o.valid) return false;
    *w0x = o.wx;
    *w0y = o.wy;
    return true;
}

}  // namespace

// Function-entry hook: maintain the per-tile owner ring from DrawMetatileAt.
// Hot path — single compare for non-target PCs.
extern "C" void ws_sidecar_fn_entry_hook(uint32_t pc) {
    if (pc != g_dm_pc) return;
    const uint32_t off = g_cpu.R[1] & kRingMask;     // ring tile offset (TL)
    const int mwx = static_cast<int>(g_cpu.R[2]);    // world METATILE x
    const int mwy = static_cast<int>(g_cpu.R[3]);    // world METATILE y
    // The 2x2 tile block at off,+1,+0x20,+0x21 maps to world tiles
    // (2mwx,2mwy)=TL, (2mwx+1,2mwy)=TR, (2mwx,2mwy+1)=BL, (2mwx+1,2mwy+1)=BR.
    struct { uint32_t d; int dx; int dy; } e[4] = {
        {0u, 0, 0}, {1u, 1, 0}, {0x20u, 0, 1}, {0x21u, 1, 1}};
    for (auto& q : e) {
        TileXY& o = g_owner[(off + q.d) & kRingMask];
        o.wx = static_cast<int16_t>(2 * mwx + q.dx);
        o.wy = static_cast<int16_t>(2 * mwy + q.dy);
        o.valid = 1u;
    }
    ++g_draws;
}

void ws_sidecar_init_from_env() {
    if (g_enabled) return;
    if (parse_hex_env("GBARECOMP_WS_SIDECAR") == 0) return;
    g_dm_pc = parse_hex_env("GBARECOMP_WS_SC_DRAWMETATILE") & ~1u;
    g_tilemap_ptrs = parse_hex_env("GBARECOMP_WS_SC_TILEMAP_PTRS");
    if (!g_dm_pc || !g_tilemap_ptrs) {
        std::fprintf(stderr, "[ws-sidecar] NOT armed: need "
            "GBARECOMP_WS_SC_DRAWMETATILE and _TILEMAP_PTRS\n");
        return;
    }
    std::memset(g_owner, 0, sizeof(g_owner));
    std::memset(g_cache, 0, sizeof(g_cache));
    std::memset(g_cache_valid, 0, sizeof(g_cache_valid));
    if (g_runtime_fn_entry_hook && g_runtime_fn_entry_hook !=
            &ws_sidecar_fn_entry_hook) {
        std::fprintf(stderr, "[ws-sidecar] WARNING: fn-entry hook already "
            "installed by another probe; sidecar overriding\n");
    }
    g_runtime_fn_entry_hook = &ws_sidecar_fn_entry_hook;
    g_enabled = true;
    std::fprintf(stderr, "[ws-sidecar] armed: DrawMetatileAt=0x%08X "
        "tilemap_ptrs=0x%08X cache=%dx%d\n", g_dm_pc, g_tilemap_ptrs,
        kCacheW, kCacheH);
}

bool ws_sidecar_enabled() { return g_enabled; }

void ws_sidecar_sync_frame() {
    if (!g_enabled) return;
    gba::GbaBus* bus = active_bus();
    if (!bus) return;
    uint32_t buf[3];
    for (int k = 0; k < 3; ++k)
        buf[k] = rd32(bus, g_tilemap_ptrs + static_cast<uint32_t>(k) * 4u);
    if (!buf[0]) return;  // buffers not allocated yet (pre-field)
    for (uint32_t slot = 0; slot < (kRing * kRing); ++slot) {
        const TileXY& o = g_owner[slot];
        if (!o.valid) continue;
        const int cidx = (o.wy & (kCacheH - 1)) * kCacheW + (o.wx & (kCacheW - 1));
        for (int k = 0; k < 3; ++k)
            g_cache[k][cidx] = rd16(bus, buf[k] + slot * 2u);
        if (!g_cache_valid[cidx]) ++g_cache_fills;
        g_cache_valid[cidx] = 1u;
    }
    ++g_syncs;
}

bool ws_sidecar_tilemap_entry(int bg, int hw_x, int screen_y,
                              uint16_t* out_entry) {
    if (!g_enabled || bg < 1 || bg > 3 || !out_entry) return false;
    gba::GbaBus* bus = active_bus();
    if (!bus) return false;
    int w0x, w0y; Scroll s;
    if (!world_origin(bus, bg, &w0x, &w0y, &s)) return false;
    const int wtx = w0x + fd8(s.hofs + hw_x) - fd8(s.hofs);
    const int wty = w0y + fd8(s.vofs + screen_y) - fd8(s.vofs);
    const int cidx = (wty & (kCacheH - 1)) * kCacheW + (wtx & (kCacheW - 1));
    if (!g_cache_valid[cidx]) return false;
    *out_entry = g_cache[bg - 1][cidx];
    return true;
}

bool ws_sidecar_dump(const char* path, int extra_cols_per_side) {
    if (!g_enabled || !path || !path[0]) return false;
    gba::GbaBus* bus = active_bus();
    if (!bus) return false;
    std::FILE* f = std::fopen(path, "w");
    if (!f) return false;

    int cached = 0;
    for (int i = 0; i < kCacheW * kCacheH; ++i) cached += g_cache_valid[i];
    std::fprintf(f, "# widescreen sidecar verify report\n");
    std::fprintf(f, "draws=%llu syncs=%llu cache_filled=%d/%d extra_px=%d\n",
                 g_draws, g_syncs, cached, kCacheW * kCacheH, extra_cols_per_side);

    int w0x, w0y; Scroll s;
    if (!world_origin(bus, 1, &w0x, &w0y, &s)) {
        std::fprintf(f, "central slot not owned yet (no field draw seen)\n");
        std::fclose(f);
        return true;
    }
    const int extra_tiles = (extra_cols_per_side + 7) / 8;
    const int row_y = 80;  // mid-screen scanline
    std::fprintf(f, "BG1 hofs=%d vofs=%d  world_origin=(%d,%d)\n",
                 s.hofs, s.vofs, w0x, w0y);
    std::fprintf(f, "# expanded-span resolution at screen_y=%d "
        "(o = output tile col; central 0..29, margins outside)\n", row_y);
    std::fprintf(f, "# o  world_tile(x,y)  cached  entry\n");
    int resolved = 0, total = 0, distinct_ok = 1, prev_wtx = -99999;
    for (int o = -extra_tiles; o <= 29 + extra_tiles; ++o) {
        const int hw_x = o * 8;
        const int wtx = w0x + fd8(s.hofs + hw_x) - fd8(s.hofs);
        const int wty = w0y + fd8(s.vofs + row_y) - fd8(s.vofs);
        const int cidx = (wty & (kCacheH - 1)) * kCacheW + (wtx & (kCacheW - 1));
        const bool ok = g_cache_valid[cidx] != 0;
        const bool margin = (o < 0) || (o > 29);
        ++total; if (ok) ++resolved;
        if (wtx != prev_wtx + 1 && o != -extra_tiles) {
            // world tiles must be strictly consecutive across the span; a
            // repeat/jump would mean the resolution aliases (the Step B seam).
            distinct_ok = 0;
        }
        prev_wtx = wtx;
        std::fprintf(f, "%4d  (%d,%d)  %s  0x%04X%s\n", o, wtx, wty,
                     ok ? "YES" : "no", ok ? g_cache[0][cidx] : 0,
                     margin ? "  <margin>" : "");
    }
    std::fprintf(f, "\nSUMMARY: span=%d  resolved_from_cache=%d  "
        "world_tiles_consecutive(no-alias)=%s\n", total, resolved,
        distinct_ok ? "YES" : "NO");
    std::fprintf(f, "  resolved==span AND consecutive == sidecar supplies the "
        "TRUE extended world (Step B seam eliminated).\n");
    std::fclose(f);
    std::fprintf(stderr, "[ws-sidecar] wrote %s (cache_filled=%d resolved=%d/%d)\n",
                 path, cached, resolved, total);
    return true;
}

}  // namespace gbarecomp
