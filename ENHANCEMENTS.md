# ENHANCEMENTS.md — deferred accuracy hardening & known cosim divergences

Candidate improvements and known-but-deferred divergences surfaced by the
differential co-simulation oracle (`COSIM_ORACLE.md`). Distinct from
`recomp-template/ENHANCEMENTS.md`, which governs opt-in *faithfulness-breaking*
features (widescreen, etc.). Entries here are the opposite: places where the
faithful core is *almost* bit-exact and the last delta is either benign or
awaiting an independent-oracle adjudication before a fix is justified.

---

## MinishCap — open-bus prefetch latch differs across backends during HALT

**Status:** OPEN, deferred (benign in effect; needs NBA/mGBA to adjudicate).
Surfaced 2026-07-01 by the recomp-vs-interp first-divergence oracle.

**Symptom.** After the harness-fidelity fix (`be01a1b`, force-interp data
accesses routed through the runtime bus bridge), the recomp and force-interp
backends are **per-instruction bit-identical (cycle + PC) for all 799,688
instructions** of a 7-frame MinishCap boot trace. The cosim then halts at a
single residual: during the BIOS `IntrWait`/HALT loop (BIOS pc ~0x348, frame 6+,
`halted=1`), the **open-bus prefetch sub-hash differs** between the two backends.
Every other subsystem is identical at that checkpoint — CPU registers, CPSR,
IWRAM, EWRAM, VRAM, PAL, OAM, IO (IE/IF/IME/timers/DMA/WAITCNT), audio, PPU, and
the master clock (`g_runtime_cycles`) all match. The difference is **persistent**
(frozen while halted; it does not self-heal, unlike the DMA-steal transient that
motivated `be01a1b`).

**What it is.** The Game-Pak / BIOS open-bus prefetch latch
(`gba_bus.cpp` `bios_prefetch_`, updated by `latch_bios_prefetch()` from
`runtime_should_yield()` while PC < 0x4000). The two backends latched a
*different* prefetch word at the last BIOS instruction before entering the halt
loop, and it stays frozen there. This value is **dead state** — it only becomes
guest-visible if code reads the protected-BIOS region or unmapped memory (open
bus) while this value is latched. See the MC-HP-002 animation-reframe fix
(`project_mc_hp_002_animation_reframe`) for the one class of bug where an
open-bus read *is* load-bearing; this divergence is that same latch, so it is not
safe to dismiss outright even though no incorrect read has been observed here.

**Why deferred, not fixed now.**
- Effect is benign at every observed checkpoint (no divergent open-bus *read*;
  all architecturally-live state matches).
- The pairing is recomp-vs-interpreter — **both implementations are ours**, so it
  cannot tell *which* backend latched the correct value. Resolving "which is
  right" requires the independent cycle-accurate oracle (NanoBoyAdvance, :19844,
  or mGBA :19843), which is the `COSIM_ORACLE.md` §8 escalation — not yet built.
- Per `feedback_constrain_surface_area`: solidify the tractable common case
  first. The io/timer/DMA-steal transient class (the high-frequency noise) is
  already eliminated; this is a low-frequency corner.

**Suspected cause (unconfirmed).** The last `latch_bios_prefetch` before the CPU
halts fires at a slightly different instruction between backends, or the
force-interp SWI/halt-entry path latches once differently than the generated
BIOS. Confirming needs the raw latched values (add `bios_open_bus()` to the cosim
`dev`/`cpu` dump) plus the last few pre-halt PCs from the fp ring.

**To resolve when picked up.**
1. Add the raw `bios_open_bus()` value to the cosim `dev` dump; capture both
   backends' value + last pre-halt PC at the divergent checkpoint.
2. Adjudicate against NanoBoyAdvance/mGBA: whichever backend matches the
   hardware oracle's open-bus value at that PC is correct.
3. Fix the wrong side's latch timing in the recompiler/runtime (never a stub or
   a cosim-hash exclusion — the latch is real state, cf. MC-HP-002).

Cross-refs: `COSIM_ORACLE.md` (method + iteration log), `ISSUES.md`,
`gba_bus.cpp` (`bios_prefetch_`, `latch_bios_prefetch`, open-bus read paths),
commits `be01a1b` (bridge fix) / `21f5b1b` (fp cycle-diff verdict).
