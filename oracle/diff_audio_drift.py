#!/usr/bin/env python3
"""diff_audio_drift.py — drift-tolerant recomp-vs-oracle audio comparator.

Companion to the GBA accuracy burndown (Axis 5a, audio). Cross-emulator
bit-exact on the *mixed* GBA output is unrealistic (independent resample /
phase / loudness), so the mixed-stream verdict is **drift-tolerant**:

  1. cross-correlation lag alignment      (best integer-sample lag + peak r)
  2. post-alignment Pearson r + RMS error  (on amplitude-normalised streams)
  3. onset-timing histogram                (energy-flux onsets, matched deltas)
  4. per-note pitch error                  (autocorr pitch track, cents error)

The PSG channels are deterministic from the IO write stream, so for those we
also support a **near-bit-exact** per-channel check (metric 5) between two
capture sources that BOTH expose the per-channel ring (e.g. recomp static vs
recomp interpreter) — first divergent sample + max abs error.

Sources
-------
* recomp  : the runtime's always-on capture ring via the `audio_cap` TCP cmd
            (non-destructive; mixed + ch1..4 + direct_a/b).  --recomp-port
* nba     : the NanoBoyAdvance oracle's pre-resample mixer ring via its
            `audio_cap` cmd (stereo l/r + cycle stamp). Downmixed to mono.
            --nba-port
* file    : a raw little-endian int16 mono stream on disk.  --file-a/--file-b

This tool does NOT pause/step two emulators into lockstep — each side
free-runs N frames, fills its own always-on ring, and we QUERY a window, then
align in software via cross-correlation (ring-buffer discipline).

Usage
-----
  # First slice: recomp BIOS chime vs NBA, mixed-stream drift metrics
  python oracle/diff_audio_drift.py --recomp-port 19842 --nba-port 19844 \
      --frames 150 --count 40000

  # Self-test (sanity): a stream vs itself -> lag 0, r 1.0
  python oracle/diff_audio_drift.py --recomp-port 19842 --self --frames 150

  # PSG per-channel bit-check between two recomp sources (static vs interp)
  python oracle/diff_audio_drift.py --recomp-port 19842 \
      --recomp2-port 19852 --frames 200 --psg-bitcheck
"""
import argparse
import json
import socket
import struct
import sys

import numpy as np


