# Native Wireless Adapter / Union Room

Tracked by `beads-mc7.23`.
First acceptance target: two USA Emerald instances enter the Union Room,
discover one another and initiate an interaction, with deterministic replay.
No complete battle or trade walkthrough is required for this milestone.

The developer declares supported serial devices. The lobby host chooses the
connection type before starting; joining players inherit it. The selected
medium participates in startup compatibility and checkpoint identity.

## Implementation sequence

1. Implement an independently written RFU adapter behind `GbaSerialDevice`:
   GPIO reset, Nintendo ID exchange, normal 32-bit serial transfers, SI/SO
   acknowledgement, commands, clock handover and serial IRQ deadlines.
2. Add a deterministic local radio domain: advertisements, searches,
   parent/child association, bounded data buffers and notifications. Deliver
   device events only on the shared emulated timeline. No host-time radio or
   network serial packets.
3. Snapshot adapter parsers, pins, in-flight transfers, radio associations,
   payloads, search state and deadlines with the whole session. Test reset,
   failed restore and replay across discovery and clock handover.
4. Route Emerald's wireless selection through the existing session/netplay
   runner and host-selected lobby setting. Keep cable behavior and defaults.
5. Qualify two real Emeralds using copied existing saves, first locally, then
   through the input netcode; capture room discovery/interaction and verify
   replay identity. Re-run relevant cable/session regressions.

## Sources and scope

Hardware behavior reference: the logic-analyzer research by Corwin / kuiper,
extended by afska and davidgfnet, in
[gba-link-connection](https://github.com/afska/gba-link-connection/blob/c61bf351f68ad2d6e1c9d72d70e21bec19adfc0b/docs/wireless_adapter.md).
Emerald's `librfu_*` driver and Union Room code provide the guest-side contract.
Neither implementation is embedded or mechanically translated into the runtime.
mGBA master currently lists RFU as planned and has no wireless SIO device; it
remains useful for cable regression, not an RFU differential oracle.

One wireless association contains a parent and at most four children.
Emerald separately maintains eight visible Union Room leader entries. A radio
domain may contain several groups; neither limit is a global machine count.
The initial application still admits two copies of the same game. Domain
membership, input seats, radio IDs and parent/child slots are distinct.

Single-Pak multiboot, cross-version cartridges, more than two network players,
RF interference fidelity and complete accessory compatibility are deferred.
The adapter parser and deterministic radio model must leave room for them.

The [mixed-cartridge design](MIXED_CARTRIDGE_MULTIPLAYER.md) separates cartridge
compatibility, RFU association size and Union Room display population, and
describes the local/LAN/online admission changes needed for future cross-version
sessions. Those features are not enabled by the current implementation.

## Qualified on 2026-09-26

Two native Emerald USA instances cold-booted from the previously qualified
trade-ready cartridge saves, entered the Union Room, discovered one another,
and initiated contact. The initiating console displayed the activity menu;
the receiving console displayed "Somebody has contacted you." The complete
5,485-frame input sequence reproduced identical final session bytes after
restore (1,536,679 bytes). The single-process optimized probe averaged 126 fps
while simulating both consoles. No battle or trade completion is required or
claimed by this milestone.

The same 600-frame interaction from the warm room state ran in separate Windows
processes through recomp-net. All three cases agreed on exactly 1,536,643 bytes,
SHA-256 `1cce67d5a5d7654b8250ae3dbd55b407d408e69eecf84e6d1928f51f443b7150`:

| Input mode | Simulated receive latency per peer | Forward speed |
| --- | --- | --- |
| Delay-sync, D=6 | 40 ms +/- 10 ms | 56 fps |
| Rollback, D=6/P=6 | 40 ms +/- 10 ms | 59-60 fps |
| Rollback, D=6/P=6 | 80 ms +/- 30 ms | 52-53 fps |

The harsher case exercised a rollback correction. These are local two-process
simulator measurements, not a claim of Internet-service or every-PC performance.
The ordinary desktop build and launcher bridge compile and pass their checks.
The launcher starts using isolated copied assets/saves in the private preview.
The new lobby codec/host-authority tests pass on Linux; the commercial RFU
execution above was qualified on Windows.

Twelve targeted engine tests cover cable behavior, RFU protocol and timeout,
reset/disconnect, session restore, launch capabilities, delay/rollback,
incompatible startup policy and restart. Shared lobby tests cover online JSON,
LAN file and Direct IP JOIN/CAPS/START, guest authority and unsupported variants.

## Developer integration and reproduction

Cable remains the default. A game opts in by setting
`GbaNetplayLaunch::supported_media` to include the bit for
`GbaLinkMedium::Wireless`. The shared lobby lists only the declared devices;
the host chooses **Lobby Settings -> Connection type**. LAN and online clients
inherit the same stable device ID. The runtime independently validates that
choice and includes it in startup/checkpoint identity. Direct-IP CLI users can
select `--netplay-device wireless` on both endpoints.

Emerald also needs the five indirect ARM entries in the copied RFU IRQ block
and the byte-validated native `rfu_STC_fastCopy` callback. Regenerate from this
integration's `game.toml`; the older cable-only native corpus is insufficient.

Configure the commercial probe with the normal generated/ROM/program settings
and `GBARECOMP_LINK_PROBE_SETUP_SOURCE` pointing to Emerald's
`tests/link_probe_setup.cpp`. Use `GBARECOMP_LINK_PROBE_OPTIMIZATION=1` for speed
measurements (the default 0 favors build time). Set `GBA_LINK_PROBE_DEVICE=wireless`,
`GBA_LINK_PROBE_SLICES=1`, and `GBA_LINK_PROBE_REPLAY_RUN=1`. The input route is
`tests/link/scenarios/emerald-wireless-union-room.inputs`, run for 5,485 frames.
It expects the two private ready-save fixtures, in order, with SHA-256:

* `98983c44eed082efc7441d3b31e1209ccdf996daf9e5a1d59311269c84833d37`
* `73958d8968f45a2d8c1f2fd524b91ec353455918358b34615262e7f4aa609d7d`

No cartridge, BIOS, save image or generated guest code is included in this
repository. The probe reads original save images without writing them.

## Fidelity limits

The normal-32 SPI shifter uses 256/2,048 emulated cycles for the two supported
clock rates. Radio delivery uses a provisional deterministic 4,096-cycle service
interval, with ideal reception; it is not yet a measured RF timing model.
Disconnect recovery commands, RF interference/retry behavior, discovery rotation
through more than four simultaneous advertisers, and obscure RFU commands still
need separate qualification. The domain can represent multiple independent
five-member groups (unit-tested with ten adapters), while application admission
remains two players. This work is a Union Room preview, not full RFU compatibility.
