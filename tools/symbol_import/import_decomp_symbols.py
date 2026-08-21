#!/usr/bin/env python3
"""import_decomp_symbols.py — turn a decompilation's build metadata into the
symbol overlay that gba_recompile consumes.

The recompiler names every generated C function after its seed
(`FunctionSeed.name`; see src/recompile/function_finder.h). Without seeds the
finder synthesises `tfunc_08XXXXXX` / `afunc_XXXXXXXX`, so a recompiled game is
readable only as raw addresses. This tool harvests the real names — and the
authoritative code/data split — from a byte-matching decomp build and emits
them in the two forms the recompiler already reads:

  imported_symbols.tsv       `addr \\t mode \\t name`, passed as --symbols.
                             Each row becomes a named function seed.
  <id>_symbols.toml          a CONFIG OVERLAY, passed as a second --config:
                             [identity] + [[data_range]] (+ [[code_copy]]).

Plus two artifacts for humans and other tools:

  function_boundaries.tsv    exact start/end from st_size.
  imported_data_symbols.tsv  `addr \\t region \\t size \\t name`, passed as
                             --data-symbols so the runtime debugger can name
                             memory operands, not just PCs.

The overlay deliberately carries NO [program] table. A game's game.toml is
hand-authored, carries reviewed evidence in its notes, and is never written or
replaced by tooling; the overlay composes on top of it and loses every naming
conflict (see gba_recompile's load_config_overlay).

Inputs, and why each exists
---------------------------
--syms      `readelf -sW <elf>`. readelf rather than nm because raw st_value
            keeps bit0 set for THUMB functions (nm masks it) — that bit IS the
            mode. st_size gives exact extents.

--sections  `readelf -SW <elf>`. Non-executable PROGBITS sections in ROM are
            data. This is the whole story for a decomp that links .text and
            .rodata into separate output sections (every pret Gen3 decomp).

--map       `ld -Map <map>`. Needed when a decomp's linker script merges code
            and data into ONE loadable output section, which makes the section
            table useless: jellees/mksc emits a single `.text` at 0x08000000
            spanning the entire 4 MB cartridge, so `--sections` yields zero
            data ranges while the map still lists all 621 `.rodata*` input
            sections with exact addresses and sizes.

Data-range sources (--data-source, repeatable; default `auto`)
-------------------------------------------------------------
  sections         non-executable PROGBITS ROM sections. Coarse, exact.
  map              non-`.text` input sections in ROM from the link map.
                   Coarse, exact, survives single-output-section layouts.
  mapping-symbols  ARM ELF mapping symbols ($a/$t = code, $d = data) delimit
                   code and data at instruction granularity INSIDE a section.
                   This is much finer than the others: it also marks every
                   THUMB literal pool. Powerful, but it turns thousands of
                   in-function pools into authoritative ranges, which is a
                   behavioural change for the finder — opt in deliberately
                   and re-validate, do not switch a shipped game to it
                   casually.
  auto             `sections` if it produced any ranges, else `map`, else
                   nothing. Never `mapping-symbols`.

Usage:
  import_decomp_symbols.py --id AMKE --name "Mario Kart: Super Circuit (USA)" \\
      --syms symbols/mksc_readelf_syms.txt \\
      --sections symbols/mksc_readelf_sections.txt \\
      --map symbols/mksc.map \\
      --rom roms/mario_kart_super_circuit_usa.gba \\
      --out symbols
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import re
import sys

# readelf -sW row:
#   "  539: 0800080d    56 FUNC    LOCAL  DEFAULT    3 Name"
SYM_RE = re.compile(
    r"^\s*\d+:\s+([0-9A-Fa-f]+)\s+(\d+)\s+(\S+)\s+(\S+)\s+\S+\s+(\S+)\s+(.+?)\s*$"
)
# readelf -SW row:
#   "  [ 3] .text  PROGBITS  08000000 001000 400000 00  AX  0   0  4"
SEC_RE = re.compile(
    r"^\s*\[\s*\d+\]\s+(\S+)\s+(\S+)\s+([0-9A-Fa-f]+)\s+[0-9A-Fa-f]+\s+"
    r"([0-9A-Fa-f]+)\s+[0-9A-Fa-f]+\s+([A-Za-z]*)"
)
# ld -Map input-section row, both forms. ld wraps the name onto its own line
# once it reaches 14 characters, so `.ARM.attributes` and `.rodata.wave_167`
# arrive split across two lines:
#   " .rodata        0x08062774    0x7b8e8 asm/rodata08062774.o"
#   " .rodata.wave_167"
#   "                0x081234a0       0x20 sound/wave/wave_167.o"
MAP_FULL_RE = re.compile(
    r"^\s(\.\S+)\s+0x([0-9A-Fa-f]+)\s+0x([0-9A-Fa-f]+)\s+(\S.*)$"
)
MAP_NAME_ONLY_RE = re.compile(r"^\s(\.\S+)\s*$")
MAP_ADDR_ONLY_RE = re.compile(
    r"^\s+0x([0-9A-Fa-f]+)\s+0x([0-9A-Fa-f]+)\s+(\S.*)$"
)

# ARM ELF mapping symbols, and the assembler's numeric local labels
# (`_08001AEC`). Neither is a real program symbol.
MAPPING_SYM_RE = re.compile(r"^\$[atd](\.\w+)?$")
LOCAL_LABEL_RE = re.compile(r"^_[0-9A-Fa-f]{6,8}$")
# `sub_8001ADC` / `func_08001ADC`: address-derived placeholder names. They mark
# a real function boundary but carry no meaning beyond the address.
PLACEHOLDER_RE = re.compile(r"^(?:sub|func|nullsub)_[0-9A-Fa-f]{6,8}$", re.I)

# GBA address map. Used to tag data symbols and to decide what "in ROM" means.
REGIONS = [
    ("bios",    0x00000000, 0x00003FFF),
    ("ewram",   0x02000000, 0x0203FFFF),
    ("iwram",   0x03000000, 0x03007FFF),
    ("io",      0x04000000, 0x040003FE),
    ("palette", 0x05000000, 0x050003FF),
    ("vram",    0x06000000, 0x06017FFF),
    ("oam",     0x07000000, 0x070003FF),
    ("rom",     0x08000000, 0x09FFFFFF),
    ("sram",    0x0E000000, 0x0E00FFFF),
]


def region_for(addr: int) -> str:
    for name, lo, hi in REGIONS:
        if lo <= addr <= hi:
            return name
    return "other"


def coalesce(ranges):
    """Merge overlapping/adjacent [start, end) pairs. Input may be unsorted."""
    out = []
    for start, end in sorted(ranges):
        if end <= start:
            continue
        if out and start <= out[-1][1]:
            out[-1] = (out[-1][0], max(out[-1][1], end))
        else:
            out.append((start, end))
    return out


def in_ranges(addr: int, ranges) -> bool:
    for start, end in ranges:
        if start <= addr < end:
            return True
    return False


# ── parsing ──────────────────────────────────────────────────────────


def parse_symbols(path: pathlib.Path, rom_lo: int, rom_hi: int):
    """Return (funcs, data, mapping, by_name).

    funcs   {addr: (mode, size, name)}       STT_FUNC, addr has bit0 cleared
    data    [(addr, size, name)]             STT_OBJECT, plus ABS symbols that
                                             land in a real GBA region
    mapping [(addr, kind)]                   kind in {"code", "data"}
    by_name {name: (raw_value, size)}        for --code-copy-pair resolution
    """
    funcs: dict[int, tuple[str, int, str]] = {}
    data: list[tuple[int, int, str]] = []
    mapping: list[tuple[int, str]] = []
    by_name: dict[str, tuple[int, int]] = {}

    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = SYM_RE.match(line)
        if not m:
            continue
        value_s, size_s, typ, _bind, ndx, name = m.groups()
        name = name.strip()
        if not name or ndx == "UND":
            continue
        value, size = int(value_s, 16), int(size_s)

        if MAPPING_SYM_RE.match(name):
            # $a/$t open a code region, $d opens a data region.
            mapping.append((value, "data" if name[1] == "d" else "code"))
            continue
        if LOCAL_LABEL_RE.match(name):
            continue

        by_name.setdefault(name, (value, size))

        if typ == "FUNC":
            # bit0 of st_value is the THUMB flag, not part of the address.
            funcs.setdefault(value & ~1,
                             ("thumb" if (value & 1) else "arm", size, name))
        elif typ == "OBJECT":
            data.append((value, size, name))
        elif typ == "NOTYPE" and ndx == "ABS":
            # A decomp that is still partly disassembly declares its known RAM
            # globals as absolute symbols (mksc feeds ~120 of them to the
            # linker via --just-symbols=symbols.txt). Those are exactly the
            # names that make a watchpoint or trace dump readable.
            #
            # Everything else in .symtab's ABS bucket is noise, and there is a
            # lot of it: `.include`d assembler equates are re-emitted once per
            # object file (mksc has 149k ABS rows for ~120 distinct globals),
            # and object-file basenames appear as zero-valued ABS symbols. So
            # require a plausible RAM/ROM/IO address — which also excludes the
            # zero-valued equates, since a decomp symbol never legitimately
            # names a BIOS address.
            if (region_for(value) not in ("other", "bios")
                    and not name.endswith(".o")):
                data.append((value, size, name))

    return funcs, data, mapping, by_name


def parse_sections(path: pathlib.Path, rom_lo: int, rom_hi: int):
    """Return (code_ranges, data_ranges) from readelf -SW."""
    code, data = [], []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = SEC_RE.match(line)
        if not m:
            continue
        _name, sectype, addr_s, size_s, flags = m.groups()
        if sectype != "PROGBITS":
            continue
        addr, size = int(addr_s, 16), int(size_s, 16)
        if size == 0 or not (rom_lo <= addr <= rom_hi):
            continue
        (code if "X" in flags else data).append((addr, addr + size))
    return code, data


def parse_map(path: pathlib.Path, rom_lo: int, rom_hi: int):
    """Return data ranges from an ld -Map link map.

    Everything that is not a `.text*` input section, has non-zero size and
    lands in ROM is data: .rodata, .rodata.<blob> (bin2s output), .data.
    """
    out = []
    pending_name = None
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        name = addr_s = size_s = None
        m = MAP_FULL_RE.match(line)
        if m:
            name, addr_s, size_s, _owner = m.groups()
            pending_name = None
        else:
            m = MAP_ADDR_ONLY_RE.match(line)
            if m and pending_name is not None:
                name = pending_name
                addr_s, size_s, _owner = m.groups()
                pending_name = None
            else:
                m = MAP_NAME_ONLY_RE.match(line)
                pending_name = m.group(1) if m else pending_name
                continue
        addr, size = int(addr_s, 16), int(size_s, 16)
        if size == 0 or not (rom_lo <= addr <= rom_hi):
            continue
        if name == ".text" or name.startswith(".text."):
            continue
        out.append((addr, addr + size))
    return out


def ranges_from_mapping_symbols(mapping, rom_lo: int, rom_hi: int):
    """Return data ranges delimited by ARM $d / $a / $t mapping symbols.

    A $d region runs until the next mapping symbol of any kind (or the end of
    ROM for the final one).
    """
    pts = sorted(set(mapping))
    out = []
    for i, (addr, kind) in enumerate(pts):
        if kind != "data" or not (rom_lo <= addr <= rom_hi):
            continue
        end = pts[i + 1][0] if i + 1 < len(pts) else rom_hi + 1
        out.append((addr, min(end, rom_hi + 1)))
    return out


def parse_code_copy_pairs(specs, by_name):
    """Resolve --code-copy-pair BUF=SRC[:mode] against the symbol table.

    Code that is copied to RAM at runtime and executed there is invisible to
    static discovery: the destination holds no bytes in the ROM image. A decomp
    names both ends (an IWRAM buffer object and the ROM source function), so
    the pair is enough to emit a [[code_copy]] plus an entry seed.
    """
    out = []
    for spec in specs or []:
        mode = "arm"
        body = spec
        if ":" in body:
            body, mode = body.rsplit(":", 1)
        if "=" not in body:
            raise SystemExit(f"--code-copy-pair must be BUF=SRC[:mode]: {spec}")
        buf_name, src_name = body.split("=", 1)
        buf = by_name.get(buf_name)
        src = by_name.get(src_name)
        if not buf or not src:
            missing = buf_name if not buf else src_name
            print(f"warn: --code-copy-pair {spec}: symbol {missing} not found; "
                  f"skipped", file=sys.stderr)
            continue
        buf_addr, buf_size = buf
        if buf_size == 0:
            print(f"warn: --code-copy-pair {spec}: {buf_name} has size 0; "
                  f"skipped (no extent to copy)", file=sys.stderr)
            continue
        out.append((buf_addr, src[0] & ~1, buf_size, mode, buf_name, src_name))
    return out


# ── emit ─────────────────────────────────────────────────────────────


def write_overlay(path: pathlib.Path, args, rom_sha1, rom_size,
                  data_ranges, range_sources, code_copies):
    with path.open("w", encoding="utf-8", newline="\n") as fh:
        fh.write(
            "# AUTO-GENERATED by gbarecomp/tools/symbol_import/"
            "import_decomp_symbols.py.\n"
            "# DO NOT EDIT — rerun the importer instead.\n"
            "#\n"
            "# This is a CONFIG OVERLAY, not a program config: pass it as a\n"
            "# second --config after the game's hand-authored game.toml.\n"
            "# It deliberately declares no [program] table, and it loses every\n"
            "# conflict with the base config by design.\n"
            f"#\n# program : {args.name or args.id}\n"
            f"# sources : {', '.join(range_sources) or 'none'}\n")
        if rom_sha1:
            fh.write(
                "\n# The base config's [identity].sha1 must match this. The\n"
                "# overlay's addresses are only meaningful for the exact ROM\n"
                "# the decomp reproduced, so a mismatch is a hard error.\n"
                "[identity]\n"
                f'sha1 = "{rom_sha1}"\n')
            if rom_size:
                fh.write(f"# rom size = 0x{rom_size:08X}\n")
        for rt, src, size, mode, buf_name, src_name in code_copies:
            fh.write("\n[[code_copy]]\n"
                     f"runtime_start = 0x{rt:08X}\n"
                     f"source_start = 0x{src:08X}\n"
                     f"size = 0x{size:X}\n"
                     f'name = "{buf_name}"\n'
                     f'note = "decomp: {src_name} is copied here at runtime"\n')
            fh.write("\n[[extra_func]]\n"
                     f"addr = 0x{rt:08X}\n"
                     f'mode = "{mode}"\n'
                     f'name = "{buf_name.lower()}_entry"\n'
                     f'note = "entry of the {buf_name} code_copy"\n')
        for start, end in data_ranges:
            fh.write("\n[[data_range]]\n"
                     f"start = 0x{start:08X}\n"
                     f"end = 0x{end:08X}\n"
                     f'note = "non-executable ({"/".join(range_sources)})"\n')


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--id", required=True,
                    help="program id, used for the overlay filename (e.g. AMKE)")
    ap.add_argument("--name", default="",
                    help="human-readable program name, for the file header")
    ap.add_argument("--syms", required=True, type=pathlib.Path,
                    help="readelf -sW output")
    ap.add_argument("--sections", type=pathlib.Path,
                    help="readelf -SW output")
    ap.add_argument("--map", dest="map_path", type=pathlib.Path,
                    help="ld -Map output")
    ap.add_argument("--rom", type=pathlib.Path,
                    help="ROM image, for the identity sha1")
    ap.add_argument("--out", required=True, type=pathlib.Path,
                    help="output directory")
    ap.add_argument("--data-source", action="append", default=[],
                    choices=["auto", "sections", "map", "mapping-symbols"],
                    help="where data ranges come from (repeatable; "
                         "default auto)")
    ap.add_argument("--code-copy-pair", action="append", default=[],
                    metavar="BUF=SRC[:mode]",
                    help="runtime code copy, by symbol name (repeatable)")
    ap.add_argument("--rom-base", default="0x08000000")
    ap.add_argument("--rom-end", default="0x09FFFFFF")
    ap.add_argument("--strip-placeholder-names", action="store_true",
                    help="drop address-derived names (sub_8001ADC) and let the "
                         "finder synthesise tfunc_/afunc_ instead; the seed "
                         "addresses are still emitted")
    ap.add_argument("--dry-run", action="store_true",
                    help="report only; write nothing")
    args = ap.parse_args()

    rom_lo = int(args.rom_base, 16)
    rom_hi = int(args.rom_end, 16)

    funcs, data, mapping, by_name = parse_symbols(args.syms, rom_lo, rom_hi)
    print(f"==> {args.syms}: {len(funcs)} FUNC, {len(data)} data, "
          f"{len(mapping)} mapping symbols")

    # ── data ranges ──────────────────────────────────────────────────
    available = {}
    if args.sections and args.sections.exists():
        _code, sec_data = parse_sections(args.sections, rom_lo, rom_hi)
        available["sections"] = sec_data
        print(f"==> sections: {len(sec_data)} non-executable ROM range(s)")
    if args.map_path and args.map_path.exists():
        map_data = parse_map(args.map_path, rom_lo, rom_hi)
        available["map"] = map_data
        print(f"==> map: {len(map_data)} data input section(s)")
    if mapping:
        ms_data = ranges_from_mapping_symbols(mapping, rom_lo, rom_hi)
        available["mapping-symbols"] = ms_data
        print(f"==> mapping symbols: {len(ms_data)} data region(s) "
              f"(fine-grained; includes literal pools)")

    requested = args.data_source or ["auto"]
    if "auto" in requested:
        if available.get("sections"):
            requested = ["sections"]
        elif available.get("map"):
            requested = ["map"]
            print("==> auto: the section table yielded no data ranges (single "
                  "loadable output section?); using the link map")
        else:
            requested = []
            print("==> auto: no data-range source available", file=sys.stderr)

    chosen, used_sources = [], []
    for src in requested:
        rows = available.get(src)
        if not rows:
            print(f"warn: --data-source {src} requested but unavailable/empty",
                  file=sys.stderr)
            continue
        chosen.extend(rows)
        used_sources.append(src)
    data_ranges = coalesce(chosen)
    covered = sum(end - start for start, end in data_ranges)
    print(f"==> data ranges: {len(data_ranges)} coalesced from "
          f"{used_sources or ['none']} covering 0x{covered:X} bytes")

    # A function entry inside a data range is a contradiction; the range wins
    # (it came from the same build as the symbol). Report every drop — a
    # nonzero count here means one of the two inputs is being misread.
    dropped = sorted(a for a in funcs if in_ranges(a, data_ranges))
    for addr in dropped:
        print(f"warn: dropping FUNC {funcs[addr][2]} at 0x{addr:08X}: "
              f"inside a data range", file=sys.stderr)
    for addr in dropped:
        del funcs[addr]

    code_copies = parse_code_copy_pairs(args.code_copy_pair, by_name)
    for rt, src, size, mode, bn, sn in code_copies:
        print(f"==> code_copy {bn} (0x{rt:08X}) <- {sn} (0x{src:08X}) "
              f"size=0x{size:X} [{mode}]")

    rows = sorted(funcs.items())
    arm = sum(1 for _a, (mode, _s, _n) in rows if mode == "arm")
    placeholder = sum(1 for _a, (_m, _s, name) in rows
                      if PLACEHOLDER_RE.match(name))
    print(f"==> functions: {len(rows)} (arm={arm} thumb={len(rows) - arm}), "
          f"dropped-in-data={len(dropped)}")
    print(f"==> names: {len(rows) - placeholder} meaningful, "
          f"{placeholder} address-derived placeholders"
          + (" (stripped)" if args.strip_placeholder_names else ""))

    rom_sha1 = rom_size = None
    if args.rom and args.rom.exists():
        blob = args.rom.read_bytes()
        rom_sha1, rom_size = hashlib.sha1(blob).hexdigest(), len(blob)
        print(f"==> rom: sha1={rom_sha1} size=0x{rom_size:08X}")
    else:
        print("warn: no --rom; the overlay will carry no [identity] gate",
              file=sys.stderr)

    if args.dry_run:
        print("==> dry run; nothing written")
        return 0

    args.out.mkdir(parents=True, exist_ok=True)

    with (args.out / "imported_symbols.tsv").open(
            "w", encoding="utf-8", newline="\n") as fh:
        fh.write("# addr\tmode\tname  (STT_FUNC from a byte-matching decomp "
                 "ELF; blank name = let the finder synthesise one)\n")
        for addr, (mode, _size, name) in rows:
            emitted = "" if (args.strip_placeholder_names
                             and PLACEHOLDER_RE.match(name)) else name
            fh.write(f"0x{addr:08X}\t{mode}\t{emitted}\n")

    with (args.out / "function_boundaries.tsv").open(
            "w", encoding="utf-8", newline="\n") as fh:
        fh.write("# start\tend\tmode\tname  (end = start+size-1; 0 = size "
                 "unknown)\n")
        for addr, (mode, size, name) in rows:
            end = addr + size - 1 if size else 0
            fh.write(f"0x{addr:08X}\t0x{end:08X}\t{mode}\t{name}\n")

    seen_data = {}
    for addr, size, name in sorted(data):
        seen_data.setdefault(addr, (size, name))
    with (args.out / "imported_data_symbols.tsv").open(
            "w", encoding="utf-8", newline="\n") as fh:
        fh.write("# addr\tregion\tsize\tname  (STT_OBJECT, plus absolute "
                 "symbols landing in a GBA region)\n")
        for addr, (size, name) in sorted(seen_data.items()):
            fh.write(f"0x{addr:08X}\t{region_for(addr)}\t0x{size:X}\t{name}\n")

    overlay = args.out / f"{args.id}_symbols.toml"
    write_overlay(overlay, args, rom_sha1, rom_size, data_ranges,
                  used_sources, code_copies)

    print(f"==> wrote {overlay.name}, imported_symbols.tsv "
          f"({len(rows)}), imported_data_symbols.tsv ({len(seen_data)}), "
          f"function_boundaries.tsv into {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
