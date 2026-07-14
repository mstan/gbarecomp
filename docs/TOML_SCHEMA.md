# TOML Schema — gba_recompile per-binary config

Status: draft v1 (2026-05-24).

The `gba_recompile` tool consumes one TOML file per binary
(`--config <path>`). The file supplements the function-finder's
automated walk: manual function entries are deduplicated against
discovered ones, and data ranges hard-exclude byte ranges from
being decoded as code.

## Power-user contract

Entries are **not validated for correctness.** A wrong address in
`[[extra_func]]` will generate junk code; a too-narrow `[[data_range]]`
will let the finder walk into literal pools. That's the cost of the
escape hatch — the user owns the consequences.

The tool rejects only entries that **structurally contradict each
other** (same address declared both `extra_func` and `exclude_func`,
overlapping `data_range` + `extra_func`, mode mismatch between
finder and manual). Everything else is the user's call.

## False-positive policy

This is the load-bearing rule that shapes the schema:

> Missing a function is a **miss** — surfaces as a `runtime_dispatch_miss`
> at runtime, points at the exact PC to add. Loud, addressable, fixable.
>
> Decoding data as code is a **false positive** — generates junk C, may
> silently dispatch into the junk, corrupts state, masks real bugs.
> Bypasses the L2 oracle's diff. Catastrophic.

Misses are cheap; false positives are expensive. The finder leans
conservative; the TOML closes the gap.

## File structure

```toml
[program]
name         = "GBA BIOS"
id           = "gba_bios"
load_address = 0x00000000
size         = 0x00004000
entry_pc     = 0x00000000
speculative_literal_harvest = false # optional; default true
codegen_shards = 16                 # optional; default 1

[identity]
sha1 = "300c20df6731a33952ded8c436f7f186d25d3492"   # required, abort on mismatch
md5  = "a860e8c0b6d573d191e4ec7db1b1e4f6"           # optional cross-check

[[extra_func]]
addr = 0x00000000
mode = "arm"
name = "reset_vector"           # optional; default: afunc_XXXXXXXX / tfunc_XXXXXXXX
note = "BIOS reset entry"       # optional documentation

[[extra_func]]
addr = 0x00000B6C
mode = "thumb"
resume = true                    # optional interior exception-return alias
note = "IRQ return into a containing function"

[[resume_range]]
start = 0x08001000               # real host entry
end   = 0x08001020               # exclusive, aligned end
mode  = "thumb"
note  = "Observed interruptible hot function"

[[data_range]]
start = 0x000001A0
end   = 0x000001C0              # [start, end) — exclusive upper bound
note  = "Nintendo logo pattern bytes"

[[code_copy]]
runtime_start = 0x030056F0
source_start  = 0x080B197C
size          = 0x00001280
name          = "iwram_funcs"    # optional documentation
note          = "ROM code copied to IWRAM before execution"

[[jump_table]]
addr         = 0x00000168
stride       = 4
count        = 43
format       = "abs32"           # abs32 | abs16 | pcrel_thumb | pcrel_arm
entries_mode = "arm"
name         = "swi_dispatch_table"
note         = "SWI 0x00..0x2A handlers"

[[exclude_func]]
addr   = 0x080FFE40
reason = "literal pool walked as code by finder pre-vX.Y"
```

## Section reference

### `[program]` (required)

| key            | type    | meaning |
|----------------|---------|---------|
| `name`         | string  | Human-readable label, appears in log output. |
| `id`           | string  | Short slug. Used in output filenames. |
| `load_address` | int hex | Where the binary's first byte maps in CPU memory. |
| `size`         | int hex | Byte size of the binary (must match the file on disk). |
| `entry_pc`     | int hex | Initial PC. Seeded as `[[extra_func]]` automatically; declaring it again is a redundant no-op. |
| `speculative_literal_harvest` | bool | Optional, default `true`. Harvest plausible callback pointers from PC-relative literal loads. Set `false` for evidence-driven strict-static corpora where runtime-observed `[[extra_func]]` roots are preferred over speculative reachability. Direct branch discovery is unchanged. |
| `codegen_shards` | int | Optional, default `1`, range 1..256. Deterministically partitions emitted guest functions into `recompiled_NNN.cpp` translation units. Stable address hashing and write-if-changed output keep unrelated shards untouched when a reviewed seed is added. |

