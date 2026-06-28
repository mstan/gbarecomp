#!/usr/bin/env python3
"""nba_common.py -- shared plumbing for the per-axis recomp-vs-NBA drift
comparators (GBA accuracy burndown).

Modeled on oracle/diff_audio_drift.py's `Client` + hex decode helpers, factored
out so the five comparators (diff_mmio_nba / diff_cycle_nba / diff_irq_nba /
diff_video_nba / determinism_probe) share ONE validated TCP + parsing layer.

This module is measurement-only and READ-ONLY against both servers. It never
pauses/steps two emulators into lockstep to "synchronize" them: each side
free-runs and fills its own always-on ring; the comparators QUERY a window.

Servers
-------
* recomp (:19842) -- the gbarecomp runtime (game build) or bios_smoke. Decimal
  JSON fields; per-region read cmds (read_vram/read_pal/read_oam/...). mmio_cap
  returns entries[]; irq_cap returns entries[] (TAKE-time only); cyc_anchor
  queries the always-on insn-fingerprint ring (needs GBARECOMP_INSN_TRACE=1 and
  the per-game build).
* nba (:19844) -- the NanoBoyAdvance oracle. Hex-packed parallel arrays for
  mmio_cap/irq_cap; cyc_anchor/run_to_pc actively free-run to collect hits;
  read{region,addr,len}; raise+take IRQ ring.

Everything degrades gracefully: a missing server / absent command yields a clear
one-line message and `None`, never a stack trace.
"""
from __future__ import annotations

import json
import socket
import struct
import time


# -- TCP plumbing -----------------------------------------------------------
class Client:
    """Line-delimited JSON request/response over TCP (same contract as the
    other oracle scripts). Retries briefly on connect so a just-spawned peer
    that is still binding is tolerated."""

    def __init__(self, host, port, timeout=15.0, connect_deadline=8.0):
        deadline = time.time() + connect_deadline
        last = None
        self.sock = None
        while time.time() < deadline:
            try:
                self.sock = socket.create_connection((host, port), timeout=timeout)
                break
            except OSError as e:
                last = e
                time.sleep(0.1)
        if self.sock is None:
            raise ConnectionError(f"can't reach {host}:{port}: {last}")
        self.sock.settimeout(timeout)
        self.buf = b""
        self.host = host
        self.port = port

    def call(self, timeout=None, **kw):
        line = (json.dumps(kw) + "\n").encode()
        self.sock.sendall(line)
        if timeout is not None:
            self.sock.settimeout(timeout)
        try:
            while b"\n" not in self.buf:
                chunk = self.sock.recv(1 << 16)
                if not chunk:
                    raise ConnectionError("peer closed the connection")
                self.buf += chunk
        finally:
            self.sock.settimeout(None)
        out, _, self.buf = self.buf.partition(b"\n")
        return json.loads(out.decode())

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def connect(host, port, label, timeout=15.0, connect_deadline=8.0):
    """Best-effort connect. Returns a Client or None (printing a clear reason),
    so a comparator can run one side / explain the absence instead of crashing.
    """
    try:
        c = Client(host, port, timeout=timeout, connect_deadline=connect_deadline)
        return c
    except (OSError, ConnectionError) as e:
        print(f"[skip] {label} on {host}:{port} unreachable -- {e}")
        return None


def ok(resp):
    return isinstance(resp, dict) and resp.get("ok")


# -- hex decoders (little-endian, matching both servers' json_emit_hex) ------
def hx(h):
    return bytes.fromhex(h) if h else b""


def hx_u8(h):
    return list(hx(h))


def hx_u16(h):
    b = hx(h)
    return list(struct.unpack(f"<{len(b)//2}H", b)) if b else []


def hx_u32(h):
    b = hx(h)
    return list(struct.unpack(f"<{len(b)//4}I", b)) if b else []


def hx_u64(h):
    b = hx(h)
    return list(struct.unpack(f"<{len(b)//8}Q", b)) if b else []


# -- IRQ source naming (matches oracle/irq_sources.py) -----------------------
IRQ_SRC = {0: "VBlank", 1: "HBlank", 2: "VCount", 3: "TM0", 4: "TM1", 5: "TM2",
           6: "TM3", 7: "Serial", 8: "DMA0", 9: "DMA1", 10: "DMA2", 11: "DMA3",
           12: "Key", 13: "GPak"}


