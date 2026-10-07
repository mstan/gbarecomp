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

This strategy completes one winning shared-engine candidate end to end. The
three-title pool supplies comparisons, not a requirement to build or time every
possible route/setting. Use one declared production configuration per title and
reuse valid route evidence before spending the bounded capture budget.

| Title and concrete useful-work route | Production floor gap / next milestone | Candidate boundary and observable checks |
|---|---|---|
| Mega Man Zero (primary): restore Sept26 `ready.state`, replay the already exercised movement/shooting window with Ciel for 1,200 guest frames, recording start/end frames and scene. | Existing production binary/source identity and successful active capture are available. Select a coherent maintained framework/title pin for a matched LLE/HLE production build; the old binary is discovery evidence, not the new candidate baseline. | Scanline output/publication and scheduler tick/yield work: completed frames, gameplay movement/shots, IRQ/event delivery, input response, audio progression and continued combat/script progression. |
| Mario Kart Super Circuit (companion): active race start/countdown through a short driving segment with opponents, advancing a fixed guest-frame interval rather than menus/attract mode. | Aug20 boot smoke is not a ready measured race floor. Identify a functioning production runner, title pin, isolated save and reproducible race checkpoint/input before timing. | Test shared renderer/event service during racing: countdown/order, motion/opponents, track/sprite output, steering/input, audio and frame/event completion. |
| Emerald (companion): restored overworld walking followed by entering and advancing one battle/menu transition. | Existing older executable/boot log does not establish active-route readiness. Qualify a coherent production pin and isolated checkpoint before measurement. | Tile/sprite rendering and event delivery across overworld/battle: movement, transition completion, menu input, audio, save persistence and continued progression. |

First implementation theory: batch or replace a coarse shared scanline-render
service, or reduce repeated scheduler tick/yield service across a declared
caller boundary. The active MMZ sample points to approximately 19% scanline
renderer and 15% tick/yield self samples; it does not identify the winning
algorithm or an additive speedup ceiling. Measure eligible work on the production
routes, including worker CPU, before deciding. Choose the boundary that has a
clear maintained LLE floor, substantial repeated eligible cost, and a caller
contract permitting materially less total work. This must be shared engine
service code, not a hardcoded MMZ address/routine intercept. The existing RAM-DMA
pilot is selected only if actual-game eligible coverage and end-to-end cost make
it competitive; its large synthetic service ratio alone is insufficient.

Before code, declare the candidate's supported service family, intended material
gain and acceptable tiny output/timing approximations. A provisional planning
target is roughly 10% reduction in total useful-work cost on the primary route,
not universal acceptance: the measured gain must exceed observed noise and retain
useful guest-frame/event progress. Profile diagnostics separately from matched
uninstrumented LLE/HLE measurements. The primary comparison is ABBA (two balanced
pairs); each ready companion gets one A/B pair. Do not automatically repeat runs
or expand configurations when evidence is inconclusive; document the next
specific question and retain an unsuccessful experiment as draft.

Caller contracts cover frame publication/buffer ownership and memory visibility,
pending-event/IRQ delivery and completion order for the replaced operation.
Compare exact pixels or event/output counters only where exactness is claimed;
permitted minute visual/timing differences do not require full internal-state
identity. Automated production checks must cover boot, active play/progression,
pause/resume, reset, isolated save/load/persistence, audio and input at affected
boundaries. Test both build selections, and preserve a functioning LLE build.

Final playable handoff: a production **Mega Man Zero** HLE package from the
winning coherent title/framework pins, with its LLE build alternative, isolated
save/checkpoint and instructions for the same Ciel movement/shooting segment plus
normal continued play. Hand it to the owner only after automated checks and a
material useful-work gain pass; report measured scope, known approximations and
companion results. The owner performs the final feel/progression playtest.
Merge, enable by default only on the demonstrated platform/workload scope with
build-time LLE opt-out, and close the issue only after that feedback passes.

## Measurement, decision and delivery protocol

Owner completion rule: establish a material game-workload gain and automated
compatibility, then deliver the final playable build for the owner's feel check.
After that check passes, integrate the prepared default change and close the
scoped work. Exhaustive game coverage and completed campaigns are not additional
completion requirements.

1. **Pin the workload and floor.** Use the three games and concrete routes above.
   Build LLE and HLE from the same title/framework revisions, compiler/options,
   ROM/firmware identities, presentation/audio settings and initial game state;
   only the selected implementation differs. Keep the replaced LLE service
   runnable. An old executable is discovery evidence, not a mismatched control.
   Use native game saves or replayed inputs when private savestates cannot cross
   builds. First resolve the named route/build gaps; do not perfect unrelated
   hardware before replacing a functioning operation.
   Verify that companion routes actually exercise the replacement; an unaffected
   title is a regression control, not evidence for that HLE service. If the
   chosen service changes, replace an unsuitable companion in the three-title
   set instead of accumulating extra games or claiming unexercised coverage.
