// ws_sidecar.cpp — see ws_sidecar.h.

#include "ws_sidecar.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "runtime_arm.h"            // g_cpu, g_runtime_fn_entry_hook
#include "runtime_bus_bridge.h"     // gbarecomp::active_bus()
#include "gba_bus.h"                // GbaBus raw region pointers, io()
#include "gba_io.h"                 // GbaIo::raw()
#include "gba_ppu.h"                // gba::g_ws_tilemap_provider

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
uint32_t g_mapheader = 0;                 // EWRAM addr of gMapHeader (.mapLayout @+0)
uint32_t g_gmain = 0;                      // IWRAM addr of gMain (callback2 @ +4)
uint32_t g_cb2_overworld = 0;             // guest PC of CB2_Overworld (thumb, no |1)
bool     g_active_mode = false;           // GBARECOMP_WS_SC_ACTIVE: per-frame fill
bool     g_in_active_fill = false;        // re-entrancy guard for synthetic draws
unsigned long long g_draws = 0;
unsigned long long g_syncs = 0;
unsigned long long g_cache_fills = 0;
unsigned long long g_active_calls = 0;
unsigned long long g_active_fail = 0;

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

// Side-effect-free IWRAM u32 write (for transient gBGTilemapBuffers redirect).
void wr32_iwram(gba::GbaBus* bus, uint32_t addr, uint32_t v) {
    if (addr < 0x03000000u || addr + 3u >= 0x03008000u) return;
    uint8_t* p = bus->iwram_ptr() + (addr - 0x03000000u);
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

// VRAM byte save/restore for the transient scratch the synthetic draws write,
// so the guest's VRAM is left byte-identical (count_nonzero would otherwise
// count the scratch, and the guest could read the region).
void vram_save(gba::GbaBus* bus, uint32_t addr, uint8_t* buf, uint32_t n) {
    if (addr < 0x06000000u || addr - 0x06000000u + n > 0x18000u) return;
    std::memcpy(buf, bus->vram_ptr() + (addr - 0x06000000u), n);
}
void vram_restore(gba::GbaBus* bus, uint32_t addr, const uint8_t* buf, uint32_t n) {
    if (addr < 0x06000000u || addr - 0x06000000u + n > 0x18000u) return;
    std::memcpy(bus->vram_ptr() + (addr - 0x06000000u), buf, n);
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

// World tile at screen (0,0) for BG `bg`. The ring slot for screen (0,0) is
// (base_row, base_col); ideally that slot is owned. If not (e.g. a savestate
// left the owner ring partial and the central slot hasn't been redrawn yet),
// extrapolate from the NEAREST owned slot using the ring's contiguous-window
// structure: the slots hold 32 consecutive world tiles, so the world tile at a
// slot differs from a neighbor by the signed ring delta. Returns false only if
// the entire owner ring is empty.
bool world_origin(gba::GbaBus* bus, int bg, int* w0x, int* w0y, Scroll* s) {
    *s = bg_scroll(bus, bg);
    const int base_col = static_cast<int>((static_cast<uint32_t>(s->hofs) >> 3) & 31u);
    const int base_row = static_cast<int>((static_cast<uint32_t>(s->vofs) >> 3) & 31u);
    auto sd = [](int d) { return ((d + 16) & 31) - 16; };  // signed ring delta
    int best = 0x7fffffff, bx = 0, by = 0; bool found = false;
    for (int r = 0; r < kRing; ++r) {
        for (int c = 0; c < kRing; ++c) {
            const TileXY& o = g_owner[r * kRing + c];
            if (!o.valid) continue;
            const int dc = sd(base_col - c), dr = sd(base_row - r);
            const int dist = (dc < 0 ? -dc : dc) + (dr < 0 ? -dr : dr);
            if (dist < best) {
                best = dist;
                bx = o.wx + dc;  // world tile at base_col
                by = o.wy + dr;  // world tile at base_row
                found = true;
                if (dist == 0) { *w0x = bx; *w0y = by; return true; }
            }
        }
    }
    if (!found) return false;
    *w0x = bx;
    *w0y = by;
    return true;
}

}  // namespace

// Function-entry hook: maintain the per-tile owner ring from DrawMetatileAt.
// Hot path — single compare for non-target PCs.
extern "C" void ws_sidecar_fn_entry_hook(uint32_t pc) {
    if (pc != g_dm_pc) return;
    if (g_in_active_fill) return;  // skip our own synthetic margin draws
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

extern "C" int ws_sidecar_provider_adapter(int bg, int hw_x, int screen_y,
                                           uint16_t* out_entry);  // fwd

void ws_sidecar_init_from_env() {
    if (g_enabled) return;
    if (parse_hex_env("GBARECOMP_WS_SIDECAR") == 0) return;
    g_dm_pc = parse_hex_env("GBARECOMP_WS_SC_DRAWMETATILE") & ~1u;
    g_tilemap_ptrs = parse_hex_env("GBARECOMP_WS_SC_TILEMAP_PTRS");
    g_mapheader = parse_hex_env("GBARECOMP_WS_SC_MAPHEADER");  // for active fill
    g_gmain = parse_hex_env("GBARECOMP_WS_SC_GMAIN");          // overworld policy
    g_cb2_overworld = parse_hex_env("GBARECOMP_WS_SC_CB2_OVERWORLD") & ~1u;
    if (const char* a = std::getenv("GBARECOMP_WS_SC_ACTIVE"))
        g_active_mode = (a[0] && a[0] != '0');
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
    gba::g_ws_tilemap_provider = &ws_sidecar_provider_adapter;  // PPU margin source
    g_enabled = true;
    std::fprintf(stderr, "[ws-sidecar] armed: DrawMetatileAt=0x%08X "
        "tilemap_ptrs=0x%08X cache=%dx%d\n", g_dm_pc, g_tilemap_ptrs,
        kCacheW, kCacheH);
}

bool ws_sidecar_enabled() { return g_enabled; }
bool ws_sidecar_active_mode() { return g_active_mode; }

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

// Render one world metatile via the guest's OWN DrawMetatileAt into free VRAM
// scratch, returning its 4 tile entries per BG. Uses the runtime's call-return
// contract exactly as a guest BL would (runtime_call_push_return + dispatch).
// The guest's gBGTilemapBuffers ptrs are transiently redirected to VRAM scratch
// so the live 32-ring is untouched. Reads the extended virtual map, so it works
// for NEVER-SEEN metatiles. Caller must have saved g_cpu + the 3 buffer ptrs.
namespace {
constexpr uint32_t kScratch[3] = {0x0600C000u, 0x0600C800u, 0x0600D000u};
const uint32_t kRetSentinel = 0xF0000001u;  // thumb-flagged fake return PC

bool draw_one_metatile(gba::GbaBus* bus, uint32_t map_layout, int mx, int my,
                       uint16_t tiles[3][4]) {
    // Args: R0=mapLayout, R1=offset(0 -> scratch base), R2=x, R3=y.
    g_cpu.R[0] = map_layout;
    g_cpu.R[1] = 0u;
    g_cpu.R[2] = static_cast<uint32_t>(mx);
    g_cpu.R[3] = static_cast<uint32_t>(my);
    g_cpu.R[14] = kRetSentinel;          // LR -> our sentinel
    g_cpu.cpsr |= CPSR_T_BIT;            // DrawMetatileAt is THUMB
    runtime_call_push_return(kRetSentinel);
    runtime_dispatch(g_dm_pc | 1u);
    runtime_call_cancel_return(kRetSentinel);  // no-op if already consumed
    // The 2x2 metatile occupies scratch offsets 0,1,0x20,0x21.
    static const uint32_t blk[4] = {0u, 1u, 0x20u, 0x21u};
    for (int k = 0; k < 3; ++k)
        for (int t = 0; t < 4; ++t)
            tiles[k][t] = rd16(bus, kScratch[k] + blk[t] * 2u);
    return true;
}
}  // namespace

namespace {
// Overworld discriminator: gMain.callback2 == CB2_Overworld (and field-return
// variants the caller may OR into the env). If unconfigured, assume overworld
// (no pillarbox) so the probe still works without policy addrs.
bool is_overworld(gba::GbaBus* bus) {
    if (!g_gmain || !g_cb2_overworld) return true;
    const uint32_t cb2 = rd32(bus, g_gmain + 4u) & ~1u;
    return cb2 == g_cb2_overworld;
}
}  // namespace

void ws_sidecar_active_fill() {
    if (!g_enabled || !g_mapheader) return;
    gba::GbaBus* bus = active_bus();
    if (!bus) return;
    // Policy: only extend the world in the overworld; elsewhere letterbox.
    if (!is_overworld(bus)) { gba::g_ws_pillarbox = 1; return; }
    gba::g_ws_pillarbox = 0;
    int w0x, w0y; Scroll s;
    if (!world_origin(bus, 1, &w0x, &w0y, &s)) return;  // need an anchor
    const uint32_t map_layout = rd32(bus, g_mapheader);  // gMapHeader.mapLayout
    if (!map_layout) return;

    // World-metatile region covering the central view (15 mt) + margins.
    const int mtx0 = w0x >> 1, mty0 = w0y >> 1;
    const int margin_mt = 4;  // > ceil(24px/16) per side; generous
    const int mx_lo = mtx0 - margin_mt, mx_hi = mtx0 + 15 + margin_mt;
    const int my_lo = mty0 - 1,         my_hi = mty0 + 11;

    // Save state we transiently clobber.
    ArmCpuState saved = g_cpu;
    uint32_t saved_ptr[3];
    for (int k = 0; k < 3; ++k)
        saved_ptr[k] = rd32(bus, g_tilemap_ptrs + static_cast<uint32_t>(k) * 4u);
    for (int k = 0; k < 3; ++k)
        wr32_iwram(bus, g_tilemap_ptrs + static_cast<uint32_t>(k) * 4u, kScratch[k]);
    // Make the synthetic guest draws side-effect free: shadow-tick mode turns
    // runtime_tick into a pure cycle counter (no device advance, no IRQ
    // delivery, no g_runtime_cycles change) — so the real run is unperturbed and
    // this is safe to call every frame during live play.
    const unsigned saved_shadow = g_runtime_shadow_tick;
    const unsigned long long saved_shadow_cyc = g_runtime_shadow_cycles;
    g_runtime_shadow_tick = 1u;
    // Preserve the VRAM scratch the synthetic draws overwrite (so VRAM stays
    // byte-identical to a non-sidecar run). Each draw touches offsets 0,1,0x20,
    // 0x21; 0x80 bytes/region covers it with margin.
    constexpr uint32_t kScratchSave = 0x80u;
    uint8_t scratch_bak[3][kScratchSave];
    for (int k = 0; k < 3; ++k)
        vram_save(bus, kScratch[k], scratch_bak[k], kScratchSave);

    g_in_active_fill = true;
    for (int my = my_lo; my <= my_hi; ++my) {
        for (int mx = mx_lo; mx <= mx_hi; ++mx) {
            uint16_t tiles[3][4];
            draw_one_metatile(bus, map_layout, mx, my, tiles);
            ++g_active_calls;
            // Store the 2x2 block at world tiles (2mx,2my)..(2mx+1,2my+1).
            const int wtl[4][2] = {{2*mx, 2*my}, {2*mx+1, 2*my},
                                   {2*mx, 2*my+1}, {2*mx+1, 2*my+1}};
            for (int t = 0; t < 4; ++t) {
                const int cx = wtl[t][0] & (kCacheW - 1);
                const int cy = wtl[t][1] & (kCacheH - 1);
                const int cidx = cy * kCacheW + cx;
                for (int k = 0; k < 3; ++k) g_cache[k][cidx] = tiles[k][t];
                if (!g_cache_valid[cidx]) ++g_cache_fills;
                g_cache_valid[cidx] = 1u;
            }
        }
    }
    g_in_active_fill = false;

    // Restore everything so the guest is bit-for-bit unaffected.
    g_runtime_shadow_tick = saved_shadow;
    g_runtime_shadow_cycles = saved_shadow_cyc;
    for (int k = 0; k < 3; ++k) {
        wr32_iwram(bus, g_tilemap_ptrs + static_cast<uint32_t>(k) * 4u, saved_ptr[k]);
        vram_restore(bus, kScratch[k], scratch_bak[k], kScratchSave);
    }
    g_cpu = saved;
}

unsigned long long g_prov_calls = 0, g_prov_hits = 0;
bool ws_sidecar_tilemap_entry(int bg, int hw_x, int screen_y,
                              uint16_t* out_entry) {
    ++g_prov_calls;
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
    ++g_prov_hits;
    return true;
}

// Adapter matching the PPU's provider pointer signature (int return).
extern "C" int ws_sidecar_provider_adapter(int bg, int hw_x, int screen_y,
                                           uint16_t* out_entry) {
    return ws_sidecar_tilemap_entry(bg, hw_x, screen_y, out_entry) ? 1 : 0;
}

bool ws_sidecar_dump(const char* path, int extra_cols_per_side) {
    if (!g_enabled || !path || !path[0]) return false;
    // Active-fill mode: render the expanded region via the guest's own draw
    // right before reporting, so the verify reflects true (incl. never-seen)
    // margins rather than only eviction-captured tiles.
    if (const char* a = std::getenv("GBARECOMP_WS_SC_ACTIVE"))
        if (a[0] && a[0] != '0') ws_sidecar_active_fill();
    gba::GbaBus* bus = active_bus();
    if (!bus) return false;
    std::FILE* f = std::fopen(path, "w");
    if (!f) return false;

    int cached = 0;
    for (int i = 0; i < kCacheW * kCacheH; ++i) cached += g_cache_valid[i];
    std::fprintf(f, "# widescreen sidecar verify report\n");
    std::fprintf(f, "draws=%llu syncs=%llu active_calls=%llu cache_filled=%d/%d "
                 "extra_px=%d\n", g_draws, g_syncs, g_active_calls, cached,
                 kCacheW * kCacheH, extra_cols_per_side);
    std::fprintf(f, "provider_calls=%llu provider_hits=%llu pillarbox=%d\n",
                 g_prov_calls, g_prov_hits, gba::g_ws_pillarbox);

    // VRAM screenblock audit for Strategy-A widening (64-wide field BG needs an
    // extra 2KB screenblock per BG + the char base must not collide).
    {
        const uint8_t* io = bus->io().raw();
        auto r16 = [&](uint32_t o){ return static_cast<int>(io[o]|(io[o+1]<<8)); };
        const int dispcnt = r16(0x00);
        std::fprintf(f, "DISPCNT=0x%04X mode=%d\n", dispcnt, dispcnt & 7);
        static const char* szname[4] = {"256x256","512x256","256x512","512x512"};
        for (int b = 0; b < 4; ++b) {
            const int cnt = r16(0x08 + b * 2);
            const int charBase = (cnt >> 2) & 3;     // x16KB
            const int scrBase = (cnt >> 8) & 31;     // x2KB
            const int size = (cnt >> 14) & 3;
            std::fprintf(f, "BG%dCNT=0x%04X prio=%d charBase=%d(0x%05X) "
                "scrBase=%d(0x%05X) size=%d(%s)\n", b, cnt, cnt & 3,
                charBase, charBase * 0x4000, scrBase, scrBase * 0x800,
                size, szname[size]);
        }
    }

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