def src_name(mask):
    """Name an IE&IF / IF-bit mask. Multiple bits -> '+'-joined (the recomp src
    field can carry several); single bit is the common case."""
    if mask == 0:
        return "none"
    bits = [IRQ_SRC.get(b, f"bit{b}") for b in range(16) if mask & (1 << b)]
    return "+".join(bits) if bits else f"0x{mask:x}"


# -- MMIO record normalization ----------------------------------------------
# Canonical record tuple: (addr, value, size, cycle, pc). Comparators key on
# (addr,value,size) -- offset-free -- and treat cycle/pc as annotations.
def pull_mmio_nba(c, frames_already_run=True, max_records=262144):
    """Pull NBA's MMIO write ring oldest-first as a list of records.
    Returns (records, wrapped) where wrapped=True if the ring evicted the
    cold-reset prefix (oldest abs index > 0)."""
    head_probe = c.call(cmd="mmio_cap", count=1)
    if not ok(head_probe):
        return None, False
    head = int(head_probe.get("head", 0))
    oldest = head - min(head, max_records)
    wrapped = oldest > 0
    out = []
    start = oldest
    while start < head:
        n = min(max_records, head - start)
        r = c.call(cmd="mmio_cap", start=start, count=n)
        if not ok(r) or int(r.get("count", 0)) == 0:
            break
        cnt = int(r["count"])
        addr = hx_u32(r.get("addr", ""))
        val = hx_u32(r.get("val", ""))
        cyc = hx_u64(r.get("cyc", ""))
        pc = hx_u32(r.get("pc", ""))
        size = hx_u8(r.get("size", ""))
        for k in range(cnt):
            out.append((addr[k], val[k], size[k], cyc[k], pc[k]))
        start = int(r.get("first", start)) + cnt
    return out, wrapped


def pull_mmio_recomp(c, page=65536):
    """Pull the recomp MMIO write ring oldest-first. Returns (records, wrapped)."""
    probe = c.call(cmd="mmio_cap", count=1)
    if not ok(probe):
        return None, False
    total = int(probe.get("total", 0))
    oldest = int(probe.get("oldest", 0))
    wrapped = oldest > 0
    out = []
    start = oldest
    while start < total:
        r = c.call(cmd="mmio_cap", start=start, count=page)
        if not ok(r):
            break
        ents = r.get("entries", [])
        if not ents:
            break
        for e in ents:
            out.append((e["addr"], e["value"], e["size"], e["cycle"], e["pc"]))
        start = int(r.get("first", start)) + len(ents)
    return out, wrapped


# -- region read (full surface, chunked) ------------------------------------
REGION_RECOMP = {  # name -> (read_cmd, base, size)
    "vram":  ("read_vram", 0x06000000, 96 * 1024),
    "pal":   ("read_pal",  0x05000000, 1024),
    "oam":   ("read_oam",  0x07000000, 1024),
    "iwram": ("read_iwram", 0x03000000, 32 * 1024),
    "ewram": ("read_ewram", 0x02000000, 256 * 1024),
}
# NBA uses 'pram' for palette RAM; map our 'pal' onto it.
REGION_NBA = {"vram": "vram", "pal": "pram", "oam": "oam",
              "iwram": "iwram", "ewram": "ewram"}


def read_region_recomp(c, name, chunk=4096):
    cmd, base, size = REGION_RECOMP[name]
    out = bytearray()
    for off in range(0, size, chunk):
        n = min(chunk, size - off)
        r = c.call(cmd=cmd, addr=base + off, len=n)
        if not ok(r):
            return None
        out += hx(r["data"])
    return bytes(out)


def read_region_nba(c, name, chunk=4096):
    region = REGION_NBA[name]
    _, _, size = REGION_RECOMP[name]
    out = bytearray()
    for off in range(0, size, chunk):
        n = min(chunk, size - off)
        r = c.call(cmd="read", region=region, addr=off, len=n)
        if not ok(r):
            return None
        out += hx(r["data"])
    return bytes(out)


def first_diff(a, b):
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return i
    if len(a) != len(b):
        return n
    return None