2. **Attribute only what is missing.** Reuse suitable profiles and collect at
   most one new active-workload attribution capture per selected game in this
   implementation round. Identify the intended service's eligible dynamic work.
   Include worker threads and external modules or report them unresolved; a
   main-thread symbol histogram cannot supply a whole-process cost percentage.
   Capture diagnostics separately from performance. End discovery when there
   is enough evidence to select a useful service, not when every subsystem has
   a profile. The earlier six-launch discovery cap applied to that completed
   pass, not to the whole implementation/qualification program.
3. **Choose one replacement.** Record its caller ABI, inputs, outputs, observable
   side effects, supported operation scope, permitted tiny differences, expected
   cost removed, and candidate-specific useful gain before coding. Implement a
   shared service with build-time LLE/HLE selection and explicit build identity.
   Do not stack several speculative replacements into the same comparison.
4. **Measure equivalent active play.** Delimit a fixed gameplay window by guest
   frames and meaningful game events, excluding boot, warmup and teardown.
   Choose enough active work to dominate measurement granularity once, then keep
   it fixed. Report total process CPU milliseconds per guest frame (all threads),
   critical-path frame work, median/p95 frame time and missed presentation/audio
   deadlines where available. Record peak memory and code size, since constrained
   targets matter. Preserve normal renderer and audio production; a benchmark
   that omits presentation/audio is a core-only diagnostic, not end-to-end proof.
   The owner selected Windows first and authorized uncapping for useful
   measurements. Prefer a finite uncapped comparison where it preserves the
   same game, render and audio-synthesis work. Remove host frame-delay/VSync
   waits only in isolated benchmark configuration; do not change the guest
   timing model, resolution, effects, audio workload or HLE coverage between
   builds. Report uncapped FPS and milliseconds/frame alongside total CPU/frame,
   and verify completed render/audio work and game progress rather than trusting
   a frame counter alone. A legacy benchmark that skips rendering/presentation
   or audio remains core-only evidence; use a complete paced CPU/frame comparison
   until that benchmark path can exercise equivalent work. Normal capped play
   can show reduced CPU/frame even when FPS stays unchanged. Measure GPU
   completion/queue cost when work moves there; a shorter submission call alone
   is not a win. Keep the final owner-playtest package normally paced.
5. **Use a fixed comparison budget.** The primary game gets LLE/HLE/HLE/LLE:
   two order-balanced pairs, four measured executions. Each of the two companion
   games gets one LLE/HLE pair, two executions each. That is eight measured runs
   per candidate on one declared host/configuration, not a Cartesian matrix.
   Reuse their progression telemetry and final outputs; take expensive milestone
   captures outside timing, and use isolated LLE/HLE fixtures for detailed
   contracts. Do not automatically add separate full campaigns or trace runs.
   Keep team builds/profiling out of the timed window, record host load/power/
   thermal conditions, and preserve every result. A noisy or contradictory result
   stops that screen; fix an identified condition before a bounded replacement
   measurement. Never repeat until a passing subset appears.
6. **Decide from useful gain and compatibility.** Report both paired percentage
   and absolute savings, with the observed pair spread. About 10% lower whole
   active-workload CPU time is a planning aim, not a universal acceptance rule.
   A candidate may instead solve a declared frame-budget or stutter problem.
   Both primary pairs must show a clear consistent useful improvement beyond
   observed noise; two pairs are not a formal confidence interval. Companion
   single pairs screen for large regressions, not proof of zero performance
   change. Explain any apparent regression before broadening defaults. Exact
   promises require exact outputs; permitted approximations use a declared
   practical image/audio/result comparison. Check input, audio, progression,
   affected completion/IRQ consumers, transitions and relevant pause/reset/save
   behavior. No crash, softlock, stale buffer, lost completion or save corruption
   passes. A huge isolated kernel ratio cannot substitute for this decision.
7. **Hand off the actual finished candidate.** Provide the named primary game as
   a ready-to-launch normal-paced HLE package, an LLE comparison build, isolated
   save/checkpoint setup, launch instructions and checksums/build identity. Include
   a short before/after report, companion results and any tiny known differences.
   Prepare the intended default-selection/integration change in the draft PR so
   the owner tests the package intended to ship. Ask the owner to play normally
   and assess response, motion/collision, camera/scrolling, stereo where relevant,
   audio rhythm and continued progression. There is no prescribed full-campaign
   completion or multi-game human test matrix. Owner rejection reopens the
   affected behavior; fix and recheck that change before another handoff.
8. **Finish the scoped delivery.** After owner acceptance, integrate the reviewed
   candidate, make HLE the default for the supported titles/platform/service,
   retain a documented build-time LLE opt-out, and record the measured and manual
   evidence before closing the issue. Do not add unrelated qualification gates
   after the agreed playtest. If the replacement cannot deliver material gain,
   preserve its branch and draft PR with results, explain why, and choose a new
   boundary deliberately; an unsuccessful experiment is not a completed system.

The owner selected **Windows first; port measurements later**. Windows x64 is
therefore the initial implementation, measurement, final-playtest and default
scope. After automated checks, material gain and the owner's normal-paced feel
approval, finish that Windows delivery; a mobile/Xbox port is not a new gate
before closure. Later port work carries the winning candidate and relevant
routes to the chosen target and measures there before claiming target savings.
Do not multiply all hosts into the Windows discovery/comparison matrix.
