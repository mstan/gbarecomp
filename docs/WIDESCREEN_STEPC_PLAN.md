# Widescreen Step C — Strategy A (widen the guest field BG ring 32→64)

Opt-in 16:9 widescreen for Pokémon FRLG overworld. **OFF = byte-identical /
system-faithful** (the gate); **ON = the guest renders a true 64-tile-wide field
BG**, so margins show the real world (incl. never-before-seen tiles), with no
host-side tile synthesis. Adapts the snesrecomp (Zelda ALttP) opt-in widescreen
pattern to a hardware-level GBA recomp.

## Pattern borrowed from snesrecomp/LegendofZeldaAlttpRecomp
- **Single master switch** `g_ws_active` / `g_ws_extra` (extra cols/side). Step A
  already added these (`src/runtime/runtime.cpp`). Default 0 → faithful.
- **Capability vs policy separation**: the *capability* (render wider) lives in
  the runner/PPU; the *policy* (which screens are wide, how wide, clamped to
  bounds) is per-game, computed each frame from live guest state
  (`ZeldaConfigurePpuSideSpace` reads scroll + room bounds). Our equivalent:
  gate on `gMain.callback2 == CB2_Overworld` and clamp to map/connection bounds.
- **Content from the game's own data** — no separate generated tiles. Zelda
  could just extend PPU read bounds because its overworld tilemap is ALREADY
  512px in VRAM. **FRLG is the key difference: its field tilemap is 32 tiles
  and WRAPS mod 256 (Step B finding)** → we must make the guest materialize 64.

## VRAM audit (FireRed USA, overworld; from ws_sidecar dump)
Mode 0. Field BG1/2/3 share char base 0 (0x00000). Screenblocks packed:
BG2=sb28(0x0E000), BG1=sb29(0x0E800), BG3=sb30(0x0F000), BG0=sb31(0x0F800).
`sb24..27` (0x0C000..0x0DFFF, 8 KB, char block 3) are **free**.
A 512-wide BG (size=1) uses 2 consecutive screenblocks. Need 3 field BGs (6 sb)
+ BG0 (1) = 7 of the 8 available sb24..31. **Feasible**, but requires relocating
the field screenblocks (rewrite BGxCNT scrBase + move tilemap) when WS active.

## FRLG symbols (guest, all_symbols.tsv)
DrawMetatileAt 0x0805A948, CurrentMapDrawMetatileAt 0x0805A8E8,
DrawWholeMapViewInternal 0x0805A6A8, RedrawMapSlice{N/S/E/W} 0x0805A778/…,
MapPosToBgTilemapOffset 0x0805AAE8, CameraUpdate 0x0805ABB0, CameraMove
0x08059530, CB2_Overworld 0x080565B4, gMain 0x030030F0 (callback2 +4),
gBGTilemapBuffers1/2/3 = u32 ptrs @ 0x03005014/18/1C, sFieldCameraOffset
0x03000E90, gTotalCameraPixelOffsetX/Y 0x0300506C/68, gMapHeader 0x02036DFC
(mapLayout @ +0), MapGridGetMetatileIdAt 0x08058E48.

## Implementation steps (each gated on g_ws_active; off path untouched)
1. **VRAM relocation (field init / first overworld frame, when WS on):** set
   BG1/2/3 BGxCNT size=1 (512x256) and scrBase to consecutive pairs in
   sb24..29 (BG1=24/25, BG2=26/27, BG3=28/29), BG0 stays sb31. Move the existing
   32-wide tilemap data into the left half of each new screenblock pair.
2. **Widen the tilemap buffers**: gBGTilemapBuffers1/2/3 (the EWRAM source
   buffers DMA'd to VRAM) to 64-wide (1024→2048 u16). Either re-point the guest
   ptrs at host-allocated wider guest-RAM buffers, or widen via the heap. The
   buffer→VRAM copy (ScheduleBgCopyTilemapToVram / its DMA) must cover 64 cols.
3. **Widen the draw routines** to fill cols 0..63 from the virtual map
   (gBackupMapData has the extended region). Via the marker-anchored injector
   (PRINCIPLES Q5 / the g_runtime_fn_entry_hook): at DrawWholeMapViewInternal /
   RedrawMapSlice*, when g_ws_active, extend the iteration to the extra margin
   metatile columns and feed MapPosToBgTilemapOffset a 64-wide range
   (&0x1F → &0x3F semantics). Reuse the guest's own DrawMetatileAt (no
   metatile→tile reimplementation — rules-compliant).
4. **PPU** wide path reads the now-512-wide field BGs (standard GBA 2-screenblock
   addressing — verify render_scanline_wide handles size=1). Central 240px stays
   byte-identical when off.
5. **Policy**: enable only when callback2==CB2_Overworld; clamp extra cols to map
   width + active connections (gMapHeader / connection data) so no OOB; pillarbox
   menus/transitions. Mirror Zelda's per-frame margin computation.
6. **Verify**: OFF byte-identical gate (FireRed 600f --widescreen 0 ==
   62265916/2251/116/578); ON → provenance/sidecar margins resolve to TRUE
   consecutive world tiles incl. never-seen (vs Step B seam); visual at +24/side.

## Reuse / supersede
- Step C1 sidecar (eviction cache, src/debug/ws_sidecar.cpp): its *render-from-
  host-cache* path is superseded by Strategy A (guest draws into VRAM; PPU reads
  VRAM). BUT its fn-entry-hook owner mapping + per-frame sync + the verify dump
  remain the **verification oracle** and can drive step 3's extra-draw decisions.
- The general fn-entry hook (Step B) is the injection substrate for step 3.

## Risks
- Injector complexity (anchoring host snippets at guest PCs that survive regen).
- Screenblock-relocation timing (must happen before the field's first BG copy;
  re-apply after map loads / save-state restore).
- DMA/HBlank tilemap-copy interactions (see MC-HP-003 timed-DMA lessons).
- OAM sprite x for margin sprites (Step A wide path decodes signed 9-bit x).
- Per-game: bounds/connection clamping is FRLG-specific policy.
