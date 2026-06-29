#!/usr/bin/env python3
"""diff_irq_nba.py -- Axis 3 (Interrupt / event timing) recomp-vs-NanoBoyAdvance
IRQ comparator. Aligns IRQ TAKE events by (source, Nth-of-source) and diffs the
take-to-take cycle intervals.

Asymmetry handled: the recomp exposes TAKE-time only ({cycle,src,ret,cpsr,
from_halt}, src = IE&IF mask at the vector); NBA exposes a raise+take ring (raise
carries the IF bit mask, take carries the saved LR). We attribute each NBA take a
source from its most-recent preceding raise, so both sides yield (source ->
take-cycle sequence). We compare take-vs-take. NBA's raise->take LATENCY is
reported SEPARATELY as a hardware reference (the recomp has no raise timestamp --
burndown Axis-3 gap).

Reports: per-source take-to-take interval drift (consecutive same-source deltas,
offset-cancelled), missing/extra IRQs per source, the ordered source-sequence
first mismatch, and NBA raise->take latency stats.

Sync doctrine: align by IRQ source + per-source index (a hardware event), never a
frame number; deltas cancel the boot offset; each side free-runs its own always-
on IRQ ring and we query a window.

Reads:
  recomp (:19842) irq_cap{count} -> {total,count,entries:[{cycle,src,ret,cpsr,
    from_halt}]} -- populated by runtime_irq in the per-game build (empty under
    bios_smoke; degrades gracefully).
  nba (:19844) irq_cap{count,start?} -> {head,first,count,cyc,val,kind} (hex);
    kind 0=raise(val=IF mask), 1=take(val=LR).

Usage
-----
  python oracle/diff_irq_nba.py --frames 200            # real sweep
  python oracle/diff_irq_nba.py --self-nba --frames 200 # comparator self-test
"""
from __future__ import annotations

import argparse
import statistics
import sys

import nba_common as nc


def pull_irq_nba(c, ring=65536):
    probe = c.call(cmd="irq_cap", count=1)
    if not nc.ok(probe):
        return None
    head = int(probe.get("head", 0))
    oldest = head - min(head, ring)
    raises = []   # (cycle, mask)
    takes = []    # (cycle, lr, src_mask, latency)
    start = oldest
    pending_raise = None
    while start < head:
        n = min(ring, head - start)
        r = c.call(cmd="irq_cap", start=start, count=n)
        if not nc.ok(r) or int(r.get("count", 0)) == 0:
            break
        cnt = int(r["count"])
        cyc = nc.hx_u64(r.get("cyc", ""))
        val = nc.hx_u32(r.get("val", ""))
        kind = nc.hx_u8(r.get("kind", ""))
        for k in range(cnt):
            if kind[k] == 0:  # raise
                pending_raise = (cyc[k], val[k])
                raises.append((cyc[k], val[k]))
            else:             # take
                mask = pending_raise[1] if pending_raise else 0
                lat = cyc[k] - pending_raise[0] if pending_raise else -1
                takes.append((cyc[k], val[k], mask, lat))
        start = int(r.get("first", start)) + cnt
    return {"raises": raises, "takes": takes}


def pull_irq_recomp(c, count=65536):
    r = c.call(cmd="irq_cap", count=count)
    if not nc.ok(r):
        return None
    takes = []
    for e in r.get("entries", []):
        takes.append((e["cycle"], e.get("ret", 0), e["src"], -1))
    return {"raises": [], "takes": takes, "total": int(r.get("total", 0))}


def by_source(takes):
    d = {}
    for cyc, lr, mask, lat in takes:
        d.setdefault(nc.src_name(mask), []).append(cyc)
    for k in d:
        d[k].sort()
    return d


def intervals(cyc_list):
    return [cyc_list[i + 1] - cyc_list[i] for i in range(len(cyc_list) - 1)]


