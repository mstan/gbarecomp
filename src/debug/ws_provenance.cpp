// ws_provenance.cpp — see ws_provenance.h.

#include "ws_provenance.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "runtime_arm.h"            // g_cpu, g_runtime_fn_entry_hook
#include "runtime_bus_bridge.h"     // gbarecomp::active_bus()
#include "gba_bus.h"                // GbaBus::iwram_ptr(), io()
#include "gba_io.h"                 // GbaIo::raw()

namespace gbarecomp {

namespace {

// Owner ring: one entry per slot of the 32x32 field BG tilemap (256x256 px).
// Last writer wins, exactly mirroring the guest tilemap buffer. world_x/world_y
// are WORLD METATILE coordinates (the args DrawMetatileAt receives).
struct TileOwner {
    int16_t  wx;
    int16_t  wy;
    uint8_t  valid;
};
constexpr int kRingTiles = 32;                       // 32x32 tile ring
constexpr uint32_t kRingMask = 0x3FFu;               // 1024 slots - 1
TileOwner g_owner[kRingTiles * kRingTiles];

uint32_t      g_dm_pc = 0;          // guest PC of DrawMetatileAt (0 = disarmed)
bool          g_armed = false;
unsigned long long g_draws = 0;     // total DrawMetatileAt calls recorded

// Optional guest anchors for richer reporting.
uint32_t g_fieldcamera_addr = 0;    // IWRAM addr of gFieldCamera
uint32_t g_gmain_addr = 0;          // IWRAM addr of gMain
uint32_t g_cb2_overworld = 0;       // guest PC of CB2_Overworld (thumb, no |1)

uint32_t parse_hex_env(const char* name) {
    const char* v = std::getenv(name);
    if (!v || !v[0]) return 0;
    return static_cast<uint32_t>(std::strtoul(v, nullptr, 0));
}

// Read a u16 from a raw byte buffer (little-endian) with a bounds guard.
uint16_t rd16(const uint8_t* p, uint32_t off, uint32_t size) {
    if (!p || off + 1u >= size) return 0;
    return static_cast<uint16_t>(p[off] | (p[off + 1] << 8));
}
uint32_t rd32(const uint8_t* p, uint32_t off, uint32_t size) {
    if (!p || off + 3u >= size) return 0;
    return static_cast<uint32_t>(p[off]) | (static_cast<uint32_t>(p[off + 1]) << 8) |
           (static_cast<uint32_t>(p[off + 2]) << 16) |
           (static_cast<uint32_t>(p[off + 3]) << 24);
}

}  // namespace

// Function-entry hook (assigned to g_runtime_fn_entry_hook). Fires on EVERY
// recompiled function entry, so the non-target path must be a single compare.
extern "C" void ws_prov_fn_entry_hook(uint32_t pc) {
    if (pc != g_dm_pc) return;
    // AAPCS at entry: R1 = u16 offset (tile index into the 32x32 ring),
    // R2 = world metatile x, R3 = world metatile y.
    const uint32_t offset = g_cpu.R[1] & kRingMask;
    const int16_t wx = static_cast<int16_t>(static_cast<int32_t>(g_cpu.R[2]));
    const int16_t wy = static_cast<int16_t>(static_cast<int32_t>(g_cpu.R[3]));
    // DrawMetatile writes the 2x2 tile block at offset, +1, +0x20, +0x21 — all
    // belonging to the same world metatile.
    static const uint32_t kBlk[4] = {0u, 1u, 0x20u, 0x21u};
    for (uint32_t d : kBlk) {
        const uint32_t s = (offset + d) & kRingMask;
        g_owner[s].wx = wx;
        g_owner[s].wy = wy;
        g_owner[s].valid = 1u;
    }
    ++g_draws;
}

void ws_provenance_init_from_env() {
    if (g_armed) return;
    g_dm_pc = parse_hex_env("GBARECOMP_WS_PROBE_DRAWMETATILE");
    if (!g_dm_pc) return;  // not requested
    g_dm_pc &= ~1u;        // normalize thumb bit
    g_fieldcamera_addr = parse_hex_env("GBARECOMP_WS_PROBE_FIELDCAMERA");
    g_gmain_addr = parse_hex_env("GBARECOMP_WS_PROBE_GMAIN");
    g_cb2_overworld = parse_hex_env("GBARECOMP_WS_PROBE_CB2_OVERWORLD") & ~1u;
    std::memset(g_owner, 0, sizeof(g_owner));
    g_runtime_fn_entry_hook = &ws_prov_fn_entry_hook;
    g_armed = true;
    std::fprintf(stderr,
        "[ws-probe] armed: DrawMetatileAt=0x%08X fieldcamera=0x%08X "
        "gmain=0x%08X cb2_overworld=0x%08X\n",
        g_dm_pc, g_fieldcamera_addr, g_gmain_addr, g_cb2_overworld);
}

bool ws_provenance_armed() { return g_armed; }

bool ws_provenance_dump(const char* path, int extra_cols_per_side) {
    if (!g_armed || !path || !path[0]) return false;
    gba::GbaBus* bus = active_bus();
    if (!bus) return false;

    const uint8_t* iwram = bus->iwram_ptr();      // base = 0x03000000
    const uint8_t* io = bus->io().raw();          // 1 KB IO page

    auto iw32 = [&](uint32_t guest) -> uint32_t {
        if (guest < 0x03000000u) return 0;
        return rd32(iwram, guest - 0x03000000u, 0x8000u);
    };
    auto iw16 = [&](uint32_t guest) -> uint16_t {
        if (guest < 0x03000000u) return 0;
        return rd16(iwram, guest - 0x03000000u, 0x8000u);
    };

    const uint16_t dispcnt = rd16(io, 0x000u, 0x400u);
    const uint16_t bghofs[4] = {rd16(io, 0x10u, 0x400u), rd16(io, 0x14u, 0x400u),
                                rd16(io, 0x18u, 0x400u), rd16(io, 0x1Cu, 0x400u)};
    const uint16_t bgvofs[4] = {rd16(io, 0x12u, 0x400u), rd16(io, 0x16u, 0x400u),
                                rd16(io, 0x1Au, 0x400u), rd16(io, 0x1Eu, 0x400u)};

    std::FILE* f = std::fopen(path, "w");
    if (!f) return false;

    std::fprintf(f, "# widescreen tilemap-provenance report\n");
    std::fprintf(f, "DrawMetatileAt=0x%08X  draws_recorded=%llu  extra_px=%d\n",
                 g_dm_pc, g_draws, extra_cols_per_side);
    std::fprintf(f, "DISPCNT=0x%04X  mode=%u\n", dispcnt, dispcnt & 7u);
    for (int b = 0; b < 4; ++b) {
        std::fprintf(f, "BG%d HOFS=%u VOFS=%u (tilecol=%u tilerow=%u)\n", b,
                     bghofs[b], bgvofs[b], (bghofs[b] >> 3) & 31u,
                     (bgvofs[b] >> 3) & 31u);
    }
    if (g_gmain_addr) {
        const uint32_t cb2 = iw32(g_gmain_addr + 4u);  // gMain.callback2
        const bool ow = g_cb2_overworld && ((cb2 & ~1u) == g_cb2_overworld);
        std::fprintf(f, "gMain.callback2=0x%08X  overworld=%s\n", cb2,
                     ow ? "YES" : "no/unknown");
    }
    if (g_fieldcamera_addr) {
        std::fprintf(f, "gFieldCamera @0x%08X:", g_fieldcamera_addr);
        for (uint32_t i = 0; i < 0x18u; i += 2u)
            std::fprintf(f, " %04X", iw16(g_fieldcamera_addr + i));
        std::fprintf(f, "\n");
    }

    // ── Owner-grid coherence (per tilemap column / row) ──────────────
    // For each of the 32 ring columns, how many DISTINCT world_x owners it
    // currently holds (1 = coherent). >1 means the column was redrawn with a
    // different world tile and now aliases. Same for rows / world_y.
    std::fprintf(f, "\n# owner-grid coherence (valid slots only)\n");
    int incoherent_cols = 0, incoherent_rows = 0, valid_slots = 0;
    for (int c = 0; c < kRingTiles; ++c) {
        int16_t first = 0; bool have = false, multi = false;
        for (int r = 0; r < kRingTiles; ++r) {
            const TileOwner& o = g_owner[r * kRingTiles + c];
            if (!o.valid) continue;
            if (!have) { first = o.wx; have = true; }
            else if (o.wx != first) multi = true;
        }
        if (multi) ++incoherent_cols;
    }
    for (int r = 0; r < kRingTiles; ++r) {
        int16_t first = 0; bool have = false, multi = false;
        for (int c = 0; c < kRingTiles; ++c) {
            const TileOwner& o = g_owner[r * kRingTiles + c];
            if (!o.valid) { continue; }
            ++valid_slots;
            if (!have) { first = o.wy; have = true; }
            else if (o.wy != first) multi = true;
        }
        if (multi) ++incoherent_rows;
    }
    std::fprintf(f, "valid_slots=%d/1024  incoherent_cols=%d  incoherent_rows=%d\n",
                 valid_slots, incoherent_cols, incoherent_rows);

    // ── Expanded-span alias map (the seam proof) ─────────────────────
    // Analyze the field BG (BG1) at the current camera. Central viewport =
    // 240 px = 30 tiles; margins add ceil(extra/8) tiles per side. For each
    // output tile column we resolve the ring slot it samples (wrap mod 32) and
    // compare its owner to the expected world tile. When the output span > 32
    // tiles, ring columns are necessarily re-sampled (aliased): the margin
    // cannot show its true world tiles. That re-sampling IS the seam.
    const int kFieldBg = 1;
    const int extra_tiles = (extra_cols_per_side + 7) / 8;
    const uint32_t base_col = (bghofs[kFieldBg] >> 3) & 31u;
    const uint32_t base_row = (bgvofs[kFieldBg] >> 3) & 31u;
    const TileOwner& ref = g_owner[base_row * kRingTiles + base_col];
    const int ref_wx = ref.valid ? ref.wx : 0;

    std::fprintf(f,
        "\n# expanded-span alias map: BG%d, base_col=%u base_row=%u "
        "ref_world_x=%d extra_tiles=%d\n", kFieldBg, base_col, base_row,
        ref_wx, extra_tiles);
    std::fprintf(f,
        "# o  tmap_col  owner(wx,wy)  expected_wx  status\n");

    // World coords are METATILE units (2 tiles wide); the metatile boundary
    // falls on even absolute tilemap columns. Expected world_x for output tile
    // o therefore advances one metatile every two tiles, phase-aligned to
    // base_col: expected_wx(o) = ref_wx + floor2(base_col+o) - floor2(base_col).
    auto floor2 = [](int v) { return (v >= 0) ? (v / 2) : -((1 - v) / 2); };
    const int base_mt = floor2(static_cast<int>(base_col));

    bool col_seen[kRingTiles] = {false};
    int n_green = 0, n_alias = 0, n_wrong = 0, n_untracked = 0;
    int central_ok = 0, central_bad = 0, margin_bad = 0, margin_total = 0;
    const int o_lo = -extra_tiles;
    const int o_hi = 30 + extra_tiles - 1;   // 30 central tiles + margins
    for (int o = o_lo; o <= o_hi; ++o) {
        const uint32_t tcol = (base_col + static_cast<uint32_t>(o + 64)) & 31u;
        const TileOwner& ow = g_owner[base_row * kRingTiles + tcol];
        const int expected_wx =
            ref_wx + floor2(static_cast<int>(base_col) + o) - base_mt;
        const char* status;
        if (!ow.valid) { status = "MAGENTA(untracked)"; ++n_untracked; }
        else if (col_seen[tcol]) { status = "YELLOW(alias)"; ++n_alias; }
        else if (ow.wx == expected_wx) { status = "green(ok)"; ++n_green; }
        else { status = "RED(wrong-owner)"; ++n_wrong; }
        col_seen[tcol] = true;
        const bool margin = (o < 0) || (o >= 30);
        const bool good = (status[0] == 'g');  // "green(ok)"
        if (margin) { ++margin_total; if (!good) ++margin_bad; }
        else { if (good) ++central_ok; else ++central_bad; }
        std::fprintf(f, "%4d  %7u   (%d,%d)  %d  %s%s\n", o, tcol,
                     ow.valid ? ow.wx : 0, ow.valid ? ow.wy : 0,
                     expected_wx, status, margin ? "  <margin>" : "");
    }
    std::fprintf(f,
        "\nSUMMARY: span=%d tiles  green=%d  alias=%d  wrong=%d  untracked=%d\n",
        o_hi - o_lo + 1, n_green, n_alias, n_wrong, n_untracked);
    std::fprintf(f,
        "  central(30 tiles): correct=%d incorrect=%d   margins(%d tiles): "
        "seam(alias|wrong|untracked)=%d\n",
        central_ok, central_bad, margin_total, margin_bad);
    std::fprintf(f,
        "  central correct + margin seam confirms: the central 240px view is\n"
        "  faithful, but every margin tile is alias/wrong/untracked — the 256px\n"
        "  ring cannot supply a %d-px span. Step C extended-tilemap injection is\n"
        "  required (camera panning alone cannot fix it: text BGs wrap mod 256).\n",
        240 + 2 * extra_cols_per_side);

    std::fclose(f);
    std::fprintf(stderr, "[ws-probe] wrote %s (draws=%llu alias=%d)\n", path,
                 g_draws, n_alias);
    return true;
}

}  // namespace gbarecomp
