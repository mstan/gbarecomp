# Native GBA link sessions

Implementation branch: `feature/link-session`. Tracking: central Beads
`beads-mc7.21`. Emerald and Mario Kart have playable two-player cable netplay.
The owner verified linked gameplay in both desktop applications, then verified
Minish Cap and Mega Man Zero single-player against the updated runtime.

## Implemented and checked

* `GbaSerialDevice` connects a native peripheral to one machine's MMIO. The
  cable implementation is separate from the session and transport.
* `GbaLinkHub` models multiplayer transfers for 2–4 cable ports. Initial session
  launch is restricted to two copies of the same cartridge/program.
  Normal 8/32-bit transfers also support the multiplayer cable's forward data
  chain, both internal clock rates, externally armed receivers and serial IRQs.
  Port zero owns the cable clock, matching the reference coordinator policy;
  this does not model electrical contention between independent clock drivers,
  a bidirectional normal-mode cable, or every mid-bit register/pin behavior.
* `GbaInstance` owns bus, CPU continuation, clocks, PPU and devices. Generated
  functions still share their executable code and legacy C ABI; the scheduler
  binds an active context only while executing that instance.
* Native IRQ entry, nested IRQ state and exception return survive context
  switches without retaining a native C++ stack. Generated execution propagates
  suspension through ordinary returns; custom callbacks retain exception unwind.
* The session owns the common device timeline. Link events wait for all
  endpoints; host threads and host time cannot decide transfer ordering.
* Canonical little-endian whole-session snapshots have manifest identity,
  CPU continuations, hardware, cable state and scheduler debt. Loads validate
  staging instances before replacing the live session. Failed loads leave it
  intact. Reacquire machine references after successful loads.
* `GbaNetplayHost` implements `RNetRbHost` with an rbengine snapshot ring and
  incremental replay, plus the ordinary recomp-net delay-sync callbacks. The
  network interface contains controller rows, never SIO transactions.
* Replay discards host audio output while retaining mixer/FIFO evolution. The
  core never presents frames or writes save files. Speculative cartridge save
  writes reside in snapshotted memory.
* `GbaNetplayHost::take_output` provides the newest forward frame and audio
  once, selecting the local machine through the manifest's input-seat mapping.
  Replay, stalled admission and duplicate reads cannot emit output. Skipped
  forward frames discard their old audio; other machines never feed the local
  speaker queue. The opt-in product window consumes this interface.
* `GbaNetplayStartup` uses recomp-net's existing MEMCARD upload and BOOT transfer
  operations. Each seat contributes its own save image and RTC seed. The host
  assembles the two-machine state; identity/owner receipts and a state-digest
  barrier precede input admission. Images and native executable code stay local.
  The application must hash-verify its images and supply a build/BIOS/mod identity.
* The host retains a recovery snapshot independently of rollback-ring eviction,
  and can restore it before restarting a match. It does not write save files.
  Replaying across that boundary invalidates the cached candidate; a demoted
  confirmation watermark cannot preserve stale speculative save bytes.
* `GbaNetplayCheckpointAgreement` compares an exact confirmed boundary while
  both drivers can still correct. Each peer checks its snapshot against the full digest,
  exchanges a receipt, and completes a retransmittable ready barrier. A mismatch
  refuses archive export. Agreed archives include both machines, cable and
  scheduler with a version, program identity and whole-archive checksum.
  After bilateral agreement, the match drains rollback and checks that the
  accepted bytes survived unchanged. Unilateral quiescence can suppress a
  correction still owed by the other peer.
* `gba_store_agreed_checkpoint` writes that pair to one application-selected
  recovery file using an exclusive adjacent staging file, a flush and atomic
  replacement (plus a directory flush on POSIX). It never exports individual
  speculative cartridge saves. The bounded loader rejects corruption and wrong
  identity without changing the previous decoded state. Cross-peer storage is
  not an atomic distributed transaction: keep the prior pair until both peers
  can resume the same agreed archive; do not mix independent cartridge exports.
