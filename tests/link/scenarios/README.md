# Native cartridge qualification routes

These files contain controller inputs only. Supply your own matching cartridge,
BIOS, and generated native program to `gba_link_probe`; no assets are included.
Rows are relative frame numbers followed by active-high button masks for seats
zero and one. A row stays active until the next row. Decimal and `0x` masks are
accepted. Snapshot input starts the script's frame count again at zero.

`GBA_LINK_PROBE_REPLAY_RUN=1` saves the starting paired state, runs the complete
local input segment, restores its start and repeats every input. It compares
every final snapshot byte, including cartridge save chips and in-flight cable
state. Unlike `GBA_LINK_PROBE_REPLAY=1`, it does not append five extra frames.
Use this around save confirmations and link transitions; use the two-process
rollback harness below to test corrections arriving over the network.

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
unmodified playthrough is made. The probe never writes back to these source
files.

`emerald-public-cable-desk.inputs` uses those saves in the order above. It runs
8,815 frames from cold boot, withdraws Bulbasaur to give the first player two
party members, navigates both players to Pokemon Centers, dismisses the first
visit tutorial, and stops at the cable counter. The uninterrupted Linux run
matches the state assembled while qualifying each segment: cycle 2,476,098,240,
hash `55e5fae4`, with `emerald-usa-native-probe`. This route includes ordinary
menu delays and detours; it changes no guest memory directly.

Native cable-entry saves then advance the two flash chips to generations 3/4
and 1658/1659, with all 28 main-sector checksums valid on each. Full-segment
replay passes across 300 frames of active programming and another 440 frames
through completion. The latter state's 1,533,888 bytes match on Windows and
Linux, hash `4c1cdff0`. Both games subsequently detect IDs 0/1, exchange trainer
data and enter the Trade Center without game-reported cable errors.

`emerald-public-cable-trade.inputs` continues from that cable-desk checkpoint
for 6,810 frames. It saves both games, connects them, enters the Trade Center,
selects Torchic and Salamence, confirms the exchange, and returns to the party
menu after the coordinated save. The checked local run ends at cycle
4,389,000,000, hash `c7e8f81e` (1,533,880 bytes). The original personality/trainer
identities exchange in both
parties and in the newest complete save banks (generations 4/5 and 1659/1660);
the other party members are preserved. Pokemon checksums and all 28 sector
checksums per cartridge validate. Five-frame whole-session replay also passes
during the animation and after completion.

`emerald-warm-trade.inputs` is the 3,390-frame suffix starting at the exchanged
party menu, cycle 3,436,762,560. Validate a completed slot-zero trade with:

```sh
python tests/link/emerald_trade_check.py /tmp/emerald-party-menu.state /tmp/emerald-after-trade.state
```

The checker reads snapshots only. It rejects an unchanged party, nonreciprocal
exchange, duplicate offered identities, a bad Pokemon checksum, incomplete or
corrupt flash sectors, a stale saved party, and game-reported link errors.
It is a pinned USA Emerald qualification tool, not a general save-state decoder.

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

For the complete Emerald trade, use the party-menu snapshot and
`emerald-warm-trade.inputs`, adding `--frames 3600 --timeout 1800 --emerald-trade`.
The extra idle frames allow the final coordinated save to settle. `--timeout`
only changes the probe's simulation deadline (seconds, maximum one hour);
startup, checkpoint agreement and reconnect limits retain their usual bounds.
`--emerald-trade` checks reciprocal persisted Pokemon identities in the final
agreed state as well as equality between peers.

The complete 3,600-tick Linux delay-sync and rollback trades pass those
assertions with 1,533,880 identical bytes, hash `d7ed71cf`, under the same
latency/jitter relay. The rollback peers resimulate 3,274 and 1,999 ticks in
the recorded run, including the trade and its coordinated flash save. This
advances the session across the 32-bit cycle boundary. This is native probe
qualification; the product launcher and recovery/save UI are still separate
release gates.