# ── TCP plumbing ───────────────────────────────────────────────────────────
class Client:
    def __init__(self, host, port, timeout=10.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.f = self.sock.makefile("rwb")

    def call(self, **kw):
        self.f.write((json.dumps(kw) + "\n").encode())
        self.f.flush()
        return json.loads(self.f.readline().decode())

    def close(self):
        try:
            self.f.close()
            self.sock.close()
        except OSError:
            pass


def _hex_i16(h):
    if not h:
        return np.zeros(0, dtype=np.int16)
    b = bytes.fromhex(h)
    return np.frombuffer(b, dtype="<i2").astype(np.int16)


def _hex_u64(h):
    if not h:
        return np.zeros(0, dtype=np.uint64)
    b = bytes.fromhex(h)
    return np.frombuffer(b, dtype="<u8").astype(np.uint64)


def capture_recomp(port, frames, count, host="127.0.0.1", quit_after=False,
                   start=None):
    """Step `frames` frames, then non-destructively read the capture ring.

    Returns dict: rate, mixed, ch1..ch4, direct_a, direct_b (all np.int16).
    """
    c = Client(host, port)
    try:
        for _ in range(frames):
            c.call(cmd="step")
        cap = {"cmd": "audio_cap", "count": count}
        if start is not None:
            cap["start"] = start
        r = c.call(**cap)
        if not r.get("ok"):
            raise RuntimeError(f"audio_cap failed: {r}")
        out = {"rate": int(r["rate"]), "first": int(r.get("first", 0))}
        for k in ("mixed", "ch1", "ch2", "ch3", "ch4", "direct_a", "direct_b"):
            out[k] = _hex_i16(r.get(k, ""))
        if quit_after:
            c.call(cmd="quit")
        return out
    finally:
        c.close()


def capture_nba(port, frames, count, host="127.0.0.1", quit_after=False,
                start=None):
    """Run `frames` NBA frames, read the pre-resample stereo ring, downmix.

    Returns dict: rate, mixed (mono np.int16), l, r, cyc (np.uint64).
    """
    c = Client(host, port)
    try:
        c.call(cmd="run_frames", n=frames)
        cap = {"cmd": "audio_cap", "count": count}
        if start is not None:
            cap["start"] = start
        r = c.call(**cap)
        if not r.get("ok"):
            raise RuntimeError(f"nba audio_cap failed: {r}")
        l = _hex_i16(r.get("l", "")).astype(np.int32)
        rr = _hex_i16(r.get("r", "")).astype(np.int32)
        n = min(len(l), len(rr))
        mono = ((l[:n] + rr[:n]) // 2).astype(np.int16)
        out = {
            "rate": int(r["rate"]),
            "first": int(r.get("first", 0)),
            "mixed": mono,
            "l": l[:n].astype(np.int16),
            "r": rr[:n].astype(np.int16),
            "cyc": _hex_u64(r.get("cyc", "")),
        }
        if quit_after:
            c.call(cmd="quit")
        return out
    finally:
        c.close()


# ── DSP helpers ────────────────────────────────────────────────────────────
def _trim_silence(x, thresh=1e-4):
    """Trim leading/trailing near-silence on a float stream (peak-normalised)."""
    if x.size == 0:
        return x, 0
    pk = np.max(np.abs(x)) or 1.0
    mask = np.abs(x) > thresh * pk
    if not mask.any():
        return x, 0
    lo = int(np.argmax(mask))
    hi = int(len(mask) - np.argmax(mask[::-1]))
    return x[lo:hi], lo


def _normalize(x):
    x = x.astype(np.float64)
    x = x - np.mean(x)
    rms = np.sqrt(np.mean(x * x)) or 1.0
    return x / rms


def xcorr_align(a, b, max_lag=None):
    """Best integer lag aligning b to a, by normalised cross-correlation.

    Positive lag => b lags a (b starts `lag` samples later). Returns
    (lag, peak_r).
    """
    from scipy.signal import correlate, correlation_lags

    fa, fb = _normalize(a), _normalize(b)
    corr = correlate(fa, fb, mode="full")
    lags = correlation_lags(len(fa), len(fb), mode="full")
    if max_lag is not None:
        keep = np.abs(lags) <= max_lag
        corr, lags = corr[keep], lags[keep]
    lag = int(lags[int(np.argmax(corr))])
    # Report a TRUE correlation coefficient at the winning lag (Pearson over
    # the actual overlap) — the raw full-correlation peak is over-counted when
    # the two streams differ in length, so don't divide-and-hope.
    a_aln, b_aln = apply_lag(a, b, lag)
    return lag, pearson(a_aln, b_aln)


def apply_lag(a, b, lag):
    """Return the overlapping aligned slices (a_aln, b_aln) for the given lag."""
    if lag >= 0:
        b2 = b[:]
        a2 = a[lag:]
    else:
        a2 = a[:]
        b2 = b[-lag:]
    n = min(len(a2), len(b2))
    return a2[:n], b2[:n]


def pearson(a, b):
    fa, fb = a.astype(np.float64), b.astype(np.float64)
    fa -= fa.mean()
    fb -= fb.mean()
    da = np.sqrt(np.sum(fa * fa))
    db = np.sqrt(np.sum(fb * fb))
    if da == 0 or db == 0:
        return 0.0
    return float(np.sum(fa * fb) / (da * db))


def energy_envelope(x, rate, hop_ms=5.0, win_ms=20.0):
    hop = max(1, int(rate * hop_ms / 1000))
    win = max(hop, int(rate * win_ms / 1000))
    xf = x.astype(np.float64)
    n = 1 + max(0, (len(xf) - win) // hop)
    env = np.empty(n)
    for i in range(n):
        seg = xf[i * hop: i * hop + win]
        env[i] = np.sqrt(np.mean(seg * seg)) if seg.size else 0.0
    return env, hop


def detect_onsets(x, rate, hop_ms=5.0):
    """Energy-flux onset frame indices -> onset times in ms."""
    env, hop = energy_envelope(x, rate, hop_ms=hop_ms)
    if env.size < 3:
        return np.zeros(0), env, hop
    flux = np.diff(env, prepend=env[0])
    flux[flux < 0] = 0.0
    thr = flux.mean() + 1.5 * flux.std()
    onsets = []
    for i in range(1, len(flux) - 1):
        if flux[i] > thr and flux[i] >= flux[i - 1] and flux[i] > flux[i + 1]:
            onsets.append(i)
    times = np.array(onsets) * (hop / rate) * 1000.0
    return times, env, hop


def onset_histogram(ta, tb, tol_ms=30.0):
    """Greedy-match onsets, return matched deltas (tb-ta) ms + match stats."""
    deltas = []
    used = np.zeros(len(tb), dtype=bool)
    for t in ta:
        if len(tb) == 0:
            break
        d = tb - t
        j = int(np.argmin(np.abs(d)))
        if not used[j] and abs(d[j]) <= tol_ms:
            used[j] = True
            deltas.append(float(d[j]))
    return np.array(deltas), len(ta), len(tb)


def pitch_track(x, rate, frame_ms=40.0, hop_ms=20.0, fmin=50.0, fmax=2000.0):
    """Autocorrelation pitch per frame (Hz, 0 = unvoiced)."""
    frame = int(rate * frame_ms / 1000)
    hop = int(rate * hop_ms / 1000)
    xf = x.astype(np.float64)
    lo = int(rate / fmax)
    hi = int(rate / fmin)
    out = []
    for i in range(0, max(0, len(xf) - frame), hop):
        seg = xf[i:i + frame]
        seg = seg - seg.mean()
        e = np.sum(seg * seg)
        if e < 1e-6:
            out.append(0.0)
            continue
        ac = np.correlate(seg, seg, mode="full")[len(seg) - 1:]
        if hi >= len(ac):
            out.append(0.0)
            continue
        region = ac[lo:hi]
        if region.size == 0:
            out.append(0.0)
            continue
        peak = int(np.argmax(region)) + lo
        # voiced only if the periodicity peak is reasonably strong
        out.append(rate / peak if ac[peak] > 0.3 * ac[0] else 0.0)
    return np.array(out)


def pitch_cents_error(pa, pb):
    """Per-frame cents error where both frames are voiced."""
    n = min(len(pa), len(pb))
    pa, pb = pa[:n], pb[:n]
    v = (pa > 0) & (pb > 0)
    if not v.any():
        return np.zeros(0), 0
    cents = 1200.0 * np.log2(pb[v] / pa[v])
    return cents, int(v.sum())


# ── Metrics report ─────────────────────────────────────────────────────────
def compare_mixed(a_raw, b_raw, rate, label_a, label_b, max_lag_ms=200.0):
    a, off_a = _trim_silence(a_raw.astype(np.float64))
    b, off_b = _trim_silence(b_raw.astype(np.float64))
    print(f"\n=== MIXED-STREAM DRIFT: {label_a} vs {label_b} (rate {rate} Hz) ===")
    print(f"  samples: {label_a}={len(a_raw)} ({len(a)} after silence-trim), "
          f"{label_b}={len(b_raw)} ({len(b)})")
    pk_a = float(np.max(np.abs(a_raw))) if a_raw.size else 0.0
    pk_b = float(np.max(np.abs(b_raw))) if b_raw.size else 0.0
    rms_a = float(np.sqrt(np.mean(a_raw.astype(np.float64) ** 2))) if a_raw.size else 0.0
    rms_b = float(np.sqrt(np.mean(b_raw.astype(np.float64) ** 2))) if b_raw.size else 0.0
    print(f"  peak:    {label_a}={pk_a:.0f}  {label_b}={pk_b:.0f}  "
          f"(loudness ratio {label_b}/{label_a} = "
          f"{(pk_b / pk_a) if pk_a else float('nan'):.3f})")
    print(f"  rms:     {label_a}={rms_a:.1f}  {label_b}={rms_b:.1f}")
    if len(a) < 16 or len(b) < 16:
        print("  ! too little audio to align — is the window during silence?")
        return

    max_lag = int(rate * max_lag_ms / 1000)
    lag, peak_r = xcorr_align(a, b, max_lag=max_lag)
    lag_ms = lag / rate * 1000.0
    print(f"\n  [1] xcorr align : best lag = {lag} samples "
          f"({lag_ms:+.2f} ms), peak r = {peak_r:.4f}")

    a_aln, b_aln = apply_lag(a, b, lag)
    r = pearson(a_aln, b_aln)
    na, nb = _normalize(a_aln), _normalize(b_aln)
    rmse = float(np.sqrt(np.mean((na - nb) ** 2)))
    print(f"  [2] post-align  : Pearson r = {r:.4f}  "
          f"normalised-RMSE = {rmse:.4f}  (overlap {len(a_aln)} samp)")

    ta, _, _ = detect_onsets(a, rate)
    tb, _, _ = detect_onsets(b, rate)
    deltas, na_on, nb_on = onset_histogram(ta, tb)
    print(f"  [3] onsets      : {label_a}={na_on}  {label_b}={nb_on}  "
          f"matched={len(deltas)}")
    if len(deltas):
        edges = [-30, -20, -10, -5, -2, 2, 5, 10, 20, 30]
        hist, _ = np.histogram(deltas, bins=edges)
        print(f"      onset dt (ms): mean={deltas.mean():+.2f} "
              f"median={np.median(deltas):+.2f} std={deltas.std():.2f} "
              f"max|d|={np.max(np.abs(deltas)):.2f}")
        print(f"      histogram {edges[0]}..{edges[-1]} ms: {hist.tolist()}")

    pa = pitch_track(a, rate)
    pb_ = pitch_track(b, rate)
    # align pitch tracks by the same hop-scaled lag
    cents, nv = pitch_cents_error(pa, pb_)
    if nv:
        print(f"  [4] pitch error : voiced frames={nv}  "
              f"mean={cents.mean():+.1f}c median={np.median(cents):+.1f}c "
              f"std={cents.std():.1f}c  max|d|={np.max(np.abs(cents)):.1f}c")
    else:
        print("  [4] pitch error : no commonly-voiced frames "
              "(percussive/PCM chime — pitch metric N/A)")


def psg_bitcheck(src_a, src_b, label_a="A", label_b="B"):
    print(f"\n=== PSG PER-CHANNEL BIT-CHECK: {label_a} vs {label_b} ===")
    any_data = False
    for ch in ("ch1", "ch2", "ch3", "ch4"):
        a = src_a.get(ch, np.zeros(0, np.int16))
        b = src_b.get(ch, np.zeros(0, np.int16))
        n = min(len(a), len(b))
        if n == 0:
            print(f"  {ch}: no data")
            continue
        a, b = a[:n], b[:n]
        if not a.any() and not b.any():
            print(f"  {ch}: silent on both (no PSG activity in this window)")
            continue
        any_data = True
        diff = a.astype(np.int32) - b.astype(np.int32)
        nz = np.nonzero(diff)[0]
        if nz.size == 0:
            print(f"  {ch}: BIT-EXACT over {n} samples OK")
        else:
            print(f"  {ch}: DIVERGES — first @ sample {int(nz[0])}, "
                  f"{nz.size} differing, max|d|={int(np.max(np.abs(diff)))}")
    if not any_data:
        print("  (no PSG channel active in this window — use a PSG-driven ROM"
              " / scene, e.g. a title or menu theme)")


# ── CLI ────────────────────────────────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--recomp-port", type=int, default=19842)
    ap.add_argument("--recomp2-port", type=int, default=0,
                    help="second recomp source (e.g. interpreter) for PSG bitcheck")
    ap.add_argument("--nba-port", type=int, default=0,
                    help="NanoBoyAdvance oracle port (e.g. 19844)")
    ap.add_argument("--frames", type=int, default=150)
    ap.add_argument("--count", type=int, default=40000)
    ap.add_argument("--start", type=int, default=None,
                    help="absolute capture-ring start index (default: most "
                         "recent `count`). Use 0 to grab the BIOS-chime window.")
    ap.add_argument("--self", action="store_true",
                    help="sanity: compare the recomp stream to itself")
    ap.add_argument("--psg-bitcheck", action="store_true")
    ap.add_argument("--quit", action="store_true", help="quit servers when done")
    args = ap.parse_args()

    print(f"[capture] recomp port {args.recomp_port}: "
          f"{args.frames} frames, {args.count} samples ...")
    rec = capture_recomp(args.recomp_port, args.frames, args.count,
                         quit_after=args.quit and not args.self, start=args.start)
    print(f"[capture] recomp rate={rec['rate']} mixed={len(rec['mixed'])} "
          f"mixed_nonzero={int(np.count_nonzero(rec['mixed']))}")

    if args.self:
        compare_mixed(rec["mixed"], rec["mixed"].copy(), rec["rate"],
                      "recomp", "recomp(self)")
        return

    if args.nba_port:
        print(f"[capture] nba port {args.nba_port}: {args.frames} frames ...")
        nba = capture_nba(args.nba_port, args.frames, args.count,
                          quit_after=args.quit, start=args.start)
        print(f"[capture] nba rate={nba['rate']} mixed={len(nba['mixed'])} "
              f"mixed_nonzero={int(np.count_nonzero(nba['mixed']))}")
        if rec["rate"] != nba["rate"]:
            print(f"  ! rate mismatch recomp={rec['rate']} nba={nba['rate']} "
                  f"— resample before comparing (TODO); proceeding raw")
        compare_mixed(rec["mixed"], nba["mixed"], rec["rate"], "recomp", "nba")

    if args.recomp2_port:
        print(f"[capture] recomp2 port {args.recomp2_port}: {args.frames} frames ...")
        rec2 = capture_recomp(args.recomp2_port, args.frames, args.count,
                              quit_after=args.quit)
        compare_mixed(rec["mixed"], rec2["mixed"], rec["rate"],
                      "recomp", "recomp2")
        if args.psg_bitcheck:
            psg_bitcheck(rec, rec2, "recomp", "recomp2")
    elif args.psg_bitcheck:
        print("\n[psg-bitcheck] needs --recomp2-port (a second capture source"
              " exposing the per-channel ring, e.g. the interpreter oracle).")

    if not args.nba_port and not args.recomp2_port:
        print("\n(no oracle port given — captured recomp only. Add --nba-port "
              "for the drift comparison or --self for a sanity check.)")


if __name__ == "__main__":
    sys.exit(main())
