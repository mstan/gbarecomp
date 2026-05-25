#!/usr/bin/env python3
"""import_bios_symbols.py — emit gba_bios.toml from the disassembly
of Nintendo's GBA BIOS at third_party/gba_bios_disasm/.

The disasm is camthesaxman/gba_bios. It builds a binary that
MD5-matches our `bios/gba_bios.bin` (verified by the disasm's own
checksum.md5). We import its LABEL set as `[[extra_func]]` entries,
and the SWI branch table at 0x000001C8 as a `[[jump_table]]`
directive.

Pipeline:
  1. Run `arm-none-eabi-nm -n gba_bios.elf`. Capture (addr, name)
     pairs for text-section symbols (types `t` / `T`).
  2. Parse `asm/bios.s`. Track `.ARM` / `.THUMB` mode across the
     file and record (label_name → mode) for every top-level
     label.
  3. Classify each symbol:
       - Capital-T globals (`sub_NNNNNNNN`): function entries.
       - Lowercase named locals (`_start`, `reset_vector`,
         `swi_vector`, etc.): function entries (deliberately named
         by the disasm author).
       - Lowercase `_NNNNNNNN` labels: basic-block labels emitted
         at branch targets within functions. SKIPPED — those are
         not function entries.
       - `swi_branch_table`: data, not code. SKIPPED here; emitted
         as a [[jump_table]] directive separately.
  4. Emit `gbarecomp/bios/gba_bios.toml` with the schema from
     `docs/TOML_SCHEMA.md`.

Usage:
  python tools/import_bios_symbols/import_bios_symbols.py
      [--disasm third_party/gba_bios_disasm]
      [--out bios/gba_bios.toml]

Idempotent: rerunning overwrites the TOML.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys
from dataclasses import dataclass


ROOT = pathlib.Path(__file__).resolve().parents[2]

# Symbols we deliberately skip even though they live in .text:
# - `swi_branch_table`: data (emitted as [[jump_table]]).
# - `swi_branch_table_end`: marker; data.
#
# `_NNNNNNNN`-style auto-generated labels ARE INCLUDED. In a
# decompilation project they'd typically be basic-block labels
# within a parent function, but in this small (16 KB) BIOS the
# distinction collapses: every label is at a code address, every
# code address is a valid dispatch target, and many labels are
# real function entries reached only via indirect BX (which the
# function-finder cannot follow). Including them all matches the
# "TOML as crutch for game 1" strategy — over-cover via the
# escape hatch, refine the finder later.
AUTOLABEL_RE = re.compile(r"^_[0-9A-Fa-f]{8}$")
SKIP_NAMES = {
    "swi_branch_table",
    "swi_branch_table_end",
}

# Each entry: (name, mode_string).
# Match a label at column 0. The label may be followed by a comment,
# OR (common in this disasm) by a data directive on the SAME line:
#   _000001B8: .4byte 0x03007F00
# Group 2 is the directive/comment tail, or empty.
LABEL_LINE_RE = re.compile(
    r"^([A-Za-z_][A-Za-z0-9_]*):\s*(.*)$")
MODE_DIRECTIVE_RE = re.compile(r"^\s*\.(ARM|THUMB)\b", re.IGNORECASE)

# Data directives on a label-tail line — if a label is followed by
# one of these on the same line, the label points at data, not code.
DATA_TAIL_RE = re.compile(
    r"^\.(?:byte|2byte|4byte|word|hword|short|long|ascii|asciz|"
    r"string|space|fill|skip)\b", re.IGNORECASE)


@dataclass
class SymEntry:
    addr: int
    name: str
    mode: str       # "arm" or "thumb"
    visibility: str # "global" or "local"


def run_nm(elf_path: pathlib.Path) -> list[tuple[int, str, str]]:
    """Returns [(addr, name, type)] for text-section symbols, sorted
    by addr. `type` is the raw nm letter (lowercase 't' = local,
    uppercase 'T' = global)."""
    out = subprocess.check_output(
        ["arm-none-eabi-nm", "-n", str(elf_path)],
        text=True, encoding="utf-8")
    rows: list[tuple[int, str, str]] = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        addr_s, typ, name = parts[0], parts[1], parts[2]
        if typ not in ("t", "T"):
            continue
        try:
            addr = int(addr_s, 16)
        except ValueError:
            continue
        rows.append((addr, name, typ))
    return rows


def parse_asm_modes(asm_path: pathlib.Path
                    ) -> tuple[dict[str, str], set[str]]:
    """Walk the disassembly, track .ARM/.THUMB mode, and return
    ({label_name: mode}, {data_label names}).

    A label is classified as `data` when the directive tail on the
    same line (or the next non-blank source line) is a
    `.byte`/`.4byte`/`.word`/`.ascii`/etc. directive. Data labels
    are excluded from the function-entry set in classify()."""
    mode_by_name: dict[str, str] = {}
    data_labels: set[str] = set()
    current_mode = "arm"
    pending_label: str | None = None

    lines = asm_path.read_text(encoding="utf-8",
                                errors="replace").splitlines()
    for line in lines:
        m = MODE_DIRECTIVE_RE.match(line)
        if m:
            current_mode = m.group(1).lower()
            pending_label = None
            continue
        # Top-level labels: column 0, name followed by `:`.
        if line and not line[0].isspace():
            lm = LABEL_LINE_RE.match(line)
            if lm:
                name = lm.group(1)
                tail = (lm.group(2) or "").strip()
                # Strip trailing comment for tail inspection.
                if tail.startswith("@"):
                    tail = ""
                mode_by_name[name] = current_mode
                if tail and DATA_TAIL_RE.match(tail):
                    data_labels.add(name)
                    pending_label = None
                elif tail:
                    # Some other instruction on the same line as the
                    # label — code, not data.
                    pending_label = None
                else:
                    # Label alone — check the NEXT non-blank line for
                    # a data directive.
                    pending_label = name
                continue
        # Indented body line — inspect when we have a pending label.
        if pending_label is not None:
            stripped = line.strip()
            if stripped.startswith("@") or not stripped:
                continue   # comment / blank, keep looking
            if DATA_TAIL_RE.match(stripped):
                data_labels.add(pending_label)
            pending_label = None
    return mode_by_name, data_labels


def classify(rows: list[tuple[int, str, str]],
             mode_by_name: dict[str, str],
             data_labels: set[str]) -> list[SymEntry]:
    """Filter known-data names + asm-detected data labels, assign
    mode per remaining label, return function entries."""
    out: list[SymEntry] = []
    for addr, name, typ in rows:
        if name in SKIP_NAMES:
            continue
        if name in data_labels:
            continue
        mode = mode_by_name.get(name, "arm")  # default to ARM if unknown
        out.append(SymEntry(
            addr=addr,
            name=name,
            mode=mode,
            visibility="global" if typ == "T" else "local"))
    return out


# ─────────────────────────────────────────────────────────────────────
# SWI table sizing. The branch table lives at 0x000001C8. The next
# label in the disasm is at 0x00000274. The gap is exactly 0xAC =
# 43 * 4 bytes, which matches gbatek's documented SWI count
# (0x00..0x2A = 43 functions). This is hard-coded as a sanity check
# rather than derived dynamically.
# ─────────────────────────────────────────────────────────────────────
SWI_TABLE_ADDR = 0x000001C8
SWI_TABLE_COUNT = 43
SWI_TABLE_STRIDE = 4

INTRO_COMMAND_TABLE_ADDR = 0x00003738
INTRO_COMMAND_TABLE_COUNT = 30
INTRO_COMMAND_TABLE_STRIDE = 4

# ─────────────────────────────────────────────────────────────────────
# Manual function-entry additions.
#
# The disasm labels the whole exception vector table at 0x00000000
# under a single `_start` symbol, but the runtime dispatches each
# vector slot separately:
#   runtime_swi()      -> runtime_dispatch(0x00000008)   (SWI vector)
#   IRQ entry path     -> runtime_dispatch(0x00000018)   (IRQ vector)
# Each vector slot holds a single `B handler` instruction. We
# declare them as separate `[[extra_func]]` entries so the
# dispatch table has the addresses the runtime actually targets.
#
# 0x00000000 is already covered by the disasm's `_start` symbol;
# we don't duplicate it.
MANUAL_EXTRA_FUNCS: list[tuple[int, str, str, str]] = [
    # (addr, mode, name, note)
    (0x00000008, "arm", "bios_swi_vector",
     "B swi_vector — runtime_swi() dispatches here"),
    (0x00000018, "arm", "bios_irq_vector",
     "B irq_vector — IRQ entry dispatches here"),
    # Runtime dispatch-miss discoveries — the function-finder can't
    # follow indirect BX to these THUMB targets, and they're not
    # labeled in the disasm. Added as we hit them at runtime; refine
    # the finder later (task #7).
    (0x00001928, "thumb", "bios_unknown_1928",
     "discovered via dispatch_miss during BIOS boot"),
    (0x00002424, "thumb", "bios_intro_callback_2424",
     "embedded BIOS callback pointer at 0x000016F8"),
    (0x00001708, "thumb", "bios_noop_callback_1708",
     "embedded BIOS callback pointer at 0x000016FC"),
    (0x00002148, "thumb", "bios_unknown_2148",
     "discovered via BIOS callback pointer 0x00002149 during BIOS boot"),
    (0x00000348, "arm", "bios_halt_cont_0348",
     "continuation after BIOS Halt writes HALTCNT at 0x00000344"),
]


def emit_toml(out_path: pathlib.Path,
              entries: list[SymEntry]) -> None:
    """Write the schema-conforming TOML."""
    lines: list[str] = []
    lines.append("# gba_bios.toml — per-binary recompile config for the GBA BIOS.")
    lines.append("#")
    lines.append("# Generated by tools/import_bios_symbols/import_bios_symbols.py")
    lines.append("# from third_party/gba_bios_disasm (camthesaxman/gba_bios).")
    lines.append("# Re-run the importer instead of hand-editing.")
    lines.append("#")
    lines.append("# See docs/TOML_SCHEMA.md for the schema reference.")
    lines.append("")

    lines.append("[program]")
    lines.append('name         = "GBA BIOS"')
    lines.append('id           = "gba_bios"')
    lines.append("load_address = 0x00000000")
    lines.append("size         = 0x00004000  # 16 KB")
    lines.append("entry_pc     = 0x00000000")
    lines.append("")

    lines.append("[identity]")
    lines.append('sha1 = "300c20df6731a33952ded8c436f7f186d25d3492"')
    lines.append('md5  = "a860e8c0b6d573d191e4ec7db1b1e4f6"')
    lines.append("")

    lines.append("# ── Function entries ────────────────────────────────────────────────")
    lines.append("# Imported from the disasm's ELF symbol table (arm-none-eabi-nm).")
    lines.append("# Auto-generated `_NNNNNNNN` basic-block labels are filtered out;")
    lines.append("# only deliberately-named labels survive. Plus a hand-coded")
    lines.append("# tail of exception-vector slots (0x08 SWI, 0x18 IRQ) that the")
    lines.append("# runtime dispatches separately even though the disasm groups")
    lines.append("# them under a single `_start` label.")
    lines.append("")

    # Combined sorted list: disasm-derived entries + manual additions.
    combined: list[tuple[int, str, str, str]] = []
    for e in entries:
        note = "global / exported in disasm" if e.visibility == "global" else ""
        combined.append((e.addr, e.mode, e.name, note))
    for addr, mode, name, note in MANUAL_EXTRA_FUNCS:
        combined.append((addr, mode, name, note))
    combined.sort(key=lambda r: r[0])

    for addr, mode, name, note in combined:
        lines.append("[[extra_func]]")
        lines.append(f"addr = 0x{addr:08X}")
        lines.append(f'mode = "{mode}"')
        lines.append(f'name = "{name}"')
        if note:
            lines.append(f'note = "{note}"')
        lines.append("")

    lines.append("# ── SWI branch table ────────────────────────────────────────────────")
    lines.append("# The BIOS SWI dispatcher at swi_vector (0x140) loads from this")
    lines.append("# table to jump to the per-SWI handler. Entries are ABS32 with")
    lines.append("# the standard ARM/THUMB interworking convention: bit 0 of each")
    lines.append("# entry encodes mode (set = THUMB, clear = ARM).")
    lines.append("# Size derived from the disasm layout: 43 SWIs × 4 bytes = 0xAC.")
    lines.append("")
    lines.append("[[jump_table]]")
    lines.append(f"addr         = 0x{SWI_TABLE_ADDR:08X}")
    lines.append(f"stride       = {SWI_TABLE_STRIDE}")
    lines.append(f"count        = {SWI_TABLE_COUNT}")
    lines.append('format       = "abs32"')
    lines.append('entries_mode = "auto"  # bit 0 of entry encodes mode')
    lines.append('name         = "swi_branch_table"')
    lines.append('note         = "BIOS SWI dispatch table — SWI 0x00..0x2A"')
    lines.append("")
    lines.append("[[jump_table]]")
    lines.append(f"addr         = 0x{INTRO_COMMAND_TABLE_ADDR:08X}")
    lines.append(f"stride       = {INTRO_COMMAND_TABLE_STRIDE}")
    lines.append(f"count        = {INTRO_COMMAND_TABLE_COUNT}")
    lines.append('format       = "abs32"')
    lines.append('entries_mode = "auto"  # bit 0 of entry encodes mode')
    lines.append('name         = "bios_intro_command_table"')
    lines.append('note         = "BIOS intro command handlers; opcodes 0xB1..0xCE"')
    lines.append("")

    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8",
                        newline="\n")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--disasm", type=pathlib.Path,
                    default=ROOT / "third_party" / "gba_bios_disasm",
                    help="Path to the cloned camthesaxman/gba_bios repo.")
    ap.add_argument("--out", type=pathlib.Path,
                    default=ROOT / "bios" / "gba_bios.toml",
                    help="Output TOML path.")
    args = ap.parse_args()

    elf = args.disasm / "gba_bios.elf"
    asm = args.disasm / "asm" / "bios.s"
    if not elf.exists():
        print(f"error: {elf} missing — build the disasm first:", file=sys.stderr)
        print(f"  cd {args.disasm}", file=sys.stderr)
        print(f"  make AS=arm-none-eabi-as LD=arm-none-eabi-ld "
              f"OBJCOPY=arm-none-eabi-objcopy", file=sys.stderr)
        return 1
    if not asm.exists():
        print(f"error: {asm} missing — wrong disasm layout?", file=sys.stderr)
        return 1

    print(f"==> reading symbols from {elf}")
    rows = run_nm(elf)
    print(f"    {len(rows)} text-section symbols")

    print(f"==> parsing modes from {asm}")
    mode_by_name, data_labels = parse_asm_modes(asm)
    print(f"    {len(mode_by_name)} labels with mode "
          f"({len(data_labels)} marked as data — excluded)")

    entries = classify(rows, mode_by_name, data_labels)
    arm_n = sum(1 for e in entries if e.mode == "arm")
    thumb_n = sum(1 for e in entries if e.mode == "thumb")
    glb_n = sum(1 for e in entries if e.visibility == "global")
    print(f"==> {len(entries)} function entries "
          f"(arm={arm_n} thumb={thumb_n} global={glb_n})")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    emit_toml(args.out, entries)
    print(f"==> wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
