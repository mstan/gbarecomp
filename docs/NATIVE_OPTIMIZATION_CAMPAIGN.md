# GBA native optimization campaign

Owning issue: `beads-pc0o`, under `beads-mc7`; coordinating campaign:
`beads-2wy8.1` (`campaign:optimization-campaign`). Work is solo in isolated
worktrees. Original checkouts, owner saves and settings are preserved.

The owner requested a broad opportunity burndown, evaluation of every entry,
and final game builds for a normal-window, normal-audio look/play check. Positive
owner acceptance authorizes main/master integration, updated title pins and
verified replacement archives. No promotion or owner-facing launch has occurred.

## Current sources and evidence

Authoritative qualification root: `C:/Temp/Codex/gba-current-20261027`.
Historical experiments under
`G:/Temp/Codex/gbarecomp/native-services-20261026` qualify only their exact older
sources; they are not current-main performance claims.

The reference algorithms are engine `37c75aa`, merging local `92b26b2` and frozen
upstream `e353848`. Both arms retain prior renderer/audio/capture improvements
and use shared ARM core `451580e`. Candidate measured code is `4e18cbc`; subsequent
BIOS build-hint/documentation changes do not change those frozen binaries. The
real-GPU reference `72b5534` adds only the matched hidden-window hook to `37c75aa`.

Every arm uses the same audited, generated real LLE BIOS. Generation requires
both `--bios bios/gba_bios.bin` and `--config bios/gba_bios.toml`, with `--out`
selecting the build input directory. Actual selected corpus content/availability
is included in the candidate netplay compatibility identity. Snapshot version5
combines timing and SWI-ledger state; old snapshots are disposable and rejected.

Raw binary/source/input/toolchain/settings/host identities, samples, logs, states
and images are retained in `frozen/`, `frozen-gpu/`, `fixtures/`, `checks/`,
`results-practical-1/` and `nas-mmz-current-1/`. Machine-readable summaries are
`opportunities.json`, `campaign-results.json` and `qualified-performance.json`.
Do not publish a blog post without an explicit request.

## Opportunity burndown

Evaluation means a supported disposition, including rejected experiments and
architectural follow-ups. It does not mean every hypothesis was implemented.
Bundle measurements cannot establish an individual component's speedup.

| ID | Opportunity | Disposition |
| --- | --- | --- |
| G01 | Native ordinary RAM access | implemented; performance qualification open |
| G02 | Direct cartridge mapping | evaluated; no additional implementation selected |
| G03 | Dispatch lookup | evaluated; current upstream optimization reused |
| G04 | Persistent interpreted hot code | evaluated; no missing-code payload selected |
| G05 | Generated code optimization and cross-module calls | O2 implemented; current-source performance qualification open |
| G06 | Native event-horizon execution | evaluated; prototype excluded from current candidate |
| G07 | Native wait/poll services | evaluated; existing optimization retained |
| G08 | BIOS whole-service HLE | evaluated; existing BIOS HLE pilot not selected |
| G09 | Adaptive text tile-row jobs | implemented; current built-game performance qualification open |
| G10 | Winner-only composition | evaluated; unsuccessful prototype removed |
| G11 | OBJ decode and row preparation | evaluated; persistent cache not selected |
| G12 | Reusable tile/palette/map decode | evaluated; existing caches retained |
| G13 | Redundant canonical/presentation rendering | evaluated; required split retained |
| G14 | Occluded HD affine sampling | implemented; isolated correctness passed; timing open |
| G15 | GPU affine rendering | evaluated; larger architecture deferred |
| G16 | Native presentation conversion/upload | implemented; current GPU correctness passed, timing open |
| G17 | Buffer-level device audio | evaluated; existing batching retained |
| G18 | Native SoundMain / MP2K service | evaluated; substantial follow-up identified, not implemented |
| G19 | Broader DMA/bulk services | evaluated; existing RAM pilot remains experimental |
| G20 | Native local-link slices and binding | evaluated; existing native execution retained |
| G21 | Snapshots/hashing/restore | evaluated; prior wins retained and current snapshot contract checked |
| G22 | Default diagnostic overhead | evaluated; preserve matched diagnostic contract |

Native ordinary RAM retains boundary/mirror/observer/read-override fallbacks.
The portable implementation uses byte assembly, with no retained backing pointer.
Text-row jobs and precise title masks apply to MMZ/MKSC; Emerald provider masks
are unchanged. HD occlusion is exercised by MKSC8x, with no HD credit at1x.
Native upload fuses conversion/grading/packing for supported ARGB/ABGR textures,
retaining RGB24 fallback. Additional staging is4bytes/upload texel; the conditional
prepared graded LUT is128KiB. BIOS service HLE, the event-window prototype and IPO
are excluded from the current selected candidate. Native SoundMain remains a
larger follow-up, not an implemented or measured gain.

