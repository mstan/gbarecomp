# Debugging gbarecomp games

The execution contract lives in `DEBUG.md` at the repo root and is
mandatory reading. This file is the longer-form companion: it
explains why the rules exist and how the always-on observability
fits together.

## Why always-on rings

Static recompilers fail in subtle, history-dependent ways. The
divergence you see on frame 1200 was caused by a register / memory
/ IO state difference that took root much earlier — and that
earlier state is gone by the time you decide to look. Live
inspection is too late.

Therefore: the runtime records, continuously, into a frame ring and
a per-store / per-block / per-call ring. The debug surface
**queries** these rings. It never arms recording.

If the event you need isn't in the ring, you extend the ring. You do
not switch to "run again with this trace armed."

See `DEBUG.md §RULE 0b` and the global memory note about this.

## Frame ring vs reverse-debugger ring

Two granularities cohabit:

| Granularity | When to use |
|-------------|-------------|
| Per-frame snapshot ring (~36k frames, ~10 min @ 60 fps) | "What did the state look like at frame N?" / "When did VRAM byte X first diverge from oracle?" / "Step backwards through frames to find the first divergence." |
| `rdb_*` rings (per-store / per-block / per-call) | "Which instruction wrote VRAM byte X?" / "Show me every block entered between block 100k and 110k with PC in range Y." / "Reconstruct IWRAM at block 1.2M." |

Walk frame ring backwards first to localize **which frame** went
wrong. Then drop into `rdb_*` to identify **which write** went
wrong.

## Native ↔ oracle bridge

We run two processes:

1. **Native build** — the recompiled binary, exposing TCP on
   `19842` (default).
2. **Oracle build** — same project, linked against an mGBA bridge,
   exposing TCP on `19843`.

The two processes share the same TCP command grammar for the
overlapping subset. A `framebuf_diff` request hits both, diffs the
buffers, and reports the first differing pixel.

The oracle binary is a separate target so we never accidentally pull
mGBA's license into the native runtime.

## Sync points

Use hardware events, not frame number alone:

- VBlank IRQ count.
- DMA completion count (per channel).
- Timer overflow count (per timer).
- SWI count (and per-SWI count).
- BIOS-IRQ-return count.
- Specific PC at specific function entry.

Two processes that happen to be on the same "frame number" may have
diverged elsewhere. Two processes that just took their 412th VBlank
IRQ are at the same execution point.

## Classification

When you have a divergence:

1. **Decoder / codegen** — the same ARM/THUMB word is producing
   different semantics on our side. Confirm by feeding the word
   through `decoder_smoke` and comparing to a reference (mGBA
   debugger output, jsmolka tests).
2. **Runtime / timing** — semantics match, but cycle counts or
   scheduler ordering differ. Look at `scheduler_state` and
   `timer_state` diffs.
3. **Memory / bus** — load or store hit the wrong region or wrong
   mirror, or waitstates produced different values for unaligned
   reads. Look at `read_io` diffs near the suspect time.
4. **IO** — IO write produced different downstream effect. Look at
   `io_diff` and per-device state.
5. **IRQ / DMA / timer** — scheduler fired in the wrong order.
   Look at `irq_state`, `dma_state`, `timer_state`.
6. **PPU** — VRAM/OAM/PAL writes match but framebuffer differs.
   Almost certainly a render bug; compare `ppu_state` and
   `framebuf_diff`.
7. **Audio** — only matters when the game depends on FIFO/timer/DMA
   for game logic timing (it can — Minish Cap drives some timing
   off audio DMA).
8. **Save chip** — wrong save type detected, or save chip command
   state machine wrong.
9. **Game metadata** — symbols missing, function boundary wrong,
   ROM hash mismatch.

The classification determines who fixes it: decoder/codegen → tool;
runtime/timing/io/devices → `src/gba/`; metadata → `game.toml`.

## What never happens

- Editing `generated/*.c` by hand.
- Adding `if (game == "minish_cap")` to the GBA core.
- Stubbing an SWI to "return what the game expects."
- Silencing an unmapped IO read because it's noisy.
- Pausing both native and oracle and stepping in lockstep.
