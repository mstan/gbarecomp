# Mixed-cartridge multiplayer: compatibility and implementation plan

Status: design, tracked by `beads-mc7.24`. Cable and Wireless Adapter support
are merged, but application admission still requires **two copies of the same
game/build**. Nothing in this document enables cross-version play or larger
network sessions.

## Original-game compatibility

For Generation III trading, the target matrix is below. `C` means Game Link
Cable, `W` means Wireless Adapter, and `—` means that medium is unavailable.
These are original-game capabilities, **not a claim that these combinations
have been implemented or tested in GBARecomp**.

| Cartridge | Ruby | Sapphire | Emerald | FireRed | LeafGreen |
|---|---|---|---|---|---|
| Ruby | C | C | C | C | C |
| Sapphire | C | C | C | C | C |
| Emerald | C | C | C/W | C/W | C/W |
| FireRed | C | C | C/W | C/W | C/W |
| LeafGreen | C | C | C/W | C/W | C/W |

Emerald, FireRed and LeafGreen have the wireless Union Room. Ruby and Sapphire
do not implement RFU and must use cable. A cable cannot put Ruby into the Union
Room. Trades remain two-player activities even when the medium supports a
larger group. Do not reuse this trade matrix as a blanket assertion about
record mixing, contests, minigames, all language/revision combinations or
every battle mode.

The guest retains its progression, party and species restrictions. For example,
Emerald's trade code checks its Champion flag and the FR/LG partner's story
progress, and the wireless code also checks the partner's national-link flag.
Lobby compatibility must not bypass these checks or promise a trade is currently
available merely because the cartridges can communicate.