# -- PPU compositor-register reconstruction (phase-alignment gate) -----------
# The framebuffer is only a meaningful recomp-vs-oracle comparison when the
# COMPOSITOR INPUTS match on both sides. Many of those inputs (BGxHOFS/VOFS,
# BG2/3 affine, WINx coords, MOSAIC, BLDY) are WRITE-ONLY -- reading them back
# returns open-bus garbage -- so we reconstruct the effective value from the
# always-on MMIO write stream instead of read-backs. (Per the burndown Axis-5
# resolution: an apparent pixel "drift" during a per-frame BLDALPHA fade ramp
# was a capture-phase artifact, not a compositor bug; gate on input parity.)
PPU_CTRL_LO = 0x04000000
PPU_CTRL_HI = 0x04000058  # DISPCNT(0x00) .. BLDY(0x54)+2
# Bytes to ignore in the shadow: DISPSTAT flag bits (0x04 low 3 bits are
# read-only HW status) and VCOUNT (0x06, read-only). We compare written state;
# the game writes identical values both sides, but VCOUNT is never written and
# DISPSTAT's status bits aren't write-driven, so exclude them defensively.
PPU_SHADOW_IGNORE = {0x06, 0x07}

PPU_REG_NAMES = {
    0x00: "DISPCNT", 0x04: "DISPSTAT", 0x08: "BG0CNT", 0x0A: "BG1CNT",
    0x0C: "BG2CNT", 0x0E: "BG3CNT", 0x10: "BG0HOFS", 0x12: "BG0VOFS",
    0x14: "BG1HOFS", 0x16: "BG1VOFS", 0x18: "BG2HOFS", 0x1A: "BG2VOFS",
    0x1C: "BG3HOFS", 0x1E: "BG3VOFS", 0x20: "BG2PA", 0x22: "BG2PB",
    0x24: "BG2PC", 0x26: "BG2PD", 0x28: "BG2X", 0x2C: "BG2Y", 0x30: "BG3PA",
    0x38: "BG3X", 0x3C: "BG3Y", 0x40: "WIN0H", 0x42: "WIN1H", 0x44: "WIN0V",
    0x46: "WIN1V", 0x48: "WININ", 0x4A: "WINOUT", 0x4C: "MOSAIC",
    0x50: "BLDCNT", 0x52: "BLDALPHA", 0x54: "BLDY",
}


def mmio_shadow(records, lo=PPU_CTRL_LO, hi=PPU_CTRL_HI):
    """Replay an oldest-first MMIO write-record list into a byte image of the
    [lo,hi) IO sub-range. Returns (img, seen) where seen marks bytes ever
    written. Handles 8/16/32-bit writes (later writes overwrite)."""
    span = hi - lo
    img = bytearray(span)
    seen = bytearray(span)
    for addr, value, size, cyc, pc in records:
        for k in range(size):
            a = addr + k
            if lo <= a < hi:
                img[a - lo] = (value >> (8 * k)) & 0xFF
                seen[a - lo] = 1
    return bytes(img), bytes(seen)


def ppu_input_parity(rec_records, nba_records):
    """Compare the reconstructed PPU compositor-register state of both sides.
    Returns (aligned: bool, diffs: list[(off, name, rec_word, nba_word)]).
    A frame is input-aligned when every written control byte matches."""
    ri, rs = mmio_shadow(rec_records)
    ni, ns = mmio_shadow(nba_records)
    diffs = []
    span = len(ri)
    off = 0
    while off < span:
        if off in PPU_SHADOW_IGNORE:
            off += 1
            continue
        if (rs[off] or ns[off]) and ri[off] != ni[off]:
            # Report at 16-bit register granularity for readability.
            reg = off & ~1
            rw = ri[reg] | (ri[reg + 1] << 8) if reg + 1 < span else ri[reg]
            nw = ni[reg] | (ni[reg + 1] << 8) if reg + 1 < span else ni[reg]
            name = PPU_REG_NAMES.get(reg, f"0x{reg:02x}")
            if not diffs or diffs[-1][0] != reg:
                diffs.append((reg, name, rw, nw))
            off = reg + 2
            continue
        off += 1
    return (len(diffs) == 0), diffs
