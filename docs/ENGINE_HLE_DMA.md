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

## Active scanline pilot (implementation preparation)

`GBARECOMP_SCANLINE_IMPLEMENTATION=LLE|HLE` selects the shared native-width
scanline service at build time; the default remains LLE. DMA remains LLE in
matched scanline builds. HLE decodes regular-background tile spans (up to eight
pixels) rather than repeating map/flip/address work per pixel, resolves row
window controls once, and keeps composition candidates in native 555 color until
final RGB publication. It retains no cross-row cache, changes no device schedule
and preserves the same scanline entry point, memory snapshot and RGB destination.
Affine/bitmap/sprite operations still participate in native-color composition;
the extended-view renderer is unchanged. This is a shared engine replacement,
not a title address hook.

The structural opportunity is up to seven of eight repeated tile-map decodes
plus RGB expansion of occluded candidates. MMZ's 25 regular-background and eight
other renderer self samples motivate the boundary; they do not predict a speedup.
The declared planning goal is a useful roughly 10% primary-route total-work gain,
with acceptance based on actual FPS/percentage improvement above noise and the
owner's judgment. No performance or gameplay result exists for this implementation
yet. Existing PPU tests pass in both selections, including color effects,
windows/OBJ, foreign composition and non-tile-aligned scroll; no speculative
additional suite was introduced.

Matched Windows production MMZ preparation uses isolated title commit `84449cf`,
the maintained experimental framework, identical local guest-generated sources
and real BIOS-generated sources, GCC Release/SDL2 and DMA LLE. Root source edits
and player saves are preserved. Timed/gameplay/visual runs await the shared serial
window. Production companion readiness and owner handoff remain pending.

### Production floor qualification (2026-10-06)

The first SDL LLE route accepted the September gameplay state but stopped on
an undefined interpreter bridge; no completed timing row existed. HLE was not
run. That attempt mixed current framework sources with an unproven copied guest
corpus and is excluded from qualification. Its logs remain in the local evidence.

A separate repair uses the title's exact framework `a263ff2` and ARM core
`763b922`, with fresh ROM and real BIOS generation by that pinned tool. Both
SDL selections build and existing PPU smoke tests pass. The repaired LLE state
route nevertheless stops at RAM dispatch `0x0202407C`/undefined `0x020251CC`.
One subsequent cold-boot LLE setup, without any old state and with the existing
campaign-safe input sequence replayed through SDL, stops at ROM dispatch
`0x08294740`/undefined `0x08295174` before active gameplay. This disproves a
stale-state-only explanation. The ROM target is adjacent to the documented
stage descriptor `0x08294744`; it must not be admitted as code without tracing
the actual caller/control-flow contract.

Neither failed route completed its bounded window or produced a qualifying
visual result. Exit success alone is insufficient. No HLE gameplay, whole-game
FPS gain, owner handoff or default promotion is claimed. Remaining work is a
functioning coherent cold-boot production floor, then the bounded native LLE/HLE
comparison; the renderer candidate itself remains draft and default LLE.
The subsequent MKSC fallback exposed a concrete preparation error: the fresh
BIOS generation command omitted the maintained `bios/gba_bios.toml`. Bare BIOS
vector discovery omitted ARM entry `0x00000300` and exception resume coverage;
the first strict-static cold-boot setup correctly rejected that dispatch. The
config supplies verified BIOS roots and `static_resume_all=true`. Regeneration
with that exact pinned config emits 770 functions and 4,373 interior resume
aliases, then both SDL arms relink successfully. No BIOS stub or speculative
code seed was added. This omission also invalidates treating the prior MMZ
failures as proven underlying title defects; those attempts retain their raw
findings, but their BIOS floor was incomplete.

MKSC's isolated title `79dffea` uses exact framework `e3c834d` and ARM core
`c626f4e`, retaining desktop networking and its imported symbol overlay. The
existing gameplay fixture selects GP, 50cc, a driver and Mushroom Cup, then
accelerates/steers from frame 8,600. One repaired SDL cold-boot setup is prepared
to save a fresh race state after frame 10,000 via the existing observer API and
inspect the final image at frame 18,000. Any matched 1,200-frame active-race pair
uses that same state with observer disabled, ordinary render/audio/presentation
work and uncapped pacing. Setup captures are not timing evidence; a valid race
and completed window are required before HLE runs. Results remain pending.
### First functioning full-game result: MKSC native race

The BIOS-fixed cold boot completed 18,000 presents, guest frames 0 to 18,002,
with zero static dispatch misses/interpreted instructions and a clear actual
Luigi/Mushroom Cup race image. A fresh state was saved at frame 10,016. One
observer-OFF LLE/HLE pair then completed the identical active window: frames
10,016 to 11,215, 1,200 presents, 321,101 steps, 2,116,299,579 cycles, final PC
`0x0806134a` and VCOUNT 7. Both report zero dispatch misses, interpreted/healed
instructions, unmapped accesses and unhandled I/O.