Sources: Nintendo's [Emerald product description](https://www.nintendo.com/en-gb/Games/Game-Boy-Advance/Pokemon-Emerald-Version-267112.html)
identifies the five cable-compatible games. The inspected guest implementations
are [Emerald trade eligibility](https://github.com/pret/pokeemerald/blob/c2dcc629fd97b3d58fedc07c9e02653eabed9136/src/trade.c),
[Emerald wireless eligibility](https://github.com/pret/pokeemerald/blob/c2dcc629fd97b3d58fedc07c9e02653eabed9136/src/union_room.c),
and [Ruby/Sapphire cable implementation](https://github.com/pret/pokeruby/blob/63a8cbf0016b351a4e68f7036fa0b77e23d2f2c1/src/link.c).

## Four, five, eight and forty mean different things

| Quantity | Meaning |
|---|---|
| 4 | Maximum machines on one GBA multiplayer cable. |
| 5 | One RFU association: one parent and up to four children. |
| 8 | Visible/recently discovered group-leader entries in Emerald's Union Room. |
| 40 | Union Room allocates 8 × 5 other-player sprites, representing those groups. This is display capacity, not forty simultaneous connections to one adapter. |
| 2 | Current GBARecomp application/network admission for either medium. |

The guest explicitly distinguishes the eight leader entries, forty sprites,
and a separate sixteen-entry recently connected player list. Its room view is
not proof of a single globally synchronized forty-player session or a precise
total including the local trainer. Preserve independent machine IDs, input
seats, radio-domain membership, association slots and discovery entries.

Sources: Nintendo's [Wireless Adapter manual](https://www.nintendo.com/eu/media/downloads/support_1/game_boy_advance_4/HealthSafety_wireless_GBA_UK.pdf),
[guest link limits](https://github.com/pret/pokeemerald/blob/c2dcc629fd97b3d58fedc07c9e02653eabed9136/include/link.h),
and [Union Room population/display definitions](https://github.com/pret/pokeemerald/blob/c2dcc629fd97b3d58fedc07c9e02653eabed9136/include/union_room.h).

Larger rooms need explicit performance and protocol work. recomp-net currently
has eight-slot input/history/scheduler/rollback limits and 32-bit occupancy
masks. The server caps player seats at eight, while the existing LAN/Direct IP
control path carries one joiner. GBA's startup exchange is also two-seat-specific.
Increasing a lobby number does not expand any of those implementations. RFU
discovery rotation beyond four simultaneous advertisers also remains unqualified.

Every peer still simulates every machine in its deterministic session. As a
rough capacity illustration only, the measured 1.54 MB two-machine RFU snapshot
would scale to about 31 MB for forty machines and 3.7 GB for 120 full snapshots,
before allocator/ring overhead; CPU cost needs its own measurement. Do not
promise forty-player performance from the current two-player proof. A future
service with separate small simulation groups would require a separate design
for discovery and deterministic migration; it must not tunnel RFU transactions
over WAN as a shortcut.

## Code inspected and current blockers

| Layer | Inspected source and existing behavior |
|---|---|
| GBA manifest | `src/runtime/multiplayer_config.{h,cpp}` has per-machine `program_id`, `rom_sha1`, boot source, link groups and seat mapping. Structural validation allows different cartridges; `validate_multiplayer_mvp` explicitly rejects them and requires two machines. |
| GBA runner | `multiplayer_runner.cpp` builds both machines from one ROM/program and one overall build identity. Startup/save ownership is two-seat-specific in `multiplayer_startup.cpp`. |
| Native execution | `RuntimeArmContext::program_dispatch` exists, but a declined callback falls through to the executable's one generated table. A non-null callback also disables the current generated native-slice fast path. This hook alone is not a qualified multi-program execution backend. |
| Snapshot | `multiplayer_session.cpp` already identifies each machine and the topology. Program bindings and deterministic per-program configuration need to be restored alongside that manifest. |
| LAN/Direct IP | recomp-net `rnet_lan_lobby.c` and `rnet_lan_direct.c` enforce the room's game and version strings; changing only the online service would leave local play incompatible. |
| Online service | recomp-net-server `ws_lobby.rs` filters room lists by game/version and rejects mismatched joins. `match_caps` preserves arbitrary JSON keys within a 4,096-byte limit, but that does not override admission checks. |
| Shared UI | `recomp_netplay_host.c` publishes one game identity and an opaque device variant. It has no per-machine cartridge selector or installed-program resolver. |

Inspected engine revision: `e11d596`; recomp-net: `dfd5821`; recomp-ui:
`faa3330`; [recomp-net-server](https://github.com/RetroPortingToolKit/recomp-net-server/tree/bdd3900622ff3f39a0f675b634885ad510b3952c):
`bdd3900`. These are source findings, not a claim about the deployed server's
current revision.

## Proposed contract: compatibility profile plus exact session manifest

Use a developer-owned compatibility catalog shared by the local launcher and
online admission path. A matrix is useful, but a title-only allowlist is too
broad. Profiles should specify a stable family ID, schema/profile revision,
medium, activity capabilities, accepted cartridge revisions/languages and
ordered permitted combinations. Model reciprocal permission explicitly;
compatibility is not generally transitive, and pairwise permission alone does
not prove a three- or four-cartridge group works.

For example, proposed profiles could be `pokemon-gen3/cable-trade/v1` (the five
games above) and `pokemon-gen3/union-room/v1` (Emerald/FireRed/LeafGreen). These
names are design examples, not currently implemented config keys. A Union Room
profile permits entering the room; it does not promise every in-room activity
is shared. Existing same-title lobbies keep their current admission behavior.

The developer declares the allowed profiles/devices. The host chooses the
medium and session roster; each player chooses an allowed cartridge for their
machine. All peers then agree on **one identical ordered manifest**, including:

- Manifest schema, compatibility profile/revision and catalog digest.
- Engine simulation ABI/build identity, BIOS identity and deterministic policy.
- For each stable machine ID: game/program ID, verified ROM revision hash,
  platform-independent native simulation build ID, boot source, save-device
  configuration, simulation-affecting mod plan and RTC seed policy.
- Ordered cable ports or wireless-domain members, with separate input-seat and
  save-owner mappings. Runtime RFU associations remain snapshotted guest state.

Do not require all machines *within* that manifest to have the same ROM. Require
all peers to resolve **the same manifest**, including the same program for each
machine. Compare platform-independent simulation identities, not Windows PE
versus Linux ELF executable hashes. Different presentation-only settings remain
subject to each game's existing netplay policy.

For Emerald ↔ FireRed, **both peers need locally supplied, verified images and
compatible native programs for Emerald and FireRed**, even if each controls only
one of them. Report missing local requirements before ready/start. Do not solve
ROM distribution or download peer-supplied executable code.

## Ownership and negotiation

1. **Game/engine catalog:** owns actual cartridge/activity compatibility and
   trusted local program resolution. The same validator serves local sessions,
   same-PC lobbies, LAN, Direct IP and Internet.
2. **recomp-ui:** displays compatible rooms and each seat's cartridge, available
   devices and missing local requirements. It distinguishes the selected game
   from the shared compatibility family. Host changes invalidate ready state.
3. **recomp-net and server:** add an explicitly versioned family/profile and
   manifest-digest negotiation path for opted-in sessions. Update discovery,
   join, capability updates, ready/start, rematch and LAN records together.
   Preserve real game identity and legacy strict matching; do not spoof every
   game's name/version into one common string to bypass checks. The server can
   apply published admission rules; it does not interpret SIO/RFU commands.
4. **Preflight barrier:** every peer resolves all required programs/images,
   checks the catalog and complete manifest, and acknowledges its canonical
   digest. Refuse unknown schemas, unsupported media, missing revisions or
   mismatched simulation builds. A legacy peer cannot silently join a mixed
   roster. Keep large manifests out of the current bounded match-caps field;
   define a bounded/versioned transfer if that field is insufficient.
5. **Simulation/startup:** initialize each machine with its own immutable ROM,
   execution bindings, mutable device/mod state and owner-contributed save/RTC.
   Retain the whole-session snapshot/digest barrier, rollback and agreed save
   checkpoint handling. Controller rows still contain inputs only. A roster
   change requires a new session epoch and agreement, not a mid-frame hot swap.

The manifest/catalog identity must also be part of recovery archives and rematch
validation. Never restore an Emerald save into the FireRed seat after a roster
reorder or silently fall back to another game's image on load.

## Implementation order

1. Add a trusted per-program registry and two tiny native fixtures with the
   same guest addresses but different code. Namespace generated symbols and
   direct calls; bind each program's dispatch tables, RAM hooks, ROM overrides,
   deterministic mod state and caches. BIOS can remain shared. Unknown PCs in
   a selected cartridge must fail closed rather than dispatch another title.
   Restore those bindings transactionally; qualify native slicing so the
   existing callback fallback does not make mixed sessions unnecessarily slow.
2. Prove two different cartridges **locally**, then replay across snapshot/load.
   FireRed ↔ LeafGreen is a useful first pair, followed by Emerald ↔ FireRed,
   with cable then wireless Union Room contact. Use actual game eligibility
   requirements and preserve the already-qualified same-game cases.
3. Implement the shared compatibility catalog, per-seat selector and manifest
   agreement across local/LAN/online paths. Keep two-player admission. Validate
   correct pairs and clear rejection of missing images, disallowed media,
   mismatched program builds, roster changes and legacy peers.
4. Add two-player mixed-cartridge delay-sync and rollback using the entire
   session as today. A short connected interaction and replay/hash agreement
   prove the architecture; broader trade/save correctness is a separate claim.
5. Expand cable to four and RFU associations to five only after startup,
   transport and UI support it. Treat larger discovery populations, late joins
   and any eventual forty-player product as separate protocol/performance
   milestones. Single-Pak multiboot remains a separate boot-source feature.