* Connection status allows a 60-second silence grace and reports reconnecting
  after two seconds. Keep pumping the existing transport/driver during that
  grace. A six-second complete UDP outage is exercised by a regression test;
  this is recovery on the existing connection, not fresh-endpoint reconnect.

Validation includes hardware register tests; a real original ARM ROM compiled
and recompiled through `gba_recompile`; mid-transfer snapshot/replay identity;
nested IRQ continuations; malformed-state rejection; input-corrected serial
exchanges and SRAM; and two independent native processes using real UDP with
40 ms receive latency and 10 ms jitter. An additional rollback run injects 2%
packet loss, a complete six-second outage, and a negative startup test that
refuses different runtime identities. The startup-loss scenario drops upload
ACKs and ready replies to verify handshake retransmission. BOOT's owner receipt
can acknowledge an upload whose last ACK was lost; the final application
barrier uses a zero-size SAVE coordination probe because this library pin
retains SAVE replies for retransmission but clears BOOT replies immediately.
Delay-sync and forced rollback compare
the confirmed state-hash timelines. The transport fixture is a small native ABI
program, not a commercial-game qualification.
The harness now injects latency/loss in an external UDP relay with a bounded
queue that fails on overflow. The shared internal 256-packet simulator can
overflow during large paired-state transfers and deliver packets without the
requested latency; such a run cannot qualify latency tolerance.
Loopback tests also compare the complete archives produced by each peer, check
creation/replacement/failed-write recovery, refuse a deliberately changed guest
state, and restore an agreed warm state on a fresh transport with a new session
identity before running thirty further matching input ticks. This test reuses
the UDP endpoints; changed-endpoint discovery still belongs to the lobby layer.

## Reference provenance

Inspected **merged implementation**, rather than historical netplay notes:

