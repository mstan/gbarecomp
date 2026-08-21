# Symbol overlays — naming a recompilation from a decomp

A recompiled game is, by default, anonymous. The function finder synthesises
`tfunc_08XXXXXX` / `afunc_XXXXXXXX` for everything it discovers, so every hang
trace, dispatch-miss report, PC-sampler dump and TCP `symbol` query answers in
raw hex. Mario Kart: Super Circuit had 13,717 functions and 137 names.

A *symbol overlay* fixes that by importing the real names — and the real
code/data split — from a decompilation that reproduces the exact same ROM.
Nothing about execution changes: names are identifiers and debug metadata.

```
decomp build ──readelf/ld -Map──▶ import_decomp_symbols.py ──▶ symbols/
                                                                 │
                              ┌──────────────────────────────────┤
                              ▼                                  ▼
                    imported_symbols.tsv               <ID>_symbols.toml
                    imported_data_symbols.tsv          (config overlay)
                              │                                  │
                              └────────▶ gba_recompile ◀─────────┘
                                              │
                        generated/ (named C functions, symbol_map.cpp,
                                    data_symbol_map.cpp)
```

## The hard prerequisite: the decomp must reproduce YOUR ROM

A decomp's addresses are meaningful only for the binary it builds. So the gate
is the decomp's own `rom.sha1` (or equivalent) matching the `[identity].sha1`
in the game's `game.toml`. Build the decomp, check the hash, and stop if it
differs — importing "close enough" symbols silently mislabels code.

The importer records the ROM's sha1 in the overlay it writes, and
`gba_recompile` aborts if that disagrees with the base config's. The rule is
enforced by the tool, not by discipline.

## Step 1 — build the decomp and capture three files

| File | Command | Why |
|---|---|---|
| symbols | `readelf -sW <game>.elf` | names, `st_value` (bit 0 = THUMB), `st_size` |
| sections | `readelf -SW <game>.elf` | section flags: `X` = code, else data |
| link map | the build's `ld -Map` output | per-**input**-section extents |

`readelf` rather than `nm`: raw `st_value` keeps bit 0 set for THUMB
functions, and that bit *is* the mode. `nm` masks it.

The map looks redundant next to the section table, and for a pret-style decomp
(separate `.text` and `.rodata` output sections) it is. It stops being
redundant the moment a linker script merges code and data into one output
section — jellees/mksc emits a single `.text` covering the whole 4 MB
cartridge, so `readelf -SW` reports one `AX` span and yields *zero* data
ranges, while the map still lists all 621 `.rodata*` input sections with exact
addresses and sizes. Capture all three; the importer picks.

`MarioKartSuperCircuitRecomp/tools/decomp/{provision.sh,build.sh}` is a
worked example of a root-free WSL build that ends by writing these three files
into the game's `symbols/`.

## Step 2 — import

```sh
python gbarecomp/tools/symbol_import/import_decomp_symbols.py \
    --id AMKE --name "Mario Kart: Super Circuit (USA)" \
    --syms     symbols/mksc_readelf_syms.txt \
    --sections symbols/mksc_readelf_sections.txt \
    --map      symbols/mksc.map \
    --rom      roms/mario_kart_super_circuit_usa.gba \
    --out      symbols
```

Outputs, all committed (they are small, deterministic, and reviewable; only
the bulky `readelf`/map inputs are gitignored):

| File | Consumed by | Purpose |
|---|---|---|
| `imported_symbols.tsv` | `--symbols` | named function seeds → generated C function names |
| `imported_data_symbols.tsv` | `--data-symbols` | `generated/data_symbol_map.cpp`, so debug output names memory operands |
| `<ID>_symbols.toml` | second `--config` | `[identity]` gate + `[[data_range]]` from the decomp's layout |
| `function_boundaries.tsv` | nothing (yet) | exact extents, for humans and future tooling |

### Data-range sources

`--data-source` is repeatable; the default `auto` prefers `sections` and falls
back to `map`.

- **`sections`** — non-executable PROGBITS ROM sections. Coarse and exact.
  The right answer for any decomp with separate output sections.
- **`map`** — non-`.text` input sections from the link map. Same granularity,
  but survives a single-output-section layout.
- **`mapping-symbols`** — ARM ELF mapping symbols (`$a`/`$t` open code,
  `$d` opens data) delimit code and data *inside* a section, at instruction
  granularity. This is strictly more information — it also marks every THUMB
  literal pool — but it turns thousands of in-function pools into
  authoritative data ranges, which changes what the finder will walk. Treat
  switching a shipped game to it as its own measured change, not a default.

`auto` never selects `mapping-symbols`.

### What stays out of the importer

Per-game knowledge does not belong in a shared tool. Runtime code copies are
expressible generically (`--code-copy-pair BUF=SRC:mode`, resolved by symbol
name), and reviewed dispatch-miss seeds belong in the game's own `game.toml`
where a human already signed off on them.

Address-derived placeholder names (`sub_8001ADC`) are kept by default: they
still mark a real function boundary, and `gf_sub_8001ADC` is no worse than
`gf_tfunc_08001ADC` while telling you the decomp knows this function.
`--strip-placeholder-names` drops them if you prefer the synthesised form.

## Step 3 — regenerate

Add three arguments to the game's regen invocation, keeping `game.toml` first:

```
gba_recompile --rom <rom> \
    --config game.toml \
    --config symbols/<ID>_symbols.toml \
    --symbols symbols/imported_symbols.tsv \
    --data-symbols symbols/imported_data_symbols.tsv \
    --out generated --max-functions 65536
```

`--config` order matters: the first is the base and wins every conflict. See
`TOML_SCHEMA.md` "Overlay configs".

## Step 4 — validate

Names cannot change behaviour, and that is exactly what to verify:

1. Regenerate and build **without** the three new arguments on the engine
   commit you are shipping. That is the baseline.
2. Regenerate and build **with** them.
3. Both runs must agree on frame count, on `FULLY_STATIC` /
   `dispatch_misses=0`, and on the rendered frames.

Two things legitimately differ:

- **Function count usually goes up**, sometimes a lot. Decomp seeds reach
  entries the sweep never found (address-taken callbacks, table targets), and
  each opens its own call graph. MKSC went 13,717 → 20,815 discovered
  functions, with zero dispatch-table entries lost.
- **Data ranges may collide with existing config.** A hard error here is a
  real finding: reviewed evidence and the decomp's link layout disagree about
  some bytes. Resolve the specific conflict; never widen a suppression.

## What you get

Before / after, MKSC:

```
symbol_map.cpp:   13,717 entries, 137 named   →   20,815 entries, 1,586 named
data_symbol_map.cpp:            (absent)      →   284 named memory regions
```

and debug output that reads like this instead of hex:

```
  #1421 MEM_WRITE pc=0x0800A3C4 <gf_scene_state_switch_01+0x2C>
        addr=0x030016E8 <sBufferDestinations+0x0> value=0x06010000
```

Data symbols resolve strictly — only *inside* a symbol's extent — so an
address that belongs to no known symbol is reported as unnamed rather than
attributed to whatever happens to sit below it. Symbols with unknown size
(common for the absolute symbols a part-disassembled decomp declares) resolve
only within a small window from their start.
