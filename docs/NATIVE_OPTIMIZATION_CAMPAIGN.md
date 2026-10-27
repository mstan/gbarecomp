# GBA native optimization campaign

Owner goal: evaluate the opportunities below, keep improvements that are neutral
or positive in each tested game, and leave the final games open for visual/play
acceptance. The owner's positive acceptance then authorizes integration into
main/master, updated title pins and replacement builds. No promotion has occurred.

Owning issue: `beads-pc0o`, under `beads-mc7`, related to coordinating
`beads-2wy8.1` (codename **optimization campaign**), label
`campaign:optimization-campaign`. Work is solo.

Engine worktree: `G:/Temp/Codex/gbarecomp/native-services-20261026/engine`,
branch `perf/native-services-20261026`, original engine `92b26b2`.
Evidence and disposable build outputs live in its parent directory; original
checkouts, native saves and owner presentation work are preserved.

## Burndown

Evaluation means a recorded decision supported by source evidence and the
smallest useful measurement. It does not require implementing every hypothesis.
An unfavorable experiment is complete when its evidence and disposition are
recorded; neutral titles do not disqualify another title's win. Shared baseline
failures are recorded separately. Add concrete newly discovered opportunities
here rather than silently expanding an untracked tuning loop.

| ID | Opportunity | Current state / next decision |
| --- | --- | --- |
| G01 | Remove ordinary RAM bus classification/virtual access | Candidate implemented behind build-selected `GBARECOMP_MEMORY_IMPLEMENTATION=HLE`; LLE remains default. Mirror, byte-order, observer and read-override checks pass. Isolated game timing pending. |
| G02 | Direct immutable cartridge reads and folded address mapping | Inspect GPIO, EEPROM, Matrix mapping, overrides and open-bus exclusions; measure memory share before expanding G01. |
| G03 | Native dispatch lookup and call-boundary simplification | Existing immutable-table cache already covers ordinary lookup; avoid duplicating it. Evaluate remaining guarded dispatch and generated cross-function transfers. |
| G04 | Remove persistent interpreted/healed hot code | Latest MKSC/Emerald fixture has zero interpreted instructions/misses. Inspect MMZ gameplay and actual title configurations; distinguish first-run healing from steady-state execution. |
| G05 | Cheaper generated instructions / host register use | Evaluate generated bodies and built profiles, including whether O1 corpus optimization leaves large runtime costs. Longer generation/builds are acceptable. |
| G06 | Event-horizon execution and instruction bookkeeping | Lazy device catch-up already exists. Evaluate removal of repeated host work between observable IRQ/MMIO/DMA/yield boundaries; preserve required scheduling behavior. |
| G07 | Native wait/poll services | Fixed-point idle acceleration already exists; inspect remaining polls and HALT/IntrWait costs without fabricating completion. |
| G08 | BIOS whole-service HLE | Existing copies/decompression/math HLE and real generated BIOS are available, but MMZ/MKSC explicitly select `hle=false` and Emerald leaves it disabled. Evaluate matched service-HLE builds without conflating boot skip; qualify actual caller effects and timing policy. |
| G09 | Adaptive text background tile-row jobs | Candidate implemented in existing HLE compositor. 64 frozen-reference images match across four widths, map sizes, flips, scrolling, windows/effects and mapping providers. Built-game adaptive timing pending. |
| G10 | Winner-only composition / compact pixel jobs | First specialization preserved compared state/images/audio but did not establish a gain. Removed; raw experiment retained. Initial timings were host-load contaminated and are not proof of regression. Revisit only if attribution supports a different design. |
| G11 | Native OBJ preparation / row jobs | Inspect repeated OAM decode, affine work, window stencils and sprite ordering. Preserve slot identity, duplicate ordering, provider ownership and mid-frame changes. |
| G12 | Reusable tile/palette/map decode | Evaluate content-validated or immutable-snapshot caches. Direct native/mod writes must remain visible; no unproven dirty-bit assumptions. |
| G13 | Remove redundant canonical/presentation rendering | Inspect duplicated native rendering when enhanced/adaptive surfaces are enabled. Preserve canonical state and required readback; do not remove device timing. |
| G14 | Skip occluded high-resolution sampling | Candidate implemented in cached HLE sampler: resolve opaque HUD/OBJ/black blocks once, retain forced-alpha source work. Focused scale/filter/HUD/state tests pass; title measurement pending. |
| G15 | GPU affine sampling/filtering/composition | Larger HD architecture candidate. Evaluate existing host renderer integration and reusable GPU paths against CPU cost before choosing implementation. Portable supported fallback remains required. |
| G16 | Native presentation conversion/upload | Attribute grading, color LUT, texture upload, copies and present pacing separately from guest work. Existing MKSC evidence identifies material presentation cost. |
| G17 | Buffer-level PSG/direct-sound production | Audio already advances by sample events. Evaluate live-game share and native buffer services, retaining FIFO/IRQ/register/readback behavior and qualified sound. |
| G18 | Native game audio driver service | Source confirms m4a is detection/observation only and MP2K shadows retain original guest mixing plus verification. A real driver-service replacement could remove substantial work. Reuse detection and mixer knowledge; measure guest SoundMain share and qualify memory/channel completion effects before replacing it. |
| G19 | Broader DMA / bulk operations | Existing ordinary-RAM HLE pilot has no established whole-game gain. Measure eligible transfers; preserve overlapping propagation, observers, device destinations and completion ordering. |
| G20 | Session binding, serial scheduling and native slices | Inspect repeated binding/slice checks in representative local-link/rollback workloads. Keep single-player and netplay costs distinct. |
| G21 | Snapshot capture/hash/transactional restore | Recent archive work already reduced capture cost; ordinary execution gain was unproven. Reuse its evidence; investigate remaining restore work only for a demonstrated rollback cost, preserving malformed-load atomicity. |
| G22 | Default diagnostic/debug overhead | Historical default tracing removal is already integrated. Audit remaining normal-game costs; retain useful counters and opt-in diagnostics. |