### `[identity]` (required)

| key    | type   | meaning |
|--------|--------|---------|
| `sha1` | string | Required. Recompiler computes the binary's SHA-1 and aborts with a diagnostic if it disagrees. Anchors the TOML to a specific binary revision. |
| `md5`  | string | Optional cross-check. Same hard-abort behavior on mismatch. |

### `[[extra_func]]` (zero or more)

Hand-declared function entry points the finder might miss
(indirect dispatch targets, jump-table-only entries, gaps in finder
coverage).

| key           | type    | required | meaning |
|---------------|---------|----------|---------|
| `addr`        | int hex | yes      | First byte of the function, or an interior resume address when `resume = true`. |
| `source_addr` | int hex | no       | Immutable ROM backing address for an observed relocated RAM entry. The source/runtime bias propagates through direct CFG edges, including placements that overlap a fixed `code_copy` range. |
| `mode`        | string  | yes      | `"arm"` or `"thumb"`. Must match the actual instruction encoding at `addr`. |
| `resume`      | bool    | no       | Default `false`. Mark an interior IRQ/SWI return PC so it is rolled into its containing function and emitted as a thin resume alias. |
| `name`        | string  | no       | Function name in generated C. Default: `afunc_AAAAAAAA` (ARM) or `tfunc_AAAAAAAA` (THUMB). |
| `note`        | string  | no       | Free-form documentation. Ignored by the recompiler. |

Use `resume = true` only after confirming that `addr` is an instruction inside
an already rooted function. The alias wrapper records the requested guest PC and
enters the containing generated function, whose resume prologue jumps to that
instruction. If no compatible containing function is found, the candidate stays
a standalone function so the gap remains visible instead of being silently
attached to the wrong host.

### `[[resume_range]]` (zero or more)

A bounded declaration for a reviewed function that has produced asynchronous
IRQ returns at multiple interior instructions. `start` is the real function
entry and `end` is its exclusive, aligned extent. The recompiler keeps the host
whole and emits resume aliases for every aligned instruction after `start`.
This avoids reactively listing one `[[extra_func]] resume = true` entry per
observed interrupt timing while remaining narrower than a whole-program policy.
Ranges are capped at 0x1000 bytes, must remain inside the program image or a
declared `[[code_copy]]` runtime span, and may not overlap `[[data_range]]` or
contain an `[[exclude_func]]` address.

**Deduplication against the finder:**

| Finder finds    | TOML declares     | Result |
|-----------------|-------------------|--------|
| `0x100 thumb`   | `0x100 thumb`     | One entry. Manual name (and `note`) win. |
| `0x100 thumb`   | (nothing)         | One entry from finder. |
| (nothing)       | `0x100 thumb`     | One entry from manual. |
| `0x100 thumb`   | `0x100 arm`       | **Error** — mode mismatch. Hand-resolve. |

### `[[data_range]]` (zero or more)

Byte ranges the finder must **not** decode as code. Hard exclusion:
the finder refuses to walk into the range from any direction.

**If control flow lands in a `data_range`, the recompile is a hard
error.** Either the range is wrong (remove or shrink it) or the
finder is following a path it shouldn't (fix the finder, or add an
`[[exclude_func]]` for the bogus walk origin). One is wrong. The
error message names both the data range and the entering branch's
PC so the contradiction is unambiguous:

```
ERROR: control flow enters data_range
  data_range:    [0x080001A0, 0x080001C0)   "literal pool after thumb_init"
  entering from: B 0x080001A4 at PC=0x08000180 (afunc_08000160)
Either:
  - shrink/remove the data_range covering 0x080001A4, or
  - mark afunc_08000160 with [[exclude_func]] if it's a false-positive function.
```

The forcing function is intentional. Silent ignore (downgrade to
miss) lets stale TOML rot.

| key     | type    | required | meaning |
|---------|---------|----------|---------|
| `start` | int hex | yes      | First excluded byte. |
| `end`   | int hex | yes      | One past last excluded byte (`[start, end)`). |
| `note`  | string  | no       | Free-form documentation. |

**Structural validation:**

