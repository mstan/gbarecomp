# Accuracy-preserving host performance investigation

Central tracker: `beads-vm0x` under `beads-mc7` / `beads-5ae`.
Original reference: engine `7a05e7610f32ce0587c7f2a97f80b695c8c9e8c3`.
Investigation branch: `perf/accuracy-host-20261022`. Work is solo.
Evidence root: `G:/Temp/Codex/gbarecomp/accuracy-perf-20261022/`.

This is an investigation and qualification record, not integration approval.
The owner explicitly requires approval after automated qualification and before
final integration. No old save-state compatibility is required. Native cartridge
saves are read-only inputs; speculative saves stay inside isolated test sessions.

## Task burndown

| Task | Status | Evidence / completion condition |
| --- | --- | --- |
| Establish repository, instructions and dirty-file boundaries | Passed | Parent is a repository collection; engine main is dirty only in `oracle/gba_tcp.py` plus an untracked mGBA archive. No on-disk AGENTS.md found at the applicable ancestors or scoped engine paths. Owner-supplied instructions apply. Isolated worktree on G:; no edits in other worktrees. |
| Establish storage and benchmark isolation | Waiting on other windows | Initial F: queue 4 / 11.8 MB/s; G: queue 0 / 0.3 MB/s. Scratch, builds, profiles and caches on G:. Owner explicitly chose to wait for current workloads to finish. Named benchmark mutex plus process inventory gate. |
| Select proving title and freeze coherent original | Passed | Mario Kart: Super Circuit USA AMKE0, SHA-1 `9d327c030c3e2d9007990518594f70c3340ac56f`; original native binaries/libraries preserved in `baseline/`. Existing generated retail BIOS explicitly included in both configurations. |
| Attribute CPU execution / interpreter / AOT dispatch | Passed, bounded instrumentation | Actual MKSC session phases and dispatch resolver timers; zero dispatch misses/interpreted instructions. Timer overhead and nested phases are explicit limitations below. |
| Attribute copies / allocations / capture / rollback | Passed; retention timing pending | Ordinary allocation counts and exact snapshot sizes. Temporary per-device vector copies identified; staging restore deliberately retained. Large diagnostic capture-ring initialization also identified; prototype deferred after observer-thread review. |
| Attribute scanlines / audio / synchronization | Passed, bounded instrumentation | Scanline, APU, timers/FIFO DMA, serial, PPU, HBlank DMA, VBlank latch/DMA, binding, scheduling, and real transport correction evidence. |
| Audit tile/OBJ, bitmap, affine, blend, windows, mosaic candidates | Passed with baseline gap | Existing tile spans/indexed HD surfaces retained; all six bitmap/tile modes exercised. No mosaic-register consumption found in the compositor: baseline accuracy gap remains unverified, never labeled a pass. Raster cost fixture awaits the same quiet timing window. |
| Implement and qualify candidates separately | One eligible; one deferred | Direct bounded archive sections pass original/candidate 2p and 4p state/pixel/audio comparisons. Diagnostic audio capture chunks are excluded from the branch's production changes; see the disposition below. |
| Balanced A/A and A/B CPU measurements | Waiting on other windows | ABBA process launches, thread cycles and thread CPU time, identical features/inputs, exact-binary A/A control. Both ordinary title execution and repeated capture/restore workloads are prepared. Profiling excluded. Retention requires a repeatable called-path CPU reduction beyond A/A noise and no material regression in unchanged simulation/restore paths. |
| Bundle compatible wins and secondary-title validation | Correctness passed; performance retention pending | One candidate, so no multi-change performance bundle. Emerald USA secondary title passed exact state/image/audio and real delay/rollback comparisons. No title pins or Sonic renderer/UI files changed. |
| Real delay/rollback transport and current-build save/rewind | Title comparisons passed; final quiet transport checks pending | Both titles' original/candidate real UDP peers, latency/loss and actual replay; confirmed bytes identical. Synthetic mid-frame replay/rewind and malformed-load atomicity passed. A later retained-candidate generic rollback check failed checkpoint agreement; the initial pass and later failure are both retained. Quiet original/candidate ordinary rollback and 2p/4p outage checks precede timing. |
| Owner review and one playtest per affected title | Pending | Automated evidence first; normal windows/audio for approved handoff; final integration awaits explicit owner approval. |

## Initial source findings (hypotheses, not performance claims)

The native compositor already uses build-selected HLE tile spans, one window
mask per scanline and RGB555 candidates. The maintained LLE path remains an
original implementation reference. The HD affine service already caches indexed
tile/map surfaces and compares live bytes each published row; those checks also
observe direct native/mod writes. Removing them requires evidence and a complete
replacement visibility contract.

