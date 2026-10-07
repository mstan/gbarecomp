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

## Representative workload discovery (2026-10-06)

The bounded discovery pool contains three distinct GBA workloads. It is a
selection pool, not an automatic title/configuration matrix. The shared pass
allows at most six fresh captures across all six systems, one configuration
per selected workload; reuse suitable existing evidence first.

| Workload | Reusable evidence and artifact age | Attribution and next decision |
|---|---|---|
| Mega Man Zero, 2D action | `MegaManZeroRecomp/build-pin-final/playtest/smoke-result.json`; Sept26 Release GCC/SDL2 binary, framework `7c9a1eba611b4eef7668937707d5f9abb0d94d9b`. Fresh bounded capture below. | Shared scanline rendering and event/tick batching deserve investigation before assuming RAM DMA is the largest engine boundary. |
| Mario Kart Super Circuit, racing | `gbarecomp/_ab_results/mksc.log`, Aug20 actual-title smoke with real recompiled BIOS. | Boot/idle evidence has no elapsed cost attribution. A coherent production artifact and active race route would be needed before ranking renderer versus CPU/device costs. |
| Emerald, RPG | `gbarecomp/_ab_results/emerald.log`, Aug21 actual-title smoke; retained production executable is older. | Idle elision counts are not host time. Useful genre diversity if the remaining capture budget warrants it; artifact identity and active route remain unqualified. |

Parent executed one fresh MMZ production capture using the Sept26 binary:
SHA-256 `fdcfb8df524a404c35203333a4774055e6e695d4d01a8108a047c2c086e39d78`.
It loaded the corresponding `ready.state` at actual frame 119727 and advanced
1,200 frames to 120927 with input replay. The final gameplay image shows
movement/shots with Ciel. Startup/exit telemetry reports static recompiled
execution, zero dispatch misses/interpreted instructions, and zero unmapped
memory/IO accesses. This establishes an exercised gameplay window, not campaign
completion or qualification of the new DMA HLE build.

Private workspace artifacts are `mmz-profile/samples-ready.csv`,
`mmz-profile/attribution.json`, `mmz-profile/samples-ready.csv.child.log`,
`mmz-profile/inputs.csv` and `mmz-profile/final.png`. The initial `samples.csv`
attempt rejected unsupported `--screenshot` and exited 1 before gameplay;
retain it as failed setup evidence and exclude it from attribution. The
corrected run used `--dump-png`, returned zero sample errors and child exit 0.

Of 172 main-thread self samples, 23 (13.372%) were outside the executable.
Two scanline-renderer symbols account for 25 + 8 = 33 (19.186%) samples;
`runtime_tick` contributes 16 (9.302%) and `runtime_should_yield` 10 (5.814%).
Thus approximately 19% renderer and 15% tick/yield are directional shared-engine
signals. The small coarse sample is not a benchmark or speedup ceiling.
Headless execution excludes presentation and the sampler does not cover worker
threads. Eligible RAM-DMA share remains unknown. Renderer/event batching are
credible next theories; the DMA pilot remains draft/default LLE pending its
own workload benefit and gameplay gates.

## Production candidate milestones and owner handoff

Windows is the first qualification platform. Complete one shared-engine
candidate using focused correctness checks, a measured FPS/percentage gain,
one basic visual sanity look, then the owner's playtest of all three selected
games. Ports follow after the Windows result; this is not a configuration matrix.

### Selected games and production readiness

| Game | Useful-work route | Build readiness |
|---|---|---|
| Mega Man Zero | Active movement/shooting with Ciel, using the already exercised 1,200-frame window as a starting point; normal continued play for the owner. | Sept26 binary/profile is discovery evidence. Build a coherent maintained title/framework LLE/HLE pair before candidate timing or owner handoff. |
| Mario Kart Super Circuit | Race countdown and active driving with opponents. | Aug20 boot smoke is insufficient for a new implementation handoff. Establish the maintained production title pin and functioning race route. |
| Emerald | Active overworld movement and a battle/menu transition. | Older executable/boot evidence is discovery only. Establish the maintained production title pin and functioning active route. |

These are three selected games, not three launchable candidate packages yet.
Old unrelated binaries must not be presented as the new implementation.
Use one declared Windows production configuration per game. Reuse valid
profiles and existing focused tests; do not require checkpoint, screenshot,
audio or internal-state sweeps before the owner can play.

### First shared-engine boundary

Investigate coarse shared scanline rendering or scheduler tick/yield service.
The active MMZ discovery sample indicates approximately 19% scanline renderer
and 15% tick/yield self samples, with a small sample and incomplete thread
coverage. These are directional signals, not additive speedup ceilings.
Select the boundary whose actual game workload shows substantial eligible cost
and whose caller ABI permits materially less work. Implement shared service
code, not hardcoded MMZ addresses or title routine interception.

The existing RAM-DMA pilot is a possible winner only when actual game coverage
and FPS/percentage benefit make it meaningful. Its synthetic service ratio
does not by itself outrank rendering/event work.

### Focused correctness and gain

Before code, declare the operation scope, caller ABI, maintained LLE alternative
and intended material gain. Keep caller-visible outputs and completion behavior
compatible; permitted tiny visual/timing approximation does not require matching
all internal state or old pixels. For rendering/event work, identify buffer
publication/ownership and affected event/IRQ completion as the concrete contract.

Reuse focused correctness tests for that boundary. Add a test only for a
concrete uncovered risk introduced by the candidate. General boot/lifecycle,
campaign, save, audio and image automation is not a mandatory acceptance sweep.
A detected defect can justify its own targeted check.

Start with one matched LLE/HLE timing pair per selected game. Use the same
production configuration and comparable active gameplay work; uncapping is
authorized for measurement. Report FPS and percentage gain with enough context
to distinguish useful-work improvement from faster guest-clock advancement.
Keep diagnostic profiling separate from timing. Add a reverse-order pair only
when observed noise or ambiguity would change the decision; do not allocate
an automatic eight-run quota or expand into a matrix.

Declare materiality before implementation. Roughly 10% remains a provisional
planning target, not a universal acceptance threshold. A convincing gain must
outweigh observed noise. If benefit or correctness is unresolved, keep the
experiment draft and identify the specific remaining question.

### Visual sanity and owner playtest

Make one representative visual sanity look at the new implementation: frames
must not be visibly garbled. No corresponding old frame or pixel-perfect
comparison is required. Deeper visual investigation is triggered only by a
noticed or reported defect. Rely heavily on the owner for gameplay feel.

Once the candidate's focused checks and material gain pass and all selected
production packages are ready, launch the new HLE **Mega Man Zero, Mario Kart
Super Circuit and Emerald** builds for the owner. Use normal pacing for human
play, isolated test saves where needed, and clearly identify the candidate.
Launch is already authorized; do not ask permission again. Ask whether each
game looks and plays right, and address specific feedback before promotion.

Positive owner feedback plus demonstrated material gain supports Windows
default-on promotion, retaining build-time LLE opt-out, followed by merge and
issue closure. Report actual title/platform scope and any known approximation.
Other ports are subsequent work, not prerequisites for this Windows handoff.