## Game qualification

| Game | Frozen title head | Workloads / state |
| --- | --- | --- |
| Mega Man Zero USA | `a1568a7` | Build current title with recomp-ui/adaptive feature. Prepare an active gameplay route and current-build restore; not yet performance qualified. |
| Emerald USA | `ca1060e` | Existing title checkpoint is only a startup/title fixture. Add field/battle activity before gameplay claims; use the already validated wireless-capable UI dependency. |
| Mario Kart Super Circuit USA | `d551cfc` | Reuse existing race route/checkpoints; compare ordinary/adaptive racing, then matched 8x/16x enhanced work. |

Keep package labels separate: ordinary native RAM, adaptive tile jobs and HD
occlusion need not apply to every workload. Record exact binaries, compiler,
engine/shared-core/UI/title identities, ROM revision, settings, inputs, host,
CPU cost per useful frame, uncapped throughput and variability in the parent
`campaign-results.json`. Retain raw logs and useful images for eventual campaign
figures; no blog post is authorized.

## Testing and handoff

- Focused introduced-risk checks precede performance qualification. Current-build
  save/load/restore and relevant rollback checks matter; old snapshots need no
  migration. Native saves remain protected.
- Serialize performance work with the shared benchmark mutex and observe actual
  host load. Both local and NAS hosts are shared. If occupied, defer the next
  resource/test-slot check for 30 minutes and continue independent work.
- Initial local single pairs are retained as preliminary observations only:
  baseline CPU cost drifted materially and concurrent host load was high.
- Final builds keep working UI and selected presentation features. Unattended
  runs are muted. Owner checks use normal windows/audio and preserved notes.
- After owner acceptance, merge the qualified package, pin titles to the landed
  engine and verify replacement archives. Keep one current archive per game in
  delivery folders and clean disposable scratch after retaining evidence.
