# Shared-engine HLE opportunities and ordinary-RAM DMA pilot

Investigation: 2026-10-06, central issue `beads-bce3`. The bounded pilot is
shared across titles; it does not intercept a title routine. It remains opt-in
and does not establish a whole-game speedup or authorize a release default.

## Opportunity assessment

| Engine boundary | Evidence status | Opportunity and disposition |
| --- | --- | --- |
| Graphics: scanline/compositor, tile decoding and composition | **Unknown** whole-title host share in the available GBA evidence | Larger batched/native rendering or cached tile composition can remove considerable repeated work. Preserve CPU-visible display state, HBlank/VBlank effects and DMA ordering. Not eliminated by choosing this smaller pilot. |
| Audio: PSG/direct sound, mixing and sample production | **Measured** historical FIFO helper improvement, 7.422 to 5.929 ns/op; **unknown** aggregate audio share | An entire buffer-production service could replace per-sample bookkeeping. Needs PCM/audio-pacing and FIFO completion contracts. Existing helper results do not prove such an HLE benefit. |
| DMA / bulk memory | **Estimated** cost: one virtual bus read and write per data unit; **unknown** title-wide eligible share | Selected bounded service: ordinary contiguous non-overlapping EWRAM/IWRAM copies. Native bulk copying removes all per-unit bus dispatch. Other DMA families remain outside this selected family. |
| Scheduler / polling / device event ownership | **Measured** historical trace/dispatch helper costs in `RUNTIME_PERFORMANCE.md`; **unknown** gameplay attribution | Coarser operation/event scheduling and polling elimination may be larger wins. Maintain frame/audio/IRQ/CPU-visible completion ordering rather than duplicating every internal tick. Needs a separate boundary investigation. |
| BIOS / services | **Existing** HLE copies/decompression/math; **known gap** absent generated BIOS in historical smoke builds | Native whole-service operations are legitimate candidates. The recompiled BIOS floor must actually be linked and exercised before replacing its covered service. This DMA pilot uses the functioning DMA floor and reads no BIOS. |
| Cartridge coprocessors / sensors | **Unknown** host share, title/hardware dependent | Broader service replacements remain candidates when caller-visible registers, save data and lifecycle are understood. No evidence ranks them above this pilot. |

Historical evidence comes from `RUNTIME_PERFORMANCE.md`; its microbenchmarks
and no-input MinishCap startup runs are not DMA HLE performance/gameplay
qualification. No Xbox measurement is implied.

## Selection and contract

Configure `GBARECOMP_DMA_RAM_IMPLEMENTATION=LLE|HLE` (default `LLE`). The choice
is fixed during the build; no launcher/environment/runtime mode exists.
The runtime startup reports `dma_ram_backend`. Use separate build directories.

Both builds expose the existing GbaIo DMA register interface. The supported
HLE family has incrementing source and incrementing/reload destination,
16- or 32-bit aligned units, at least four units, and complete non-overlapping
physical EWRAM/IWRAM spans. It applies to immediate and HBlank/VBlank DMA.
Physical mirrors are resolved before testing overlap; mirror-crossing spans,
device memory, fixed/decrementing streams, sound FIFO and watched/shadow-gate
writes retain their existing service. There is no live implementation handoff.

Inside the covered service, one native bulk copy replaces the load/store
stream. It preserves output bytes, running source/destination, reload/repeat,
enable bit, unit/run counters, the existing operation-level DMA time debt and
completion IRQ. The original service already performs transfers atomically
before charging DMA debt, so batching introduces no new inter-word device
observation point. Active bus write observers and the DMA watchpoint keep
their original per-unit observations. Overlapping forward streams retain
their original propagation behavior instead of being changed to memmove.

The maintained LLE build executes the original per-unit virtual bus service.
Its operation floor is exercised by real GbaBus/GbaIo fixtures, independently
of BIOS boot. An absent generated BIOS remains a known unrelated engine-build
limitation; the placeholder BIOS is not presented as a validated BIOS floor.

## Checks and disposition

Both Release configurations build. Existing bus/DMA checks pass in both.
`dma_bulk_tests` verifies actual immediate/timed DMA bytes, alignment,
completion IRQ/enable/time debt, source continuation and destination reload,
overlap propagation, rejected mirror/MMIO spans and write-observer suppression.
The fixture explicitly wires the bus, as the real runtime does.

For a component probe, run `dma_bulk_tests --probe`. It executes 2,000,000 actual
GbaIo immediate transfers in the selected build: 1,500,000 of 64 bytes,
437,500 of 1 KiB and 62,500 of 64 KiB. It prints elapsed time and an exact
64-KiB destination digest after checking every output byte outside timing. Compare identical compiler settings and
separate LLE/HLE build trees serially. This is a bounded RAM-DMA service probe,
not a gameplay FPS measurement; eligible DMA frequency and end-to-end benefit
remain unknown. Contract tests do not replace gameplay progression/softlock
qualification. The pilot stays an experimental draft until those gates and
material performance are established on the declared platform.

The probe reports each size separately before its aggregate. These synthetic
size counts are a coverage corpus, not measured game transfer weights; the
aggregate bytes are dominated by 64-KiB copies and cannot establish a typical
title benefit. Balanced LLE/HLE run order is required for comparisons.

## Bounded Windows component screen (2026-10-06)

Native MinGW GCC 15.2 Release, same compiler options and source revision,
serial order LLE/HLE/HLE/LLE. Parent held team builds/timing for this window;
foreign machine activity was not controlled. No warmup, game, renderer or
firmware workload is implied. Per-size times below are seconds; HLE size
intervals below 100 ms are indicative, while aggregate intervals exceed
100 ms. Two balanced pairs are a screening result, not a confidence interval.

| Run | Build | 64 B | 1 KiB | 64 KiB | Aggregate |
|---|---|---:|---:|---:|---:|
| 0 | LLE | 0.246789 | 0.825285 | 7.169574 | 8.241700 |
| 1 | HLE | 0.052828 | 0.017398 | 0.046008 | 0.116279 |
| 2 | HLE | 0.045863 | 0.017902 | 0.045829 | 0.109637 |
| 3 | LLE | 0.220736 | 0.775789 | 7.955595 | 8.952167 |

Median aggregate LLE 8.596933 s, HLE 0.112958 s (76.11x service ratio).
Every run returned zero failures and digest `5932369013024162691` after
checking all 65,536 destination bytes. The shared memory fixture's generated
pattern is public synthetic data. Transfer weights are deliberately synthetic;
this large service ratio does not imply a whole-title speedup or authorize
default-on promotion. Keep the pilot experimental/default LLE.

Two later raw GBA captures were excluded: the orchestration initially advanced
after the shell session yielded at ten seconds, allowing a short overlap.
Only the first four fully serial captures above support this screen. The
control was corrected to await session completion before the NDS captures.

Screen executable SHA-256 identities (local Release artifacts):

- LLE: `15a8b6316afaa68e29a4b6c20ee3ecdcf97c0264e9063d53f198b73b83bee1eb`
- HLE: `2c76fb4e311fe0e1a2e506180c148b310beaa6fa512ff52677a3cf417288fd2f`

Raw stdout/stderr was retained in the workspace artifact directory as
`gba-probe-0-LLE.log`, `gba-probe-1-HLE.log`,
`gba-probe-2-HLE.log` and `gba-probe-3-LLE.log`.
The source test emits all rows and verdicts needed to reproduce the screen.