Session `save_state` and `state_hash_parts` each create a complete temporary
device vector per machine and then copy it into an outer archive. `state_hash`
serializes before hashing. `load_state` stages fresh machines before committing,
which protects the live session against malformed inputs. Avoid trading away
that transactional guarantee for a timing improvement.

No candidate may change CPU/device execution, HBlank/VBlank ordering, DMA,
timers, IRQs, display status, memory visibility or sound FIFO behavior. No
presentation replacement permits skipping the underlying device timeline.

## Candidate and correctness evidence

`SimulationArchive::write_section` emits a bounded length prefix, writes the
original canonical device visitor directly into its containing archive, then
fills the prefix by index. Indices survive vector growth. On failure the partial
section is removed. Session capture and per-machine hash parts use this method.
The original standalone device serializer remains available. Load parsing and
the staged commit of validated machines are unchanged. Every capture reads live
memory again, including writes made directly through native/mod memory pointers.

There is no retained VRAM/state cache, dirty-bit scheme, tile-row cache, palette
deduplication or OAM deduplication in this candidate. The existing renderer and
its existing byte-checked HD surfaces remain intact. All CPU/device work remains.

Deterministic 24-frame 2p and 4p streams include modes 0–5, affine register
reloads, blend/window/mosaic register changes, direct VRAM/OAM/palette writes,
changing mid-frame capture points, exact replay, rewind/forward restore,
repeated presentation and rejected truncated/invalid states. They match the
unchanged engine by SHA-256:

| Stream | Original and candidate SHA-256 |
| --- | --- |
| 2p state/pixels/audio | `677254d77eb7cd1f52fbf8b2b697ed3bc51857beed608693ef68fdbbebedef4b` |
| 4p state/pixels/audio | `75eccdd7543db4b4bf8387c89baef8587f14b66288d03e19f358386e1f289f76` |

Clang/MinGW produced the same 2p stream as GCC 15.2 and passed the session tests.
This checks compiler portability on Windows, not a claim of other-OS runtime QA.
The implementation uses portable C++20 and introduces no assembly, SIMD, host
endian loads or CPU-specific backend requirement.

MKSC's original engine created a fresh frame-4000 Peach Circuit race checkpoint
from the existing controller-only route. Emerald's original engine created a
fresh frame-1200 title-screen checkpoint. Each variant ran the same 180-frame
input segment, then a complete-segment restore/replay and five-frame replay.
All reported guest PC/cycle/hash boundaries, both final RGB frames, full session
bytes and captured PCM bytes match. Neither title used interpreter fallback.
The PCM includes 395,008 samples per title, with 387,642 nonzero MKSC samples
and 393,984 nonzero Emerald samples. Audio emulation was enabled; output never
reached speakers. This Emerald check covers startup/title behavior, not a new
claim of trade/battle or full-game qualification.

Real UDP qualification ran each build against an identical-build peer, in both
delay and rollback modes for both titles, through injected latency and 2% loss.
Every pair reached tick 60 with matching confirmed checkpoint bytes; original
and candidate also match one another. Candidate rollback replay counts were
65/28 ticks for MKSC and 63/26 for Emerald. Replays were actually exercised.
Different replay counts across timed network runs are not performance evidence.
No cross-build network compatibility is claimed and production source identity
checks remain unchanged.

The full engine suite ran 121 checks: 120 passed initially. The web configuration
test selected WSL Bash and failed its expected-error check; it passed when run
with MSYS2 Bash on PATH. Final focused PPU, HD, session, netplay and real rollback
checks passed after the profiling build was disabled. See `qualification/` and
`network/report.json` for retained logs and file hashes.

## Deferred audio capture-storage prototype

Restore constructs new audio devices. Each originally allocates and initializes
262,144 diagnostic `CapSample` entries, 3.5 MiB on the tested hosts, in addition
to its playback buffers. This is host observability storage, separate from sound
generation and FIFO execution. A prototype used 4,096-entry chunks with implicit
zero values in untouched chunks. Default copies remained independent, and reset
and legacy deserialize retained existing capture entries as before.

The public capture contract probe matched the original exactly across storage
boundaries, full-ring wrap, reset, copies, queries and fresh/reused-device
restore: 4,282,828 bytes, SHA-256
`ff803b4da215b20c9ff4d3e71f8185856736826bd36c245b738f97d5f2f10cec`.
GCC and Clang matched both this stream and the canonical simulation stream.
Both commercial titles also matched state, pixels and PCM, and their actual
delay/rollback peers matched confirmed checkpoint bytes.

The prototype's full suite passed 120/122 checks. Two outage cases reported
checkpoint-unavailable or recovery-grace failures. A single predeclared paired
original/prototype follow-up passed both 2p and 4p cases: 94 and 80 common
confirmed ticks, respectively, with identical final archives. The initial
failures remain recorded; their cause was not established. See
`qualification/bundle-ctest-all.log`, `qualification/outage-control/`, and
`network-bundle/report.json`. Replay counts and packet totals are not CPU-work
measurements.

