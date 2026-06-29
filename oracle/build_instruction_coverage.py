#!/usr/bin/env python3
"""build_instruction_coverage.py -- Axis 1/6 instruction-coverage builder. NO TCP.

Cross-references the recompiler's STATIC opcode inventory against (a) where each
opcode is decoded, code-generated, and interpreted in the source, and (b) the
per-run self-heal coverage tally, then emits a single machine-readable report:

    generated/gba_instruction_coverage.json

Inputs (all read-only source / artifacts):
  * src/armv4t/arm_ir.h        -- the canonical IrOp enum (the opcode universe)
  * src/armv4t/arm_decode.cpp  -- ARM   decoder: which IrOp it produces  (used)
  * src/armv4t/thumb_decode.cpp-- THUMB decoder: which IrOp it produces  (used)
  * src/armv4t/arm_codegen.cpp -- recompiler backend: which IrOp it emits (implemented)
  * src/armv4t/interpreter.cpp -- reference executor: which IrOp it runs  (interp)
  * <game>/recomp_coverage.json (self_heal) -- per-run static/healed/interpreted tally

Per opcode it records implemented / decoded(arm|thumb) / interpreter / missing
plus the exact source line(s) of every reference (the line ranges). "missing" =
present in the enum but never emitted by codegen (a real backend gap);
"decoded_not_implemented" flags an opcode a decoder can produce but codegen never
handles (the dangerous class).

Usage
-----
  python oracle/build_instruction_coverage.py
  python oracle/build_instruction_coverage.py --coverage-json ../MinishCapRecomp/recomp_coverage.json
  python oracle/build_instruction_coverage.py --out generated/gba_instruction_coverage.json
"""
from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys

import recomp_paths as rp

ROOT = pathlib.Path(__file__).resolve().parent.parent
ARMV4T = ROOT / "src" / "armv4t"

IROP_RE = re.compile(r"IrOp::([A-Za-z_][A-Za-z0-9_]*)")


def parse_enum(header):
    """Extract IrOp identifiers from the `enum class IrOp { ... };` block."""
    text = header.read_text(encoding="utf-8", errors="replace")
    m = re.search(r"enum\s+class\s+IrOp\s*:[^{]*\{(.*?)\};", text, re.S)
    if not m:
        m = re.search(r"enum\s+class\s+IrOp\s*\{(.*?)\};", text, re.S)
    if not m:
        return []
    body = m.group(1)
    body = re.sub(r"//[^\n]*", "", body)          # line comments
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)  # block comments
    names = []
    for tok in body.split(","):
        tok = tok.strip()
        if not tok:
            continue
        tok = tok.split("=")[0].strip()           # drop "= 0"
        if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", tok):
            names.append(tok)
    return names


def scan_refs(path):
    """Map IrOp name -> sorted list of line numbers referencing it in `path`."""
    refs = {}
    if not path.exists():
        return refs
    for i, line in enumerate(path.read_text(encoding="utf-8",
                                            errors="replace").splitlines(), 1):
        for name in IROP_RE.findall(line):
            refs.setdefault(name, []).append(i)
    return refs


def line_ranges(lines):
    """Compress a sorted line list into 'a', 'a-b' run strings."""
    if not lines:
        return []
    lines = sorted(set(lines))
    out = []
    start = prev = lines[0]
    for n in lines[1:]:
        if n == prev + 1:
            prev = n
            continue
        out.append(f"{start}" if start == prev else f"{start}-{prev}")
        start = prev = n
    out.append(f"{start}" if start == prev else f"{start}-{prev}")
    return out