| Repository | Revision inspected |
| --- | --- |
| NESRecomp master | `792fd43613a2a7232740003d0ac2244c3868f4ab` |
| SegaGenesisRecomp master | `dcf77ffb3b5f185990a164077eb85668cf3460e2` |
| recomp-net (both integrations' pin; used here) | `588059cbd7bac1e157539fd0ca2b62380be012a4` |
| rbengine (both integrations' pin; used here) | `2a03e73693acee0fb78076ea058642c931bef12e` |
| mGBA behavioral reference | `1d201b22a86d31dfb3bc75145403711f6762015f` |

The native cable code is independently written around GBARecomp's MMIO and
common timeline; it does not copy or mechanically translate mGBA functions.
The factual transfer durations in `gba_link_hub.cpp` were obtained from
[mGBA's SIO implementation](https://github.com/mgba-emu/mgba/blob/1d201b22a86d31dfb3bc75145403711f6762015f/src/gba/sio.c).
The relevant coordination invariants are emulated-time ordering, primary
ownership of starts, peer rendezvous and event completion, studied in
[its coordinator](https://github.com/mgba-emu/mgba/blob/1d201b22a86d31dfb3bc75145403711f6762015f/src/gba/sio/lockstep.c).
No mGBA source is linked into the native targets. `oracle/link` is an optional,
separate reference frontend linked to an externally built mGBA library.

The original fixture agrees with mGBA on received words, final SIOCNT/IDs and
serial IRQ at all four baud rates after a startup settling interval. This is
**not** an instruction-by-instruction cycle-equivalence claim. Native unit
tests check each documented duration and event boundary independently.
The same original ROM now covers normal 8/32-bit transfers at both clocks.
Those cases agree on received data, control fields excluding pin SI, and IRQs.
The forward wiring is also documented in
[GBA Communications Information](https://www.akkit.org/info/gba_comms.html).

There is a retained immediate-start reproduction: run the oracle with a third
argument, e.g. `--immediate`. At the pinned mGBA revision the fastest-baud master
can receive `ffff1111` while the slave receives `22221111`. Verbose coordinator
logs (`GBA_LINK_ORACLE_VERBOSE=1`) show a queued MODE_SET acknowledgement waking
the primary before TRANSFER_START has collected slave data. The default fixture
waits after readiness before starting. Real hardware has not yet resolved this
startup edge; do not reproduce the reference's scheduling artifact by inserting
WAN delay into the native cable.

NDS's melonDS adaptation was also inspected: it contains attributed GPL device
sources. It is useful as a device-boundary precedent, not evidence that renaming
emulator code removes license obligations.

## What the shared network libraries actually do

At these pins, recomp-net's `RNetRbDriver` owns hold-last prediction, input
history, reconciliation, rollback episodes, snapshot cadence policy, identity
and boot digest checks, confirmed hash chains and admission. The host owns all
snapshot bytes and the simulation. `retcomm_rbengine` supplies an opaque
tick-indexed snapshot ring and a monotonic clock; it does not implement the
rollback episode policy.

A snapshot keyed T is **before frame T**. Loading T and replaying T includes
the corrected input. The host requests 120 snapshot slots; default shared-driver
cadence is one per frame. This is storage reach, not a 120-frame prediction
window: recomp-net has a 128-row input history and independently limits
prediction. Driver defaults derive P from D+4, clamped 6–16; the initial test
configuration is D=2/P=6, and the shared admission scheduler can raise D (it
raises it to 5 in the current rollback loopback). Do not describe D=2 as a
guaranteed effective network delay.

Only one shared rollback driver may be started per process because its admission
scheduler is process-global. The two-process test follows that constraint.
The driver's QUIESCE operation drains open episodes but deliberately prevents
opening new corrections: **quiesced does not imply every simulated frame is
confirmed**. The match independently agrees the exact target snapshot on both
peers before draining, then revalidates those bytes. At this pin zero is also the uninitialized watermark,
and a peer NACK can demote it. Retained recovery snapshots therefore remain
candidates: durable save or reconnect logic must explicitly agree an exact
checkpoint tick and digest with the other peer. Neither QUIESCE nor the
watermark alone is a durable-save acknowledgement.

`GbaNetplayMatch` now owns this lifecycle for runners: cold save/RTC startup or
warm paired-state verification, one forward/replay frame per `poll()`, the
60-second reconnect policy, and exact checkpoint agreement. It never waits or
sleeps for the network; callers can keep pumping window and lobby events while
inputs stall. `take_output()` selects the local machine and suppresses replay
audio/video. The launcher starts its chosen transport on the owned session;
the match includes delay/prediction/mode in startup compatibility checks.

`finish_at()` requires an input boundary already agreed by application session
control. It is not a unilateral save request. Only `CheckpointReady` permits
paired archive storage, under the caller's exact image/build identity. The
caller must stop admission before restoring a paired archive and construct a
new match/transport with a fresh session ID. Seven two-process fixture tests
exercise both admission modes, build and delay mismatch rejection before any
guest frame, a six-second outage, startup ACK loss, and paired warm restart on
Windows/Linux. The commercial probe uses this same controller.

`request_checkpoint()` sends save-and-leave control with input rows. Its first
tick is snapshotted and hashed by the network adapter, but never reaches guest
KEYINPUT. Omitting it from the digest could incorrectly confirm a predicted
missing request. Both peers choose the same future boundary only after that
control confirms; request and outage scenarios exercise this path.

## Game application

Emerald and Mario Kart desktop builds enable `GBARECOMP_NETPLAY` by default;
the generic engine and Android retain an opt-in default. They require the
games' regenerated serial coverage and pinned shared recomp-ui netplay backend. Explicit
`GBARECOMP_ROOT`, `RECOMP_UI_ROOT`, and `GBA_GAME_GENERATED_ROOT` CMake paths
allow qualification without replacing existing generated corpora or submodule
pins. `-DGBARECOMP_NETPLAY=OFF` still builds the single-player application.

The shared launcher registers two-player cable rooms and maps input seats to
cable positions, including a host occupying player two. Runtime entry verifies
the original cartridge and retail BIOS, exchanges each owner's save and RTC,
and uses a complete source identity over generated code, game hooks, devices
and networking. Multiplayer skips general plugin activation. Only explicitly
authorized presentation callbacks can build a wider local view. The runner owns
the local window, input, forward audio/video, pacing and reconnect indication.

For direct IP, both executables use the same nonzero session ID, opposite
seats, and reachable UDP endpoints. For example, on one machine:

```text
EmeraldRecomp --rom emerald.gba --bios gba_bios.bin --save player0.sav --netplay-bind 127.0.0.1:5000 --netplay-peer 127.0.0.1:5001 --netplay-seat 0 --netplay-session 123
EmeraldRecomp --rom emerald.gba --bios gba_bios.bin --save player1.sav --netplay-bind 127.0.0.1:5001 --netplay-peer 127.0.0.1:5000 --netplay-seat 1 --netplay-session 123
```

Rollback is the direct-IP default. Both peers may add `--netplay-delay-sync`
and choose `--netplay-delay 2..20` (default six). `--frames N` is an optional
qualification limit which must agree at startup.

Closing once or pressing a configured save hotkey (default Shift+F1) requests
a paired save and leave. Closing again abandons that attempt. The default
archive is `netplay/session-<ID>.paired` beside the executable; override it with
`--netplay-checkpoint PATH`. Cartridge saves are never independently exported.
Resume by giving both players their matching archive with `--netplay-resume
PATH` and a fresh session ID; this option also works before the shared launcher.
The archive includes the entire pair, and requires the exact image/build
identity. There is no GUI archive chooser yet. A save path cannot overwrite
the source cartridge save, ROM or BIOS.

`tests/link/application_loopback.py` drives the actual game executables with
SDL dummy audio/video, a latency relay, whole-archive comparison, fresh-process
resume and original-save hash checks. It does not qualify visible monitor
pacing, audible quality, or Internet lobby/NAT traversal.

### Independent local views

The netplay launcher offers a separate **Your display** choice: Native (3:2),
16:9, 21:9, 32:9, or Adaptive. Both games authorize these choices; each peer may
use a different view/window/monitor. Adaptive follows the window's aspect,
keeping 160 lines and clamping the logical width to 240–576 pixels. Fixed
choices use 284, 373, and 569 pixels respectively; resizing scales/letterboxes
that fixed view. Single-player mod/display settings remain independent.
Direct entry also accepts `--netplay-view native|16:9|21:9|32:9|adaptive`.

`GbaNetplayViewPolicy` is an explicit game capability. Leave `supported` false
for guest-mutating enhancements; set `adaptive_supported=false` to omit/reject
adaptive independently of fixed views and single-player policy. These trusted
callbacks may read active-machine memory and author host pixels only. They do
not activate the ordinary mod system, camera patches, or Mario Kart's 60fps mod.

Each canonical PPU stays 240×160. A separate local presentation PPU observes
the real scanline boundaries and copies the pre-line hidden affine references,
so HBlank road changes render correctly without advancing guest time. Provider
globals are scoped to this mirror. Window geometry, render caches and wide
pixels are excluded from hashes/snapshots. Successful restore reattaches the
observer and invalidates its caches; partial frames use native output until a
complete wide frame is available. The existing output gate hides replay frames
and audio. Emerald scenes without authored margins retain their native content.

`multiplayer_view_tests` covers affine raster parity, unchanged canonical
device state, capability gates and transactional restore. Each game also has
an explicitly built `<GameTarget>ViewProbe` (ROM, BIOS, paired raw probe state,
output prefix). It compares every session byte across fixed/adaptive widths,
live resizing and a 12-frame replay using the actual generated game code and
render hooks. Mario Kart's linked grid and Emerald's link lobby/overworld pass.
At 32:9, measured simulation plus rendering throughput was about 154fps and
105–107fps respectively on the qualification PC, excluding snapshot/network
overhead. This is headroom evidence, not a portable performance guarantee.
The actual application loopback harness accepts `--view0`, `--view1`, and
`--mispredict`; differing views agree identical paired archives and resume under
injected latency/jitter.

The current product smoke passes Emerald on Windows/Linux and Mario Kart on
Windows in both admission modes: 120 cold ticks, then 60 further ticks in new
processes loading the paired archives. Each round produces identical archives
on both peers and leaves original saves unchanged. Windows also passes with
only system directories on PATH after staging SDL2 and the three MinGW runtime
DLLs beside each executable. The final Windows host/transport suite passes all
20 cases; Linux passes eight product/host cases. Five consecutive Windows
rollback runs and five save-request/outage runs pass the bilateral stop fix.

Restart stress exposed a shared transport bug: one finished-transfer ID tracked
both directions, so sending a receipt erased the last received proposal's replay
guard. Delayed BEGIN packets could then reopen an old transfer and stall the
next checkpoint. The recomp-net pin includes `35a2135` (received history per
sender, stale BEGIN rejection and sender-scoped chunks) and `fa8416e` (bounds an
existing lobby test fixture). A captured-UDP regression fails without the fix
and passes with it on Windows/Linux. Five additional jittered paired restarts
pass. The library's 22 Windows tests and 24 Linux tests pass; the Linux total
includes a targeted rerun after correcting the unrelated overflowing fixture.

## State ownership and determinism

The dedicated codec includes EWRAM/IWRAM/VRAM/palette/OAM, IO registers,
timer residues, DMA addresses/debt, pending legacy SIO, cable data/start/deadline,
interrupt flags, CPU banks/return stack/IRQ continuations, full audio channels
and wave RAM, save bytes and command state, RTC serial state and emulated clock,
GPIO lines, PPU raster/affine state and work/latched framebuffers.

ROM, BIOS, executable tables and trusted hooks are external immutable wiring.
Host playback/capture queues, counters used only for diagnostics, pointers,
wall clocks and cache proofs are excluded. Cache proofs are invalidated on
context switches. Host input/frame hooks do not run inside the session. RTC
uses an agreed seed plus emulated cycles; game writes adjust its emulated offset.

The codec rejects unqualified shadow audio, expanded views and gyro/solar/matrix
devices. The native context refuses interpreter fallback for missing coverage.
Public machine mutation is intended for startup/qualification only; a production
runner must freeze feature configuration and image wiring before agreement.

## Memory and performance

Measured two-machine fixture snapshot: 1,271,662 bytes (about 1.21 MiB), without
flash/SRAM. Two 128 KiB Emerald flash chips bring this to about 1.46 MiB;
120 such full snapshots are about 175.5 MiB. Four equivalents would be about
2.93 MiB/snapshot and 351 MiB/history, an extrapolation pending four-machine
session support. The cable payload is 101 bytes for two ports, 147 for four
(GLNK version 2, including normal-serial state).
ROMs, BIOS and native code are shared and are not multiplied by history depth.
The loaded-save Emerald probe measured 1,533,848 bytes at its tested boundary;
continuation depth and identity-string lengths account for the small difference
from the fixture-based estimate.

The current scheduler is deliberately a conservative **qualification backend**:
it runs one recompiled instruction per instance at a time, interleaving device
events and accounting for cycle debt. It is not the final performance design.
An optional `set_native_slices(true)` path now classifies instructions (without
interpreting them) and continues native dispatch through ordinary CPU/RAM work
until the next device event. Device accesses and unknown operations end a slice;
custom dispatch/hooks fall back to the reference path. RAM dispatch callbacks
now have an explicit rendezvous before even their validation reads: a native
ROM run can use batching until it reaches the callback, then resume on the
reference path. This keeps Emerald's copied flash-code hook from disabling
batching for the entire cartridge. Bounded lookahead is less
than the shortest supported cable transfer, so a peer cannot create a previously
unknown completion IRQ behind a machine's execution. Generated-ROM tests compare
full snapshots with the reference from both cold start and mid-transfer.
An additional original generated-code fixture checks ROM-to-RAM callback entry,
device-clock agreement and byte-identical reference/batched continuation. A
240-frame Emerald intro-to-menu segment also matches the previous reference
snapshot byte for byte with batching enabled.
Generated dispatch now propagates scheduler and IRQ suspensions through ordinary
native returns, retaining the explicit guest call/IRQ continuations. Custom
callbacks keep C++ unwinding. A pure optional RAM-hook predicate lets a game
exclude generated ARM RAM code from its Thumb flash callbacks. Lookahead is
bounded to 63 cycles, below the fastest supported 64-cycle cable transfer.
Immutable dispatch-table lookups are cached; sampler enablement is read once
instead of scanning the Windows environment on every instance binding.

The 120-frame Emerald battle-menu comparison matches all 1,533,904 bytes against
the previous exception-based execution, including across Windows/Linux (SHA-256
`fe1b3d12251a691721695a4f08354f87cf456a07a964c6d71c2b291da936caba`).
On the development machine, Windows simulation alone measures 146.8 FPS
(6.81 ms mean, 7.40 ms p95). This is processing capacity, not network FPS.
The original generated fixture still agrees with the separate mGBA oracle for
multiplayer at all baud rates and normal 8/32-bit at both clocks.

The application pacer accounts for frame execution, retains its deadline through
brief jitter, and resets after a long outage. Transport is pumped between frame
deadlines; replay consumes no presentation slot. GBA matches now start with six
input-delay frames (~100 ms), configurable from 2 to 20. The shared rollback
scheduler can adapt that delay; the direct delay-sync admission path cannot.
At 40 ms latency plus 10 ms jitter in each direction, two frames gave only
28.9 FPS in delay-sync; six give 59.3 FPS in the Linux Emerald connected room.
A 600-tick Windows Mario Kart race rollback run gives 58.1–58.6 FPS and identical
1,402,794-byte states (`3de0d0f3`). These measurements use ordinary predictions,
not the intentionally corrupted prediction stress mode. They establish short
headless in-game performance on this machine, not Internet or window/audio
qualification on other hardware.
Windows additionally uses a scoped high-resolution waitable timer between polls;
ordinary `Sleep(1)` rounded toward a 16 ms quantum and reduced Mario Kart
delay-sync to 48 FPS despite ample CPU capacity. With the timer, the same
600-tick run reaches 59.1–59.3 FPS and exactly the same final state. The timer
never affects guest clocks and does not change global system timer policy.
This path remains opt-in pending commercial-game and broader codegen coverage.

Larger native slices need a proven SIO/MMIO rendezvous boundary and deterministic
ordering relative to peers that might access SIO sooner. Simply running each
machine a whole frame or allowing it to cross a newly created cable event is
incorrect. Compare optimized traces against this backend before replacement.

Later snapshot optimizations: fixed-width bulk copies, reusable storage, dirty
pages/deltas or copy-on-write, and exclusion of disposable rendering data when
correct presentation reconstruction is proven. No delta/COW scheme is assumed
in the current memory estimates. Archive numeric sizes currently target 64-bit
Windows/Linux/Steam Deck; qualify explicit widths before adding 32-bit targets.

## Future topology

Machine IDs, image/program identities, boot source, input seats and link media
are separate. The manifest supports more machines than one physical cable;
the MVP validator refuses unsupported launches explicitly.

* Cable: maximum four attached consoles, two initially.
* Wireless: a future adapter implements its local serial protocol and connects
  to a separate deterministic radio-domain coordinator. An RFU group permits
  one parent and four children; Emerald's eight visible Union Room leaders are
  not an eight-console cable or RFU group. See the game definitions
  [RFU constants](https://github.com/pret/pokeemerald/blob/master/include/librfu.h)
  and [Union Room constants](https://github.com/pret/pokeemerald/blob/master/include/constants/union_room.h).
  There is no hardcoded global 2/4/8-machine ceiling in the manifest. The pinned
  network library itself has eight seats and will need work if a future radio
  domain requires more independently controlled machines.
* Single-Pak: a future boot source owns downloaded RAM code and boot protocol;
  it must supply native code coverage without pretending every console has a
  cartridge. Deferred now.
* Cross-version Pokémon: each machine already names its own program/ROM;
  per-context dispatch is possible. Peers will need every participating image
  locally. No ROM distribution is provided. Emerald ↔ Emerald remains first.

## Remaining qualification limits

The following are not implemented/qualified by the fixture tests:

* Native batching and full generated/BIOS interior-resume coverage (including
  exceptional codegen paths), DMA beat/event ordering, and remaining normal-mode
  pin/clock edge cases.
* The owner has verified visible launcher host/join and linked gameplay in
  Emerald and Mario Kart on Windows. Automated application smoke additionally
  uses SDL dummy drivers; other hardware/display/audio combinations remain
  unqualified.
* Product recovery UX beyond explicit paired-archive CLI resume, process and
  power-loss qualification, and changed-endpoint discovery through the lobby.
  Agreement does not prove both peers' disks completed a write.
* Real Internet/NAT and Steam Deck execution. Sustained linked gameplay and
  playable speed under injected latency pass the recorded probes below;
  full battles, race completion and rematches are not required release gates.

The runtime/host fixtures and both loopback modes pass in Windows MinGW Release
and Linux x86-64 Release under WSL. Target-game qualification below is separate
from Steam Deck presentation/audio integration.

The headless native Emerald probe runs two machines through the real BIOS and
loads the available cartridge save into the overworld. It found the copied
IntrSIO32 IRQ routine at 03004664
(ROM 082E3554, 0x960 bytes), which is now mapped in Emerald's game configuration
on `feature/link-serial-coverage`. The probe also installs the regular game's
byte-validated flash RAM dispatch hook per instance. Those early results were
boot/native-coverage evidence; later cable and performance qualification follows.
With a loaded Emerald save, five additional frames restore and replay to identical
whole-session bytes. Windows and Linux also produce identical canonical hashes
for all eight original-ROM serial scenarios.
The Windows/Linux two-process Emerald movement probe also reaches an agreed 120-tick
checkpoint after rollback under 40 ms latency plus 10 ms jitter per direction:
1,533,848 identical bytes, hash `b316f504` for its specific loaded-save state and
`emerald-usa-native-probe` identity. This does not qualify trade/battle; the
original local save is before the Pokédex with only one party Pokémon. Two
publicly shared test saves have since been obtained with source/hash provenance
and validated sector checksums. The 8,815-frame cold preparation route reproduces
the staged cable-desk snapshot exactly. Both native cartridges enter the Trade
Center, exchange Torchic and Salamence, complete the coordinated save and return
to the party menu. Read-only assertions verify reciprocal personality/trainer
identities in RAM and in each newest complete flash bank, unchanged other party
members, Pokemon checksums, all 28 sector checksums per cartridge and zero
game-reported cable errors. The source saves remain unchanged.

Full input-segment restore/replay also matches across actual flash programming
and pre-trade saves (300 and 440 frames); the completed 440-frame segment has
1,533,888 identical bytes on Windows/Linux. A connected Trade Center movement
test reaches the same 120-tick checkpoint under Linux delay-sync and Windows/Linux
rollback, with 40 ms latency and 10 ms jitter in each direction: hash `b6109e3a`,
1,533,888 bytes. The full 3,600-tick Linux network trade also passes under both
delay-sync and forced rollback: hash `d7ed71cf`, 1,533,880 identical bytes, with
reciprocal Pokemon identities verified in both newest save banks. The recorded
rollback peers resimulate 3,274 and 1,999 ticks.

Emerald also connects into a native Single Battle after that trade. The first
knockout passes 1,350 ticks of Linux delay-sync and forced rollback with the same
relay: 1,533,880 identical bytes, hash `a5580339`, and 1,125/746 replay ticks.
An independent read-only assertion checks PP use and that the defeated Pokemon's
owner received its new HP state. Local continuation exercises another knockout,
a voluntary forfeit and return to the Colosseum. Both cartridges save the
complementary win/loss counters, verified after decryption in RAM and flash.
The final 1,200-frame result/save segment replays to identical whole-session
bytes. The 1,350-tick network result/save segment also passes delay-sync and
rollback: 1,533,852 identical bytes, hash `b94cc88f`, with 1,134/708 replay ticks.
Both newest save banks contain the complementary win/loss records with all
sector checksums valid. Broader platform/performance coverage and product
persistence/recovery remain separate gates. Additional complete gameplay
sequences are not required; qualification now targets entry into linked
gameplay, sustained synchronization/connection, and playable speed.
See `tests/link/scenarios/README.md` for the controller routes, independent
trade checker and reproduction without distributing cartridge/save assets.

Mario Kart's native Multi-Pak route now connects both machines, selects separate
Toad/Yoshi characters and enters Mushroom Cup with independent race controls.
The reproducible 4,845-frame cold-boot route and its limitations are documented
in `tests/link/scenarios/README.md`. It exposed an unseeded serial callback at
0802DB5C (descriptor pointer at 080DE1B4), now added to the game's configuration
on `feature/link-serial-coverage`. Five-frame replay during the linked race
matches whole-session bytes. Race completion and rematch were not tested and
are not required for the initial multiplayer release.
The real-cartridge UDP probe now also runs the native linked race under both
delay-sync and rollback. The exact 120-tick checkpoints match across both modes
and Windows/Linux: 1,402,790 bytes, hash `f907c720` for the documented warm grid
state and `mksc-usa-native-probe` identity. See `tests/link/scenarios/README.md`
for the opt-in harness; it requires locally supplied images and paired state.

These limits do not imply that the initial two-player desktop cable path is
unusable. Track outstanding qualification and integration status in Beads.

## Reproducing validation

Configure/build the ordinary native engine, then build `link_tests`,
`multiplayer_session_tests` and `codegen_tests`. With `arm-none-eabi-gcc` and
`arm-none-eabi-objcopy` available (devkitARM is detected on Windows), the
`link_rom_tests` target compiles the original fixture and recompiles it.

For the optional network targets:

```sh
git submodule update --init external/recomp-net external/rbengine
cmake -S . -B build-link -DGBARECOMP_NETPLAY=ON
cmake --build build-link --target multiplayer_netplay_tests multiplayer_netplay_peer
ctest --test-dir build-link -R 'multiplayer_(netplay_tests|delay_loopback|rollback_loopback)' --output-on-failure
```

For the external oracle, obtain the exact mGBA revision above and build Release
with `LIBMGBA_ONLY=ON`, `BUILD_STATIC=ON`, `BUILD_SHARED=OFF`, frontends/GL/GLES
and optional codec/archive dependencies disabled. Configure `oracle/link` as a
standalone CMake project with `MGBA_SOURCE` and `MGBA_BUILD` pointing to those
matching trees. Set `GBARECOMP_LINK_ORACLE_EXE` in the native build to the resulting
executable; `link_mgba_differential` then compares separate processes. None of
these oracle dependencies are required for the default native build.

The commercial probe is opt-in and reads cartridge saves without writing them:

```sh
cmake -S . -B build-probe \
  -DGBARECOMP_LINK_PROBE_GENERATED=/path/to/regenerated/emerald \
  -DGBARECOMP_LINK_PROBE_SHA1=f3ae088181bf583e55daf962a92bb46f4f1d07b7 \
  -DGBARECOMP_LINK_PROBE_PROGRAM=emerald-usa-your-build-id \
  -DGBARECOMP_LINK_PROBE_SETUP_SOURCE=/path/to/EmeraldRecomp/tools/link_probe_setup.cpp
cmake --build build-probe --target gba_link_probe
GBA_LINK_PROBE_REPLAY=1 build-probe/gba_link_probe emerald.gba gba_bios.bin 600 flash1m player0.sav player1.sav
```

Regenerate using Emerald's base game.toml, generated symbol overlay, reviewed
seeds overlay and imported function/data symbol tables. Do not ship generated
code or cartridge assets from this qualification build.