- `start < end`.
- No overlap with any `[[extra_func]]` address.
- No overlap with any `[[jump_table]]`'s table bytes.
- Ranges may overlap each other (idempotent union).

### `[[code_copy]]` (zero or more)

Runtime code-copy mappings. Some GBA binaries copy executable bytes
from ROM into RAM and branch to the RAM address. This section tells
the finder how to decode a runtime address by reading bytes from its
ROM source address.

`[[code_copy]]` does **not** create function entries by itself. Add
`[[extra_func]]` entries for known runtime entry points, or let the
finder discover branches into the copied range. When decoding a
function in the copied range, generated code keeps the runtime PC
addresses while reading opcodes from the source bytes.

| key             | type    | required | meaning |
|-----------------|---------|----------|---------|
| `runtime_start` | int hex | yes      | First executable address after the copy completes. |
| `source_start`  | int hex | yes      | First source byte in the original binary. |
| `size`          | int hex | yes      | Number of copied bytes. |
| `name`          | string  | no       | Mapping label (documentation). |
| `note`          | string  | no       | Free-form documentation. |

### `[[jump_table]]` (zero or more)

Annotated dispatch tables. The finder reads `count` entries of
`stride` bytes starting at `addr`, decodes each entry according to
`format`, and registers each decoded target as an `extra_func`
equivalent.

The bytes of the table itself are **automatically excluded from
code decoding** (no need to also declare a `[[data_range]]`
covering them).

| key            | type    | required | meaning |
|----------------|---------|----------|---------|
| `addr`         | int hex | yes      | First byte of the table. |
| `stride`       | int     | yes      | Bytes per entry. Typical: 4. |
| `count`        | int     | yes      | Number of entries. |
| `format`       | string  | yes      | One of `abs32`, `abs16`, `pcrel_thumb`, `pcrel_arm`. |
| `entries_mode` | string  | yes      | `"arm"`, `"thumb"`, or `"auto"`. See below. |
| `name`         | string  | no       | Table label (documentation). |
| `note`         | string  | no       | Free-form. |

Format semantics:

- `abs32` — `uint32_t` per entry; value is the target address.
- `abs16` — `uint16_t` per entry; value is the target address. Rare on GBA outside small ranges.
- `pcrel_thumb` — `int32_t` per entry; target = `entry_address + value`, decoded as THUMB.
- `pcrel_arm` — `int32_t` per entry; target = `entry_address + value`, decoded as ARM.

`entries_mode` semantics:

- `"arm"` / `"thumb"` — every entry's target is decoded in the
  declared mode. The low bit of the entry value (where applicable)
  is **not** consulted.
- `"auto"` — per-entry mode is decoded from bit 0 of the entry
  value: bit 0 set → THUMB, bit 0 clear → ARM. The bit is then
  cleared to recover the actual target address.

`"auto"` exists because the GBA's ARM/THUMB interworking convention
(`BX Rn` / `BLX Rn`) encodes mode in bit 0 of the target register,
so compiler-emitted jump tables fed to `BX` are typically
mixed-mode: one table contains entries pointing into both ARM and
THUMB code. Forcing a single `entries_mode` for the whole table
would mis-decode every entry on the other side.

Mode-keyword combinations:

| `format`        | valid `entries_mode`              |
|-----------------|-----------------------------------|
| `abs32`         | `"arm"`, `"thumb"`, `"auto"`      |
| `abs16`         | `"arm"`, `"thumb"`                |
| `pcrel_arm`     | `"arm"` (must match format)       |
| `pcrel_thumb`   | `"thumb"` (must match format)     |

`abs16` does not support `"auto"`: 16-bit pointers don't span an
interworking address space on the GBA.

The pcrel formats have mode baked into the keyword, so
`entries_mode` must match — any other value is a structural error.
This redundancy is intentional: the schema reader doesn't have to
disambiguate, and the TOML stays self-documenting.

### `[[exclude_func]]` (zero or more)

Escape valve: if the finder produces a false-positive entry the
TOML author can't predict structurally (e.g., a corrupted heuristic
walk), name the address here to remove it from the dispatch table.

| key      | type    | required | meaning |
|----------|---------|----------|---------|
| `addr`   | int hex | yes      | The false-positive entry to remove. |
| `reason` | string  | yes      | Required — forces the author to articulate why. |