Source review then found the supported windowed TCP observer queries capture
on a separate thread (`runtime.cpp` observer thread and `cmd_audio_cap`). Lazy
chunk growth changes metadata publication and lifetime behavior. Sequential
equivalence does not qualify that concurrent reader. The prototype is therefore
deferred before performance acceptance. Its source and patch are retained in
`deferred-audio-chunks/`, with frozen binaries in `candidate-bundle/`. Its two
production audio files were restored; they are absent from the integration
candidate. The new optional capture-contract probe is retained for future work.

Only the archive-copy candidate remains eligible for measurement. A quiet-window
paired transport recheck precedes its CPU runs. After restoring the original
audio implementation, a focused run passed six component checks but failed
`multiplayer_rollback_loopback` with "proposed checkpoint is unavailable or
unconfirmed". This check had passed earlier. The cause remains unresolved;
contention is not established as the cause. Both results are retained in
`qualification/archive-final-focused.log` and the earlier qualification logs.
Quiet checks cover ordinary rollback and 2p/4p outages on the coherent original
and retained candidate; no repeated retry-until-pass qualification is planned.

The restored implementation's GCC capture and 2p/4p simulation streams still
match the original exactly. Clang capture and 2p streams also match, with its
session and playback checks passing. The six component checks passed separately;
these do not substitute for the pending transport checks. See
`qualification/archive-final-correctness.json` and `archive-final-*.log`.
Every restored static-library member matches the frozen archive candidate except
the diagnostic replay object. That object is identical after normalizing its
12-character engine revision string (`7a05e7610f32` versus `2258281732b6`);
`qualification/archive-revision-identity.json` records this comparison. This is
binary provenance evidence, not a claim of cross-build netplay compatibility.

The benchmark guard waits without
a time limit, releases the shared benchmark slot while waiting, checks fresh
F:/G: readiness/space/queues, and rejects samples overlapping other builds,
games, tests or known heavy tools. It never interrupts competing workloads.

## Host cost attribution

ETW sampling was unavailable: Windows refused its performance-profile policy
with `0xc5585011`. No trace or statistical-sampling claim follows from that
attempt. The explicit `GBARECOMP_DETAILED_PHASE_PROFILE` build option instead
adds device/dispatch timers, activated by the existing `GBARECOMP_PHASE_PROF`
environment variable. It defaults off. In the default build the complete
`runtime_arm.cpp.obj` and `runtime_bus_bridge.cpp.obj` objects are byte-identical
to the original, verified from both static libraries.
The deferred prototype also retained byte-identical PPU and HD renderer objects;
the retained archive candidate does not modify those sources either.

The final detailed MKSC observation covers 300 emulated paired frames after the
race checkpoint. These are instrumented wall durations, including timer costs,
not the accepted A/B CPU measurements or percentages of an uninstrumented frame.
Dispatch resolution includes trusted callbacks; binding overlaps CPU/device
phases. Do not sum nested entries as independent work.

| Measured phase | Total ms / 300 paired frames | Calls |
| --- | ---: | ---: |
| AOT dispatch resolution, including callbacks | 206.204 | 3,307,680 |
| Scanline composition | 316.508 | 96,000 |
| Audio | 72.571 | 1,890,654 |
| Timers and sound FIFO DMA | 58.402 | 1,890,654 |
| Serial | 49.742 | 1,890,654 |
| PPU timing | 79.126 | 1,890,654 |
| HBlank DMA | 8.059 | 96,000 |
| VBlank framebuffer latch and DMA | 2.292 | 1,200 |

Sampled session frames report 3.57–5.26 ms in inclusive CPU execution, 3.12–3.80
ms in inclusive device execution, 0.10–0.12 ms in scheduling and 0.64–0.80 ms in
nested binding. Interpreter cost was zero for these fixtures. In the original
synthetic 2p probe, a 1,337,218-byte snapshot made 30 ordinary allocations requesting
5,331,656 bytes per capture. A restore made 48 allocations requesting 19,229,393
bytes. This explains both the capture-copy candidate and why transactional
restore remains a larger follow-up opportunity. Allocation counts exclude
aligned allocation APIs and direct C malloc calls.

## Remaining boundaries

No Sonic title, launcher, mod, renderer, revision profile or engine pin was
changed. Their working recomp-ui/adaptive Fit implementations remain in their
own repositories. This investigation does not newly qualify a Sonic release;
any future Sonic pin/update affecting presentation still requires the owner's
per-revision resize, margin content, entity and HUD checks. No owner-facing game
was launched. A single approved playtest per affected title remains a human
step, using normal windows and normal audio. Final integration is prohibited
until the owner approves the concrete qualified result.