The Mario Kart warm race scenario starts at the first race grid, after 3,645
frames of the cold route. Its 120-frame delay and rollback runs agree on
1,402,790 bytes and hash `f907c720` on Windows MinGW and Linux x86-64 (WSL),
using `mksc-usa-native-probe`. Emerald's equivalent warm movement scenario
uses `flash1m` and `emerald-warm-movement.inputs` instead. Its original local
save run agrees on 1,533,848 bytes and hash `b316f504` under Linux delay-sync
and Windows/Linux rollback, with `emerald-usa-native-probe`. Expected hashes
depend on the exact paired state, saves and program identity; results from
different states cannot be compared directly.

`emerald-trade-room-movement.inputs` starts with both public-save players just
inside the connected Trade Center, at cycle 3,171,315,840. Its 120-frame Linux
delay and Windows/Linux rollback runs also agree on every byte: 1,533,888 bytes, hash
`b6109e3a`. Both rollback peers resimulate frames. This exercises the game's
active cable protocol and independent movement; the input segment itself does
not complete a Pokemon trade.

## Emerald cable battle

`emerald-after-trade-to-battle.inputs` continues from the completed local trade
at cycle 4,389,000,000. Its 7,780 frames close the Trade Center, reconnect through
the Single Battle service, save both cartridges, enter the Colosseum and begin
Salamence versus Torchic. This route is assembled from individually qualified
native input segments; it has not separately been rerun uninterrupted. The
result is cycle 6,574,370,880, hash `f644c2a4`. A 700-frame full replay through
battle entry matches all 1,533,880 snapshot bytes.

From that checkpoint, `emerald-warm-battle-ko.inputs` runs 1,350 frames through
Headbutt and Torchic's knockout. The read-only checker verifies move PP use,
the battle master's HP result, its delivery to the owning cartridge's party,
and healthy cable state. Emerald's second cartridge runs the opponent battle
controller; its `gBattleMons` is not an independent combat authority.

```sh
python tests/link/emerald_battle_check.py /tmp/battle-intro.state /tmp/first-ko.state
```

The two-process harness accepts `--frames 1350 --timeout 900 --emerald-battle-ko`
with that warm checkpoint and script. Both Linux delay-sync and rollback pass:
1,533,880 identical bytes, hash `a5580339`, under 40 ms latency plus 10 ms jitter
per direction. The recorded rollback peers replay 1,125 and 746 ticks. The
checker rejects an incomplete combat turn instead of accepting matching peers
as evidence of a knockout.

`emerald-battle-to-result.inputs` continues locally from the battle-intro state
for 8,500 frames. It knocks out Torchic and Magcargo, sends out Smeargle, then
the second player voluntarily forfeits. Both return to the Colosseum and save
complementary win/loss records. This stitched route includes menu delays and
detours from the individually checked segments. The final cycle is
8,961,986,880, hash `b2a882d1`, 1,533,896 bytes. It qualifies a forfeit ending,
not a victory from defeating the entire opposing party.

`emerald-warm-battle-result.inputs` is the final 1,200-frame segment, starting
at cycle 8,624,911,680 with Smeargle entering battle. Full-segment replay matches
all bytes through the result save and field return. The result checker verifies
that each cartridge's encrypted wins/losses/draws counters advance correctly in
RAM and in the newest complete flash bank, with all 28 sector checksums valid:

```sh
python tests/link/emerald_battle_check.py /tmp/last-mon.state /tmp/battle-exit.state --result
```

The equivalent network harness options are
`--frames 1350 --timeout 900 --emerald-battle-result`, using the last-mon state
and `emerald-warm-battle-result.inputs`; the extra 150 idle frames let the save
settle. Public source save files remain unchanged throughout qualification.

Both network modes pass this result/save segment on Linux: 1,533,852 identical
bytes, hash `b94cc88f`. The rollback peers replay 1,134 and 708 ticks. Both newest
flash banks contain the complementary win/loss counters and valid sector
checksums. These existing longer routes remain reproducible evidence; future
acceptance focuses on sustained connected gameplay at playable speed, rather
than requiring full battles, races or rematches.