Loop-only LLE time was 2.503480 s (479.333 present FPS); HLE was 2.194235 s
(546.888 FPS): 14.09% more FPS and 12.35% less loop time. Ordinary SDL rendering,
audio drain/mix/push and every presentation remain enabled; only pacing is
uncapped. Audio counts were not separately instrumented. Startup/state load and
cleanup/save/PNG are outside the loop timer. One HLE image was inspected and is
clear actual racing, with no paired pixel comparison. A foreign PSX parallel
build was active and recorded; this is one route/pair, not an isolated-host,
all-title or port result. No repeat was necessary to resolve a defect or
ambiguity. Owner play is pending; adaptive wide rendering is unchanged and the
candidate remains draft/default LLE.

Local reproducibility evidence is `mksc-titlepin-render-run-manifest.json`,
`mksc-titlepin-active-pair-result.json`, per-arm stdout/stderr and final PNG,
`mksc-titlepin-coldboot-biosfixed/setup-result.json`, and the host contention
census. Raw private ROM/state/generated assets are excluded from commits.
Adaptive extension now applies the same deferred native-color composition to
the shared wide renderer, preserving every margin/remap/provider lookup. Wide
window controls were already row-resolved; arbitrary remapped background samples
do not use native tile spans. Existing relevant wide-center, bitmap-margin,
affine-filter and foreign-overlay PPU checks pass in both selections. Selection
is scoped to the PPU source and runtime banner, so unchanged guest objects can
be compiled once and reused for the second arm.

Upstream MKSC main advanced to `b89fbb4` (v0.1.4), incorporating save, checkpoint
and speedometer PRs; no open origin PRs were present at inspection. Current
integration therefore retains its framework `29efc028` and core `c626f4e` in a
new isolated worktree rather than downgrading the title's dependency. Fresh
current guest generation preserves its additional Time Trial roots. Both
current SDL selections build and PPU checks pass. The historical native pair
is not relabeled as current. One current adaptive-320 active pair is prepared,
with a declared roughly 10% total-loop reduction goal above noise, ordinary
audio/presents, and one HLE image/actual-width confirmation. Owner play and
promotion remain pending; title integration will be a separate draft PR.
## Current real-adaptive result (2026-10-07)

One corrected current-pin pair used real SDL borderless desktop 3440x1440 and
actual logical width 382x160, not a forced drawable. Both arms completed the
same active frames 10016 to 11215, 1200 presents, 321101 steps and 2116299579
cycles, final PC `0x0806134a`, with zero dispatch misses/interpreted/healed,
unmapped or unhandled-I/O counts. LLE loop time was 3.782089 s (317.285 FPS);
HLE was 3.538319 s (339.144 FPS): 6.889% more FPS and 6.445% less loop time.
This is a modest gain below the roughly 10% planning goal, not a target-met or
automatic-default claim. No repeat was made. Ordinary render/audio/present work
was retained, observer OFF, uncapped pacing only. Previously active foreign
PSX build contention is disclosed; no isolated-host inference is made.

One HLE extended race image is clear: kart, road/scenery and HUD render normally.
Normal-paced real-adaptive HLE and the LLE alternative are staged with exact
binary hashes/arguments in the private review manifest. Owner value/feel decision
is pending, and title PR 10 remains draft/default LLE as requested. No game is
launched while the owner is away. The historical native 14.09% FPS result stays
separate; the earlier wrong-width arms are setup evidence only.
## MMZ corrected active adaptive result (2026-10-07)

The maintained BIOS configuration repairs the previously rejected floor without
stubbing dispatch or adding guessed seeds. Exact title `84449cf`, framework
`a263ff2`, core `763b922` and matching freshly generated ROM/BIOS corpus cold
boot completed 12000 presents with zero static misses/interpreted/healed and
unmapped/unhandled-I/O counts. An exact existing `campaign_clear_key` extension
then progressed beyond dialogue and saved a fresh active checkpoint at 16016.
One new scene image shows Zero/buster/healthbar/Ciel in the actual industrial
level. Emerald's planned third sample is deferred by the owner's lightweight
scope; it is not a missing promotion gate.

One real adaptive borderless pair completed frames 16016 to 17215, 1200 presents,
actual 382x160 on real 3440x1440. Both report 131310 steps, cycles 62191750, final PC
`0x080c88ee`, VCOUNT 25 and identical idle-elision counts with zero misses or
interpreter/heal/unhandled-I/O activity. LLE loop was 3.158846 s (379.886 FPS),
HLE 2.971153 s (403.884 FPS): +6.317% FPS/-5.942% loop time. This is modest and below
the planning target, with no automatic repeat/default/merge. A single HLE image
is clear; the existing provider intentionally pillarboxes unsupported margins,
so this does not claim expanded authored world content. Normal SDL/audio/present
work is retained, observer OFF, uncapped pacing only, no forced drawable. Exact
normal-paced adaptive HLE/LLE review hashes and launch arguments are staged
privately; owner value/feel decision remains pending. No more GBA runs/builds.