`[[exclude_func]]` **takes precedence over `[[extra_func]]`** when
they conflict; declaring both at the same address is a hard error
(contradictory intent).

## Precedence and conflict resolution

In order of evaluation:

1. **Identity check** — SHA-1 (and optional MD5) verified against the binary. Mismatch → abort.
2. **Structural validation** — overlapping `extra_func` + `data_range`, mode disagreements within the TOML, `exclude_func` colliding with `extra_func`. Any → abort with diagnostic.
3. **Data ranges** registered. Finder will refuse to walk into them.
4. **Code-copy ranges** registered. Finder can decode runtime RAM addresses from their ROM source bytes.
5. **Jump tables** read, expanded to per-target `extra_func` equivalents.
6. **`extra_func` entries** registered (including the entry from `[program].entry_pc`).
7. **Finder walks** from the union of seeds. Every walked target compared against:
   - existing `extra_func` registry → dedupe (mode mismatch → error)
   - `data_range` → walk stops at boundary
8. **`exclude_func`** entries applied last — remove from final dispatch table.
9. **Summary printed** (see below).

## Summary output

After every `gba_recompile` run with a config:

```
[gba_recompile config: F:\Projects\gbarecomp\gbarecomp\bios\gba_bios.toml]
  identity sha1:           300c20df... (verified)
  extra_func entries:      54
  data_range entries:      12
  code_copy entries:       1
  jump_table entries:      1   (expanded to 43 extra_func equivalents)
  exclude_func entries:    0

[gba_recompile discovery summary]
  discovered_by_walk:      37     finder-only
  redundant_manual:        37     in both — TOML carries documentation value
  manual_seeds_only:       60     would be lost without TOML
  finder_rejected_walks:   2      hit a data_range boundary
  code_copies:             1
  excluded:                0
  TOTAL emitted:           97
```

The metric of finder quality is the trend of `manual_seeds_only`
downward over time, with `redundant_manual` rising proportionally
(or staying flat as the TOML grows for new use cases). `finder_rejected_walks`
counts how often a `data_range` saved us from a false positive.

## Runtime video settings

The runtime also consumes an optional `[video]` table. These keys do not affect
static discovery or generated code:

```toml
[video]
screen = "frontlit"
view_width = 240
```

`view_width` is the total logical output width in GBA pixels. Height remains
160. `240` is faithful and is the universal default. Values above 240 are
honored only when the game runner advertises an equal-or-larger
`RunOptions::max_view_width`; otherwise the runtime clamps to 240. Command-line
`--view-width` and environment `GBARECOMP_VIEW_WIDTH` override TOML in that
order. Legacy `widescreen=N` inputs map to `240 + 2*N` immediately and are kept
only for compatibility.

## Hash-anchored

Every TOML is bound to a specific binary by `[identity].sha1`. If
the binary changes (revision, region, patch), a new TOML is
authored. Mixing TOMLs across revisions is a hard error, not a
warning.

## What this schema deliberately does NOT support

- **`if`-conditional entries** (per-region, per-revision dynamic) —
  one TOML per binary. Duplicate files share via include or a
  generator script, not in-file conditionals.
- **Wildcard / pattern matches** — every entry is a single address
  or a closed range. No globs.
- **Per-entry attributes for the runtime** — calling convention,
  cycle cost, side effects, etc. Those belong in code, not config.
- **Comment fields that affect behavior** — `note` and `reason` are
  for humans only. Removing them never changes recompiler output.

## Convention compatibility

Naming follows the sibling-recompiler convention:

- `[[extra_func]]` — matches `nesrecomp`, `segagenesisrecomp`, `snesrecomp`.
- `[[data_range]]` — matches `nesrecomp` (`data_region`) and
  `gbcrecomp` (`[[data_region]]`). We use `data_range` for the
  byte-range semantics; `data_region` in the sibling projects is a
  superset that also names the region.
- `[[jump_table]]` — matches `segagenesisrecomp`.
- `[[exclude_func]]` — new (segagenesisrecomp uses `blacklist`; this
  name is clearer).

A future port of the TOML schema across recompilers should be
straightforward.
