// ws_provenance.h — widescreen Step B tilemap-provenance probe.
//
// A DEBUG-ONLY, always-on host-side mirror of which WORLD metatile currently
// owns each slot of a Pokémon-style field BG tilemap ring (32x32 tiles =
// 256x256 px). It is fed continuously by the recompiler's GENERAL
// function-entry hook (g_runtime_fn_entry_hook in runtime_arm.h) watching the
// guest's DrawMetatileAt(mapLayout, offset, world_x, world_y): each call records
// that the 2x2 tile block at `offset` is owned by world metatile (world_x,
// world_y). A dump QUERIES this ring for the window of interest — it never arms
// recording at probe time (matches the always-on ring-buffer discipline).
//
// Purpose: empirically prove the widescreen seam. The field BG tilemap is only
// 256 px wide and text/non-affine BGs wrap mod 256, so a 288-px output span
// (central 240 + margins) must re-sample ~4 tilemap columns — the margin cannot
// show its true world tiles. The provenance report measures exactly which
// output columns alias which world tiles, per camera position, confirming that
// Step C injection (a host-side EXTENDED tilemap cache) is required and is not
// achievable by camera panning alone.
//
// Game-agnostic mechanism, game-specific PCs supplied at runtime via env:
//   GBARECOMP_WS_PROBE_DRAWMETATILE=<hex guest PC of DrawMetatileAt>
//       (FireRed USA = 0x0805A948) — REQUIRED to arm.
//   GBARECOMP_WS_PROBE_FIELDCAMERA=<hex IWRAM addr of gFieldCamera>  (optional)
//   GBARECOMP_WS_PROBE_GMAIN=<hex IWRAM addr of gMain>               (optional)
//   GBARECOMP_WS_PROBE_CB2_OVERWORLD=<hex guest PC of CB2_Overworld> (optional)
//   GBARECOMP_WS_PROBE_DUMP=<path>  — write the report here at run end.

#pragma once

#include <cstdint>

namespace gbarecomp {

// Read env, and if GBARECOMP_WS_PROBE_DRAWMETATILE is set, install the
// function-entry hook so the owner ring updates live. Idempotent.
void ws_provenance_init_from_env();

// True once armed (the DrawMetatileAt PC env was set and the hook installed).
bool ws_provenance_armed();

// Write a human-readable provenance report for the CURRENT guest state.
// `extra_cols_per_side` is the widescreen margin width in pixels (the runtime's
// --widescreen N). Returns false if not armed or the file can't be opened.
bool ws_provenance_dump(const char* path, int extra_cols_per_side);

}  // namespace gbarecomp
