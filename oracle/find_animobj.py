#!/usr/bin/env python3
"""find_animobj.py — locate the MC-HP-002 animation object in an IWRAM dump and
report its animId (obj+0x12) and frame-script ptr (obj+0x5c).

Used to compare the recomp vs the independent mGBA oracle (gbaref `D` dump) at the
loading-zone freeze. The object is identified two ways:
  1) at the recomp's known address 0x030018D0 (MC's entity tables are static IWRAM,
     so the oracle may place it there too);
  2) by searching all 4-aligned objects whose obj+0x48 == the anim-def ROM ptr
     (default 0x080FD170 — the entity's distinctive field at the freeze).

The decisive H1/H2 signal: animId. If the oracle's object has animId==0 too, the
NULL-group animation is genuine (H1 → fix = BIOS open-bus). If the oracle has a
NONZERO animId / a clean ROM frame-ptr, animId=0 is an upstream recomp bug (H2).

Usage:
  python oracle/find_animobj.py <iwram.bin> [--base 0x03000000] [--obj 0x030018D0]
                                 [--defptr 0x080FD170]
"""
from __future__ import annotations
import argparse, struct, sys


def rd(buf, off, n):
    return int.from_bytes(buf[off:off + n], "little")


def report_obj(buf, base, obj):
    o = obj - base
    if o < 0 or o + 0x60 > len(buf):
        print(f"  obj 0x{obj:08X}: out of dump range")
        return
    animid = rd(buf, o + 0x12, 2)
    fptr   = rd(buf, o + 0x5c, 4)
    print(f"  obj 0x{obj:08X}:  animId(+0x12)=0x{animid:04X}  "
          f"frameptr(+0x5c)=0x{fptr:08X}  defptr(+0x48)=0x{rd(buf,o+0x48,4):08X}")
    print(f"    fields: " + " ".join(f"+{k:02X}=0x{rd(buf,o+k,4):08X}"
          for k in (0x00, 0x04, 0x10, 0x44, 0x48, 0x58, 0x5c)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--base", type=lambda x: int(x, 0), default=0x03000000)
    ap.add_argument("--obj", type=lambda x: int(x, 0), default=0x030018D0)
    ap.add_argument("--defptr", type=lambda x: int(x, 0), default=0x080FD170)
    args = ap.parse_args()
    buf = open(args.dump, "rb").read()
    print(f"dump {args.dump}: {len(buf)} bytes, base 0x{args.base:08X}")

    print(f"[1] object at the recomp address 0x{args.obj:08X}:")
    report_obj(buf, args.base, args.obj)

    print(f"[2] search: objects with obj+0x48 == 0x{args.defptr:08X}")
    hits = 0
    for o in range(0, len(buf) - 0x4c, 4):
        if rd(buf, o + 0x48, 4) == args.defptr:
            report_obj(buf, args.base, args.base + o)
            hits += 1
    if not hits:
        print("  (none — the oracle's entity may use a different def ptr; "
              "inspect [1] or widen the search)")


if __name__ == "__main__":
    main()
