# oracle/ — mGBA-backed reference emulator

This subdirectory builds a SEPARATE binary (`gbarecomp_oracle.exe`) that
embeds libmgba and exposes the `emu_*` / `read_emu_*` TCP command set
defined in `../TCP.md`. The native gbarecomp build never links against
this — libmgba is MPL-2.0 and our hard rule is "zero copyleft emulator
dependencies in the native build."

## Dependency

`third_party/mgba/` (cloned locally, gitignored, pinned to tag 0.10.5).
See `../third_party/README.md` for license notes.

## Build

The oracle target is opt-in. Enable with
`cmake -B build -DGBARECOMP_BUILD_ORACLE=ON ..` after building libmgba
in `third_party/mgba/build/`.

## TCP

Default port: `127.0.0.1:19843` (one above the native debug port).
Same line-delimited JSON request/response shape as `tcp_debug_server`.

## Usage from a diff harness

The native side runs `bios_smoke.exe --frames N` and dumps OAM/VRAM/PAL.
The oracle is told `emu_step_to_vblank` until its own VBlank-IRQ count
equals N (sync via hardware event, never raw frame index), then asked
for OAM/VRAM/PAL. A small diff tool compares the two byte-for-byte and
prints the first divergence.
