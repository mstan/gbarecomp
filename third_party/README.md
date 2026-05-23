# Third-party

This directory holds external libraries we vendor. Each subdirectory
must:

1. Carry its own upstream `LICENSE` (or equivalent) file unmodified.
2. Have a `README.md` at the same level naming the upstream, the
   pinned version / commit, and the reason we link it.
3. Be referenced only from `CMakeLists.txt`. No third-party code is
   moved into `src/`.

## Planned imports (none vendored yet)

| Library | Use | License | Where |
|---------|-----|---------|-------|
| `fmt` | text formatting in tools | MIT | likely `third_party/fmt/` |
| `toml++` or `tomlplusplus` | `game.toml` parsing | MIT | likely `third_party/toml++/` |
| `cxxopts` | CLI parsing in tools | MIT | likely `third_party/cxxopts/` |
| `nlohmann/json` | debug TCP responses | MIT | likely `third_party/json/` |

## Emulator dependencies (oracle target only)

| Library | Use | License | Linked from |
|---------|-----|---------|-------------|
| `mGBA` | oracle bridge process | MPL-2.0 | **oracle binary only** — never linked into native |
| `NanoBoyAdvance` | secondary oracle | GPL-3.0 | **oracle binary only** — never linked into native |

The native build must have zero copyleft emulator dependencies.
Oracle binaries are a separate target and may carry their own
license obligations; document them in `oracle/README.md` when that
folder appears.

## Game-specific references (NOT vendored here)

These are not third-party in the "we vendor them" sense — they
live in their game repo's `tools/import_*` and are read at build
time to extract symbols. The decomp source itself does not enter
this repo:

- `zeldaret/tmc` — symbol / boundary / annotation source for
  `MinishCapRecomp`. License is decomp-specific; we read symbols
  via the importer tool, not by linking.