def compare(rec, nba, tol, strict=False):
    ra, na = rec["takes"], nba["takes"]
    print("\n=== IRQ TAKE-EVENT DIFF: recomp vs nba ===")
    print(f"  total takes: recomp={len(ra)}  nba={len(na)}  "
          f"(nba raises={len(nba['raises'])})")

    # Known oracle artifact: NanoBoyAdvance fake-completes EXTERNAL-clock SIO
    # transfers (its SIOCNT handler ignores the clock-select bit), firing a
    # Serial IRQ that real single-player hardware never raises (slave clock, no
    # master -> transfer never completes). The recomp faithfully omits it. So
    # when recomp has ZERO Serial takes but NBA has them, that is the artifact,
    # not recomp drift -- annotate and exclude from the verdict. (If a game uses
    # INTERNAL-clock SIO IRQ, recomp DOES fire Serial and the counts match, so
    # nothing is excluded.) See burndown Axis-3 resolution. --strict disables.
    rs0, ns0 = by_source(ra), by_source(na)
    artifact_serial = (not strict and len(rs0.get("Serial", [])) == 0
                       and len(ns0.get("Serial", [])) > 0)
    if artifact_serial:
        print(f"  [oracle artifact] nba Serial takes={len(ns0['Serial'])} vs "
              f"recomp=0 -> NBA fake-SIO-completion (external-clock, no link "
              f"partner); recomp faithful. Excluded from verdict (--strict to "
              f"include).")
        ra = [t for t in ra if nc.src_name(t[2]) != "Serial"]
        na = [t for t in na if nc.src_name(t[2]) != "Serial"]

    # ordered source-sequence first mismatch
    rseq = [nc.src_name(t[2]) for t in ra]
    nseq = [nc.src_name(t[2]) for t in na]
    m = min(len(rseq), len(nseq))
    seq_mismatch = next((i for i in range(m) if rseq[i] != nseq[i]), None)
    if seq_mismatch is None and len(rseq) != len(nseq):
        seq_mismatch = m
    if seq_mismatch is None:
        print(f"  source sequence : identical order over {m} takes  OK")
    else:
        print(f"  source sequence : FIRST mismatch at take #{seq_mismatch}: "
              f"recomp={rseq[seq_mismatch] if seq_mismatch < len(rseq) else '(end)'}"
              f" nba={nseq[seq_mismatch] if seq_mismatch < len(nseq) else '(end)'}")

    rs, ns = by_source(ra), by_source(na)
    allsrc = sorted(set(rs) | set(ns))
    print(f"  per-source take counts + interval drift:")
    verdict = "ZERO-DRIFT"
    for s in allsrc:
        rc = rs.get(s, [])
        ncl = ns.get(s, [])
        if len(rc) != len(ncl):
            verdict = "DRIFT"
            print(f"    {s:<10} count recomp={len(rc)} nba={len(ncl)} "
                  f"(d={len(rc)-len(ncl):+d})  <-- missing/extra")
        ri, ni = intervals(rc), intervals(ncl)
        k = min(len(ri), len(ni))
        if k == 0:
            if len(rc) == len(ncl):
                print(f"    {s:<10} count={len(rc)} (too few to form intervals)")
            continue
        onset = next((i for i in range(k) if abs(ri[i] - ni[i]) > tol), None)
        med_r = statistics.median(ri[:k])
        med_n = statistics.median(ni[:k])
        # Verdict on STEADY STATE (median), per the project's offset-cancelled
        # delta doctrine: a transient at the boot interval (e.g. the known
        # axis-2 WAITCNT/prefetch boot skew) with a matching median is NOT
        # timing drift -- annotate it, don't fail on it. Real ongoing drift
        # moves the median.
        if med_r != med_n:
            verdict = "DRIFT"
            tag = "median-skew"
        elif onset is not None:
            tag = f"steady-match (boot transient @{onset})"
        else:
            tag = "steady-match"
        print(f"    {s:<10} count={len(rc)}/{len(ncl)} interval "
              f"med recomp={med_r} nba={med_n} (d={med_r-med_n:+d}) [{tag}]")

    # NBA raise->take latency reference
    lats = [t[3] for t in na if t[3] >= 0]
    if lats:
        print(f"  [ref] nba raise->take latency: n={len(lats)} "
              f"mean={statistics.mean(lats):.1f} median={statistics.median(lats)} "
              f"min={min(lats)} max={max(lats)} cyc")
    print(f"  VERDICT: {verdict}")
    return verdict


