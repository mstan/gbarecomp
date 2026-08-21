#!/usr/bin/env python3
"""Golden test for tools/symbol_import/import_decomp_symbols.py.

Runs the importer over synthetic readelf / link-map fixtures and compares its
four outputs against the goldens in `golden/`. The fixtures are hand-written
to cover the cases that actually bit us on real decomps:

  * THUMB vs ARM mode carried in bit 0 of st_value;
  * ARM mapping symbols ($a/$t/$d) and numeric local labels (_08001AEC),
    which are assembler artifacts and must never become program symbols;
  * absolute (ndx=ABS) symbols: the useful RAM globals a part-disassembled
    decomp declares, mixed with the assembler equates and object-file names
    that are re-emitted per translation unit and must be filtered out;
  * a SINGLE loadable output section covering code and data, so the section
    table yields no data ranges and `auto` must fall back to the link map;
  * link-map section names long enough that ld wraps them onto their own
    line (.rodata.wave_167), which a naive line parser silently drops;
  * a FUNC symbol that lands inside a data range, which must be dropped.

Regenerate the goldens with `--update` after an intentional format change,
and read the diff before committing it.
"""
from __future__ import annotations

import filecmp
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
IMPORTER = HERE.parent.parent / "tools" / "symbol_import" / \
    "import_decomp_symbols.py"
FIXTURES = HERE / "fixtures"
GOLDEN = HERE / "golden"

OUTPUTS = [
    "imported_symbols.tsv",
    "function_boundaries.tsv",
    "imported_data_symbols.tsv",
    "TEST_symbols.toml",
]


def run(out_dir: Path) -> None:
    cmd = [
        sys.executable, str(IMPORTER),
        "--id", "TEST",
        "--name", "Synthetic Test Program",
        "--syms", str(FIXTURES / "readelf_syms.txt"),
        "--sections", str(FIXTURES / "readelf_sections.txt"),
        "--map", str(FIXTURES / "link.map"),
        "--out", str(out_dir),
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        print(proc.stdout)
        print(proc.stderr, file=sys.stderr)
        raise SystemExit(f"importer failed with {proc.returncode}")
    # No --rom: the overlay must then carry no [identity], and the importer
    # must say so rather than inventing a hash.
    if "no --rom" not in proc.stderr:
        raise SystemExit("expected a warning about the missing --rom")


def main() -> int:
    update = "--update" in sys.argv[1:]
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp)
        run(out)
        if update:
            GOLDEN.mkdir(parents=True, exist_ok=True)
            for name in OUTPUTS:
                shutil.copyfile(out / name, GOLDEN / name)
            print(f"updated {len(OUTPUTS)} golden file(s) in {GOLDEN}")
            return 0
        failures = []
        for name in OUTPUTS:
            produced, golden = out / name, GOLDEN / name
            if not golden.exists():
                failures.append(f"{name}: no golden (run with --update)")
                continue
            if not filecmp.cmp(produced, golden, shallow=False):
                failures.append(name)
                print(f"--- {name} differs from golden ---")
                import difflib
                diff = difflib.unified_diff(
                    golden.read_text(encoding="utf-8").splitlines(),
                    produced.read_text(encoding="utf-8").splitlines(),
                    fromfile=f"golden/{name}", tofile=f"produced/{name}",
                    lineterm="")
                for line in list(diff)[:60]:
                    print(line)
        if failures:
            print("symbol_import_golden: FAILED — " + ", ".join(failures))
            return 1
    print("symbol_import_golden: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
