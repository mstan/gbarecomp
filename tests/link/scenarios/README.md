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
