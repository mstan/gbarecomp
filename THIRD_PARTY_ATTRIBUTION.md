# Third-Party Attribution

Portions of this project are ported from other open-source projects,
used with permission and under their licenses. Facts (struct offsets,
byte signatures, documented hardware/driver behavior) and, where noted,
ported logic are credited below. License texts of permissively-licensed
upstreams are reproduced in their repositories.

## JRickey/gba-recomp

- **Upstream:** https://github.com/JRickey/gba-recomp
- **Author:** Jrickey
- **License:** MIT OR Apache-2.0 (used with the author's permission)

| Our file | Upstream source | What was ported |
|---|---|---|
| `src/gba/gba_m4a.{h,cpp}` | `crates/gba-core/src/mp2k.rs` | MP2K ("m4a") SDK driver detection (SoundMain literal-pool signature + CRC fallback) and the SoundInfo / SoundChannel / WaveData guest-struct offset map. Re-implemented in C++; the live-state dump path is original to this project. |
| `src/runtime/color_lut.{h,cpp}` | `crates/screen/src/{color,profile,lut}.rs` | Screen-color simulation: CIE colorimetry core, measured per-revision panel models, and the BGR555→RGBA8 LUT build. Re-implemented in C++; the present-time RGB888-input apply path is ours. |
| `src/gba/gba_rtc.{h,cpp}` | `crates/gba-core/src/{rtc,hostclock}.rs` | Cartridge GPIO port + S-3511A RTC (serial state machine, BCD date/time, Seiko signature detection) and the civil↔linear host-clock helpers (Hinnant day algorithms, public domain). Re-implemented in C++ (Win32 `GetLocalTime` / POSIX `localtime_r`). |
| `src/gba/audio_shadow.{h,cpp}` | `crates/gba-core/src/shadow.rs` | Engine-agnostic HLE-shadow differential verifier: envelope-correlation self-check vs the canon FIFO stream, probation auto-gain, prove/strike/pause-and-reprobe. Re-implemented in C++. (Permitted under PRINCIPLES.md "Verified-enhancement HLE".) |
| `src/gba/mp2k_shadow.{h,cpp}` | `crates/gba-core/src/mp2k.rs` | MP2K HLE shadow mixer: float voice re-render (envelope mirror, PCM/DPCM sampling, intra-tick gain interpolation, reverb ring) driving the differential verifier. Re-implemented in C++. |

Each ported file carries an attribution header pointing here. Ported
code remains under the upstream's MIT OR Apache-2.0 terms; this notice
and those headers satisfy the attribution requirement.

## mGBA

- **Upstream:** https://github.com/mgba-emu/mgba (vendored at `third_party/mgba`)
- **Author:** Jeffrey Pfau and contributors
- **License:** MPL-2.0

| Our file | Upstream source | What was ported |
|---|---|---|
| `src/runtime/bios_hle.{h,cpp}` | `src/gba/bios.c` | GBA BIOS SWI High-Level Emulation: the Div/Sqrt/ArcTan/ArcTan2 fixed-point routines and stall formulas, the LZ77 / Huffman / run-length / diff-unfilter decompressors, BitUnPack, the (float) affine-matrix builders, and MidiKey2Freq. Re-implemented in C++ against this project's `g_cpu` + bus bridge. HLE is opt-in; LLE (the recompiled real BIOS) remains the default and the correctness oracle (PRINCIPLES.md "verified-enhancement HLE"). |

Portions of `src/runtime/bios_hle.cpp` are derived from mGBA and remain
subject to the MPL-2.0; the upstream source is vendored under
`third_party/mgba/src/gba/bios.c`, satisfying the license's source-availability
requirement.