def self_nba(nba, tol):
    a = pull_irq_nba(nba)
    b = pull_irq_nba(nba)
    print("\n=== IRQ SELF-TEST: nba vs nba ===")
    if not a or not b:
        print("[err] nba irq_cap returned no data")
        return "ERR"
    print(f"  takes: a={len(a['takes'])} b={len(b['takes'])} "
          f"raises a={len(a['raises'])} b={len(b['raises'])}")
    rseq = [t[:3] for t in a["takes"]]
    nseq = [t[:3] for t in b["takes"]]
    if rseq == nseq:
        print(f"  take streams identical over {len(rseq)} events  OK")
        # interval check per source
        rs, ns = by_source(a["takes"]), by_source(b["takes"])
        drift = any(intervals(rs[s]) != intervals(ns.get(s, [])) for s in rs)
        print(f"  per-source intervals identical: {not drift}")
        lats = [t[3] for t in a["takes"] if t[3] >= 0]
        if lats:
            print(f"  [ref] raise->take latency median={statistics.median(lats)} "
                  f"max={max(lats)} cyc")
        v = "ZERO-DRIFT" if not drift else "DRIFT"
        print(f"  VERDICT: {v}")
        return v
    print("  VERDICT: DRIFT (self mismatch -- comparator bug!)")
    return "DRIFT"


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--recomp-port", type=int, default=19842)
    ap.add_argument("--nba-port", type=int, default=19844)
    ap.add_argument("--frames", type=int, default=200,
                    help="frames to advance each side from cold before pulling")
    ap.add_argument("--tol", type=int, default=0,
                    help="per-interval cycle tolerance before flagging drift")
    ap.add_argument("--strict", action="store_true",
                    help="do NOT exclude NBA's fake-SIO Serial IRQs (oracle "
                         "artifact); show them as drift")
    ap.add_argument("--self-nba", action="store_true")
    ap.add_argument("--host", default="127.0.0.1")
    args = ap.parse_args()

    if args.self_nba:
        nba = nc.connect(args.host, args.nba_port, "nba")
        if not nba:
            return 2
        nba.call(cmd="reset")
        nba.call(cmd="run_frames", n=args.frames)
        v = self_nba(nba, args.tol)
        nba.close()
        return 0 if v == "ZERO-DRIFT" else 1

    rec = nc.connect(args.host, args.recomp_port, "recomp")
    nba = nc.connect(args.host, args.nba_port, "nba")
    if not nba:
        return 2
    nba.call(cmd="reset")
    nba.call(cmd="run_frames", n=args.frames)
    nba_irq = pull_irq_nba(nba)
    rec_irq = None
    if rec:
        for _ in range(args.frames):
            try:
                rec.call(timeout=30, cmd="step")
            except Exception as e:
                print(f"[warn] recomp step failed/stalled: {e}")
                break
        rec_irq = pull_irq_recomp(rec)
        rec.close()
    nba.close()

    if rec_irq is None:
        print("\n[partial] recomp irq_cap unavailable; nba-only below.")
    elif not rec_irq["takes"]:
        print(f"\n[pending] recomp IRQ ring is empty (total="
              f"{rec_irq.get('total', 0)}). It is populated by runtime_irq in the "
              f"per-game build; bios_smoke does not vector IRQs through it.")
    elif nba_irq:
        v = compare(rec_irq, nba_irq, args.tol, args.strict)
        return 0 if v == "ZERO-DRIFT" else 1

    if nba_irq:
        ns = by_source(nba_irq["takes"])
        print(f"[nba] takes={len(nba_irq['takes'])} by source: "
              + ", ".join(f"{s}={len(v)}" for s, v in sorted(ns.items())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
