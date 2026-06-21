// ws_sidecar.h — widescreen Step C recomp-native tilemap sidecar.
//
// The production counterpart to the Step B provenance probe. It supplies the
// TRUE extended world to the widescreen margins, which the 256x256 guest BG
// tilemap ring cannot (it wraps mod 256; see ws_provenance.h / Step B result).
//
// Strategy (recomp-native sidecar, ChatGPT design "B"): leave the guest's
// BGxCNT / VRAM / 32x32 ring entirely untouched (OFF stays byte-identical), and
// maintain a HOST-SIDE EXTENDED tilemap cache (64x32 tiles = 512x256 px) per
// field BG layer. The cache is populated WITHOUT reimplementing any guest map
// logic:
//   - The recompiler function-entry hook (g_runtime_fn_entry_hook) watches
//     DrawMetatileAt(mapLayout, offset, world_x, world_y) and maintains an
//     owner ring mapping each of the 1024 ring slots to its exact WORLD TILE.
//   - Once per frame, sync the cache from the guest's own live
//     gBGTilemapBuffers1/2/3 (layer routing already applied by the guest),
//     placing each resident slot's entry at its world-tile coordinate. A tile
//     stays resident for many frames before the camera evicts it, so its last
//     resident content is preserved in the cache before it is overwritten.
//
// At render time the wide PPU path asks ws_sidecar_tilemap_entry() for off-
// screen (margin) tilemap entries; it resolves the world tile from the BG
// scroll + owner ring and returns the cached entry. The PPU renders that entry
// with its existing tile/char/palette path.
//
// Game-agnostic mechanism; FRLG PCs/addrs supplied via env (so it works for any
// game by configuration):
//   GBARECOMP_WS_SIDECAR=1                      — enable (also needs --widescreen)
//   GBARECOMP_WS_SC_DRAWMETATILE=<hex PC>       — DrawMetatileAt (0x0805A948)
//   GBARECOMP_WS_SC_TILEMAP_PTRS=<hex IWRAM>    — addr of gBGTilemapBuffers1
//                                                 (1/2/3 are consecutive u32;
//                                                  FRLG = 0x03005014)
//   GBARECOMP_WS_SC_DUMP=<path>                 — write a verify report at end.

#pragma once

#include <cstdint>

namespace gbarecomp {

// Arm from env (idempotent). Installs the function-entry hook if enabled.
void ws_sidecar_init_from_env();
bool ws_sidecar_enabled();

// Sync the extended cache from the guest's live tilemap buffers. Call once per
// guest frame (VBlank) so resident tiles are captured before eviction.
void ws_sidecar_sync_frame();

// Resolve the cached tilemap entry for field BG `bg` (1..3) at hardware column
// `hw_x` (may be <0 or >=240) and screen row `screen_y`. Returns true and fills
// *out_entry (raw BG text-tilemap u16) when a cached world tile is available.
// Used by the PPU wide path for margin columns. Side-effect free.
bool ws_sidecar_tilemap_entry(int bg, int hw_x, int screen_y,
                              uint16_t* out_entry);

// Write a verify report (cache fill + expanded-span world-tile resolution) for
// the current state. Returns false if disabled / file error.
bool ws_sidecar_dump(const char* path, int extra_cols_per_side);

}  // namespace gbarecomp
