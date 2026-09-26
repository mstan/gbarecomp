# Native cartridge qualification routes

These files contain controller inputs only. Supply your own matching cartridge,
BIOS, and generated native program to `gba_link_probe`; no assets are included.
Rows are relative frame numbers followed by active-high button masks for seats
zero and one. A row stays active until the next row. Decimal and `0x` masks are
accepted. Snapshot input starts the script's frame count again at zero.

`mksc-multicart-race.inputs` runs from a cold USA Mario Kart: Super Circuit boot,
with empty Flash512 saves, through Multi-Pak connection, 50cc Mario GP, separate
Toad/Yoshi selection, Mushroom Cup, and about twenty seconds of race controls.
It reaches the course on both consoles and exercises acceleration, steering and
hopping. It does **not** finish the race or validate rematch.

Configure the commercial probe as described in `docs/MULTIPLAYER.md`, using the
game's generated code after its `multilink_serial_callback` coverage seed, then:

```sh
GBA_LINK_PROBE_SLICES=1 \
GBA_LINK_PROBE_INPUTS=tests/link/scenarios/mksc-multicart-race.inputs \
GBA_LINK_PROBE_REPLAY=1 \
GBA_LINK_PROBE_CAPTURE_PREFIX=/tmp/mksc-link- \
build-probe/gba_link_probe mario_kart_super_circuit_usa.gba gba_bios.bin 4845 flash512
```

With program identity `mksc-usa-native-probe`, the tested Linux x86-64 run reaches
cycle 1,360,941,120 with canonical hash `fcafd801` before the additional five
replay frames. Those five frames restore/replay to identical whole-session
bytes (1,402,798 bytes). The hash depends on the manifest's program identity.
Inspect the final images as well as the replay result; deterministic execution
alone is not evidence that a game has made progress.

`emerald-load-save.inputs` dismisses the USA Emerald intro and chooses Continue
from a cold boot with an existing valid Flash1M save. Run 1,265 frames with
both save paths supplied. The checked local save reaches Littleroot Town;
another save's resulting location will differ. `emerald-warm-movement.inputs`
then applies separate walking controls for 120 frames. This is loaded-save,
input and rollback qualification, **not** a completed cable trade or battle.
The original local save predates the Pokédex and contains one Pokémon.

Two publicly shared Emerald saves are pinned by source URL and SHA-256 in
`emerald-public-saves.json`. Fetch them only for opt-in local qualification:

```sh
python tests/link/fetch_emerald_saves.py build-probe/public-saves
```

The downloader bounds the response and verifies its exact hash before writing;
it never replaces an existing mismatched file. These are external test data,
not repository assets. All 28 main-save sector checksums in both downloads
validate. Project Complete Dex starts in Oldale's Pokémon Center with one
party member and filled boxes; Blackwing's save starts on Route 117 with three
party members. Both load in the native USA Emerald probe with the Pokédex
enabled. The sources describe edited/assembled collections; no claim of an
unmodified playthrough is made. Cable trade/battle qualification is still in
progress. The probe never writes back to these source files.

## Two-process cartridge tests

Build the chosen probe with `GBARECOMP_NETPLAY=ON`. First use the local probe's
`GBA_LINK_PROBE_STATE_OUT` to capture a paired warm state at a known scene;
neither ROMs nor save files nor that state belong in source control. Then run:

```sh
python tests/link/commercial_loopback.py build-probe/gba_link_probe \
  mario_kart_super_circuit_usa.gba gba_bios.bin flash512 /tmp/mksc-grid.state \
  tests/link/scenarios/mksc-warm-race.inputs
```

Add `--delay` for delay-sync. The default exercises rollback, including forced
incorrect predictions. The harness starts two separate native processes behind
a bounded UDP relay with 40 ms latency and 10 ms jitter per direction. Both
peers verify the full warm-state digest before admitting inputs and agree an
exact confirmed checkpoint afterward. The harness compares every final state
byte and rejects an overflowing relay or rollback that never ran.

The Mario Kart warm race scenario starts at the first race grid, after 3,645
frames of the cold route. Its 120-frame delay and rollback runs agree on
1,402,790 bytes and hash `f907c720` on Windows MinGW and Linux x86-64 (WSL),
using `mksc-usa-native-probe`. Emerald's equivalent warm movement scenario
uses `flash1m` and `emerald-warm-movement.inputs` instead. Its original local
save run agrees on 1,533,848 bytes and hash `b316f504` under Linux delay-sync
and Windows/Linux rollback, with `emerald-usa-native-probe`. Expected hashes
depend on the exact paired state, saves and program identity; results from
different states cannot be compared directly.