def find_coverage_json(explicit):
    cands = []
    if explicit:
        cands.append(pathlib.Path(explicit))
    game = rp.game_dir(ROOT)
    cands += [
        game / "recomp_coverage.json",
        game / "build" / "recomp_coverage.json",
        ROOT / "recomp_coverage.json",
    ]
    for c in cands:
        if c.exists():
            return c
    return None


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--coverage-json", default=None,
                    help="self-heal coverage json (default: auto-discover)")
    ap.add_argument("--out", default=str(ROOT / "generated" /
                                        "gba_instruction_coverage.json"))
    args = ap.parse_args()

    header = ARMV4T / "arm_ir.h"
    if not header.exists():
        print(f"[err] missing {header}")
        return 1
    opcodes = parse_enum(header)
    if not opcodes:
        print(f"[err] could not parse IrOp enum from {header}")
        return 1

    src = {
        "arm_decode": ARMV4T / "arm_decode.cpp",
        "thumb_decode": ARMV4T / "thumb_decode.cpp",
        "codegen": ARMV4T / "arm_codegen.cpp",
        "interpreter": ARMV4T / "interpreter.cpp",
    }
    refs = {k: scan_refs(p) for k, p in src.items()}
    for k, p in src.items():
        if not p.exists():
            print(f"[warn] source not found: {p} (its column will be blank)")

    inventory = {}
    missing = []
    decoded_not_impl = []
    impl_n = dec_n = interp_n = 0
    for idx, name in enumerate(opcodes):
        a_dec = name in refs["arm_decode"]
        t_dec = name in refs["thumb_decode"]
        cg = name in refs["codegen"]
        it = name in refs["interpreter"]
        decoded = a_dec or t_dec
        is_sentinel = (name == "Undefined")
        miss = (not cg) and (not is_sentinel)
        inventory[name] = {
            "value": idx,
            "implemented": cg,
            "decoded_arm": a_dec,
            "decoded_thumb": t_dec,
            "interpreter": it,
            "decoded": decoded,
            "missing": miss,
            "sentinel": is_sentinel,
            "refs": {
                "codegen": line_ranges(refs["codegen"].get(name, [])),
                "arm_decode": line_ranges(refs["arm_decode"].get(name, [])),
                "thumb_decode": line_ranges(refs["thumb_decode"].get(name, [])),
                "interpreter": line_ranges(refs["interpreter"].get(name, [])),
            },
        }
        if cg:
            impl_n += 1
        if decoded:
            dec_n += 1
        if it:
            interp_n += 1
        if miss:
            missing.append(name)
        if decoded and not cg and not is_sentinel:
            decoded_not_impl.append(name)

    cov_path = find_coverage_json(args.coverage_json)
    runtime_cov = None
    if cov_path:
        try:
            runtime_cov = json.loads(cov_path.read_text(encoding="utf-8"))
        except Exception as e:
            print(f"[warn] could not parse coverage json {cov_path}: {e}")
    else:
        print("[note] no recomp_coverage.json found -- runtime tally omitted "
              "(run the game build once to emit it).")

    report = {
        "generated_by": "oracle/build_instruction_coverage.py",
        "sources": {k: str(p) for k, p in src.items()} | {"enum": str(header)},
        "summary": {
            "total_opcodes": len(opcodes),
            "implemented_in_codegen": impl_n,
            "decoded": dec_n,
            "interpreted": interp_n,
            "missing_from_codegen": missing,
            "decoded_not_implemented": decoded_not_impl,
        },
        "opcode_inventory": inventory,
        "runtime_coverage": {
            "source": str(cov_path) if cov_path else None,
            "verdict": (runtime_cov or {}).get("coverage"),
            "distinct_misses": (runtime_cov or {}).get("distinct_misses"),
            "interpreted_insns": (runtime_cov or {}).get("interpreted_insns"),
            "healed_native": (runtime_cov or {}).get("healed_native"),
            "native_calls": (runtime_cov or {}).get("native_calls"),
            "misses": (runtime_cov or {}).get("misses"),
        } if runtime_cov is not None else None,
    }

    out = pathlib.Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2), encoding="utf-8")

    # ASCII console summary
    print(f"=== INSTRUCTION COVERAGE (static) ===")
    print(f"  opcodes (IrOp)        : {len(opcodes)}")
    print(f"  implemented in codegen: {impl_n}")
    print(f"  decoded (arm|thumb)   : {dec_n}")
    print(f"  interpreter executes  : {interp_n}")
    print(f"  missing from codegen  : {len(missing)}"
          + (f"  -> {', '.join(missing)}" if missing else ""))
    print(f"  decoded-not-impl      : {len(decoded_not_impl)}"
          + (f"  -> {', '.join(decoded_not_impl)}" if decoded_not_impl else ""))
    if runtime_cov is not None:
        rc = report["runtime_coverage"]
        print(f"  --- per-run self-heal tally ({pathlib.Path(cov_path).name}) ---")
        print(f"  verdict={rc['verdict']} distinct_misses={rc['distinct_misses']} "
              f"interpreted_insns={rc['interpreted_insns']} "
              f"healed_native={rc['healed_native']}")
    print(f"  wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
