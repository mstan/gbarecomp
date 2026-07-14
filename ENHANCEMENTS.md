# gbarecomp Enhancements — Breaking Faithfulness On Purpose

This project is LLE-first and proven against the oracle (see `PRINCIPLES.md`,
`CLAUDE.md`). An **enhancement** is the one place we deviate from the faithful
core on purpose — and only ever as an **opt-in, default-off layer** that reduces
byte-for-byte to the faithful build when disabled. The shared cross-project rules
live in `../../recomp-template/ENHANCEMENTS.md` (Rule 1: default is
system-authentic; Rule 2: runner capability vs game policy; etc.). This file is
the gbarecomp-specific catalog.

The invariant that governs all of them: **LLE is the foundation that keeps HLE
(and every other enhancement) from rotting.** Because a proven-faithful path
always sits underneath and a default-off switch restores it exactly, the
enhancement can be aggressive without eroding correctness.

---

## 1. HLE BIOS backend — SHIPPED (2026-07-02)

Opt-in High-Level Emulation of the GBA BIOS, as an *either/or* alternative to the
recompiled real BIOS (LLE). LLE stays the **default and the correctness oracle**;
HLE is a switch-in on top.

- **Selection:** `[bios].hle = true` in a game's `game.toml` (default `false`);
  `--bios-hle` / `--no-bios-hle`; `GBARECOMP_BIOS_HLE`. Boot-skip sub-toggle:
  `[bios].hle_keep_intro` / `--bios-hle-keep-intro` / `GBARECOMP_BIOS_HLE_KEEP_INTRO`.
- **What it does:** (a) services arithmetic/memory SWIs in-runtime
  (Div/Sqrt/ArcTan/CpuSet/CpuFastSet/affine/decompressors/BitUnPack/etc.); (b)
  skips the BIOS boot intro by synthesizing the post-boot handoff state and
  jumping to the cart entry.
- **Falls back to LLE** for the machine-state SWIs
  (Halt/Stop/IntrWait/VBlankIntrWait/SoftReset/RegisterRamReset/sound-driver/
  MultiBoot) and for anything it doesn't implement — so HLE is never load-bearing
  beyond what it covers, and never breaks a game.
- **Code:** `src/runtime/bios_hle.{h,cpp}` (SWIs ported from mGBA `src/gba/bios.c`,
  MPL-2.0, credited in `THIRD_PARTY_ATTRIBUTION.md`); the seam is one null-by-default
  hook `g_bios_hle_hook` at the top of `runtime_swi()`. Startup banners
  `bios_backend=` / `bios_boot=`.
- **Verified:** Minish Cap + Pokémon Emerald boot to gameplay with the intro
  skipped; Emerald flash save round-trips under HLE.
- **Known limits (LLE stays the oracle):** cycle cost is approximate (only
  Div/Sqrt/ArcTan/LZ77 have closed-form stalls ported); the affine SWIs and
  MidiKey2Freq use host float, so they can differ from the real BIOS in the low
  bits. Candidate for co-sim tuning: derive/refine the approximations by diffing
  HLE against LLE in the first-divergence oracle.

### The BIOS dump is still required, even with HLE on

By design, HLE mode **still loads and hash-verifies the BIOS `.bin`** (and prompts
for it if missing). HLE skips the *intro*, not the *BIOS*. The dump stays
load-bearing three ways: (1) the LLE SWI fallback executes recompiled BIOS code;
(2) faithful BIOS-region reads — the open-bus prefetch model, direct BIOS reads
some games do, and the IRQ dispatcher at `0x18` — read real BIOS bytes; (3) the
hash gate. So today, HLE = "skip intro + shortcut SWIs" *on top of* a required
BIOS.

---

## 2. No-BIOS HLE tier — DEFERRED (documented, not planned)

The version of HLE that would let a user run **without supplying a BIOS dump at
all** (the usual real-world appeal of "HLE BIOS"). Deferred deliberately: it's a
lot of work for a niche benefit, and the shipped HLE + LLE already covers the
gameplay goal. Captured here so it isn't lost.

**What it would take** (all three, because the dump is load-bearing three ways —
see §1):

1. **Full SWI coverage** — HLE *every* SWI including the machine-state ones
   currently on the LLE fallback: `Halt`/`Stop` (drive the halt/IME wait),
   `IntrWait`/`VBlankIntrWait` (the IRQ-wait loop against `IE`/`IF`/`IME` +
   `0x03007FF8` BIOS flags), `SoftReset`/`RegisterRamReset` (state resets),
   MultiBoot. These interact with the halt/IRQ machinery, which is exactly why
   they were left on LLE — they're the hard, timing-sensitive ones.
2. **HLE the IRQ entry** — replace the recompiled BIOS IRQ dispatcher at `0x18`
   (register save/restore, read user handler at `0x03007FFC`, call it, `IF`
   ack) with an in-runtime equivalent, so no BIOS code executes on an interrupt.
3. **BIOS-region read policy without the dump** — decide what a read of
   `0x00000000–0x00003FFF` (and the open-bus prefetch latch) returns when there
   are no real BIOS bytes. Two options:
   - **Approximation** — return zero / a synthesized open-bus value. Zero
     dependency, small accuracy cost (games that read the BIOS for RNG /
     checksums / open-bus quirks diverge; see the MC-HP-002 open-bus finding).
   - **Redistributable replacement BIOS** — bundle a freely-licensed
     reimplementation (e.g. Normmatt's open-source GBA BIOS, or the Cult-of-GBA
     BIOS) and run/recompile *that* instead of the user's dump. Higher fidelity,
     no user dump needed, but adds a vendored asset + its recompilation.

**Tradeoff / decision.** Full accuracy → keep requiring the real dump (status quo,
best fidelity, LLE-backed). Zero-dependency distribution → the no-BIOS tier, at
some accuracy cost or the weight of a bundled replacement BIOS. **Decision
(2026-07-02): not worth building now.** Revisit only if shipping to users without
a BIOS dump becomes a real requirement.

Per Rule 1 it would land as a further opt-in tier (e.g. `[bios].hle_no_dump`),
never disturbing the LLE default or the accurate HLE-on-LLE path.

---

## 3. Widescreen / expanded view

The shared PPU can expose a wider logical surface without changing the GBA's
160-line height. The faithful 240x160 renderer remains the default and uses its
original code path. A game opts in by setting `RunOptions::max_view_width` above
240, and the user selects a total logical width with `[video].view_width`,
`--view-width`, or `GBARECOMP_VIEW_WIDTH`. Unsupported games clamp to 240, so a
stale preference cannot change them. The older `widescreen=N` spelling remains
a compatibility alias for `240 + 2*N`.

The shared surface is only the mechanical capability. Each opted-in game still
owns scene policy and any camera, tile-streaming, spawn, or culling changes
needed to produce authentic content in the added margins. Unsupported scenes
must pillarbox rather than expose stale VRAM. `GBARECOMP_WS_WIP=1` is an explicit
development override for renderer experiments; the Pokemon-specific Step C
sidecar remains WIP-gated and is not a correctness layer. Its design and known
limitations remain in `docs/WIDESCREEN_STEPC_PLAN.md`.

Two null-by-default seams support narrow game-owned LLE work without changing
the faithful renderer: `g_rom_read32_override` can replace reviewed cartridge
literal reads after ROM identity verification, and `g_ws_obj_x_provider` can
interpret an opted-in game's extended OBJ coordinates in the wide PPU path.
Neither is consulted by native 240x160 OBJ rendering; games must prove their
coordinate invariant and leave both hooks unset unless expanded view is active.