## Game qualification

The whole candidate package is generatedO2/runtimeO3, ordinary RAM HLE, existing
HLE text/HD renderer with the new spans/occlusion work, and native presentation.
Reference generated code isO1/runtimeO3. IPO is off; real BIOS LLE and scheduler
behavior are common. Default promotion remains pending final qualification.

| Game | Current correctness and feature evidence |
| --- | --- |
| Mega Man Zero USA | Active first-mission fixture, native240/wide288/max480 complete states and PNGs exact; current-build save/load; actual hidden D3D11; adaptive Fit live resize including portrait. Windowed tests use explicit controller replay, not the headless-only demo input. |
| Mario Kart Super Circuit USA | Active Peach Circuit race, native240/wide382 and8x3056x1280 complete states and PNGs exact; current-build save/load at8x; matched actual hidden D3D11. |
| Emerald USA | Native240/wide284 field and real PoochyenaLv2 battle, complete states/PNGs and current-build save/load exact; actual hidden D3D11; widescreen0.2.0 Fit resize. Battle keeps a centered native240x160 picture with black margins in larger canvases. |

All representative continuations report zero dispatch misses/interpreted
instructions. Framework focused tests8/8, native multiplayer/link fixtures2/2,
selected-BIOS identity checks, portable SDL-free color/upload checks, and actual
D3D11 RGB24-versus-packed pixel readback pass. These are not full real-network
netplay or all-game-path claims. Prepared stripped owner profiles passed hidden,
muted P pause/resume and F1 restore; Emerald F2 battle/F1 field also passed.

## Current performance findings

NAS host: Intel Core i5-4670, Windows10, fixed single-logical affinity. All outputs
in each completed case match exactly. Results below are for the full labelled
package and SDL dummy/software presentation, not isolated optimizations or GPU
throughput.

| Workload | Reference median CPU | Candidate median CPU | Disposition |
| --- | --- | --- |
| MMZ native240 |6.6796875s |6.53125s | Neutral/inconclusive; mixed pairs |
| MMZ wide288 |9.640625s |9.1484375s | Positive:5.1% lower process CPU; AA0.96%, pair spread1.16pp |
| MKSC wide382,8x |22.5s |20.8125s | Positive:7.5% lower process CPU; AA0.35%, pair spread2.15pp |
| MKSC wide382,1x |10.703125s |9.7421875s | Inconclusive; pair spread4.99pp |
| MKSC native240,1x | — | — | Short-run AA failed; longer active replay pending |
| Emerald field/wide/battle | — | — | Pending |

Bounded elapsed-runtime medians improve in the positive-CPU cases, but their
controls/pairs are noisy: no throughput win is claimed. No reproducible CPU or
bounded-runtime regression appears in completed comparisons. Emerald and actual
GPU timing remain pending. Earlier failed-control attempts are retained.

## Measurement and remaining handoff

- Per game/workload/host, neutral or positive is acceptable. A reproducible
  individual regression blocks adoption; another title's win cannot offset it.
  Shared original/candidate failures are pre-existing defects.
- Hold the shared named mutex and exclusive file handle; preserve nonempty
  reservations. Hosts are shared; retry an occupied slot at30-minute intervals.
- The initial absolute10% idle-core preflight caused unnecessary waiting. A
  separately hashed practical wrapper selects the least-loaded physical core
  up to50% sibling load, records original eligibility and both primed1-second PDH
  windows, and pins one logical CPU. Ordinary builds may share spare capacity;
  benchmark/test/capture exclusion and reservations remain. Logical CPU usage
  is compared with logical capacity on SMT hosts, reserving another physical
  core's capacity. This is a preflight correction, not changed result acceptance.
- Warm both arms, collect A/A controls, then balanced ABBA/BAAB samples. AA<=1%
  and all pairs beyond max(2%,AA+1pp), spread<=3pp, are required for a CPU win.
  Preserve incomplete/noisy cases; never cherry-pick samples. Inspect bounded
  seconds per useful guest frame separately; a reproducible regression blocks
  adoption. Warmed wall-minus-bounded cost includes startup/dump/shutdown and
  is not a cold-start metric. Actual GPU cases retain a separate GPU-load gate.
- Finish pending per-game measurements, select defaults and rebuild exact final
  candidates. Then leave all3 normal windows/audio with private saves, prepared
  states and a lightweight recorded checklist. Read owner notes before deciding
  acceptance. Do not repeat a cleared title's handoff.
- Only after owner acceptance, integrate qualified engine/title changes, pin the
  titles to the landed engine, and verify one replacement archive per game.
  Preserve active owner profiles, native saves and evidence; clean owned build
  scratch/caches and historical duplicates outside delivery folders.
