# Native presentation upload

Campaign opportunity G16, owning Bead `beads-pc0o`, coordinating
`beads-2wy8.1`. The candidate build default is HLE. Per-game measurements are
for the full optimization package; no isolated upload speedup is claimed.

`GBARECOMP_PRESENTATION_IMPLEMENTATION=LLE|HLE` is selected at build time.
LLE retains the established RGB888 grading scratch and SDL RGB24 upload.
HLE selects an advertised native ARGB8888 or ABGR8888 texture once when the
renderer opens, then combines the same color lookup and opaque texture packing
in one portable C++ pass. This avoids the intermediate graded RGB24 image and
the extra format conversion expected when RGB24 is not a native renderer
format. Whole-game CPU and elapsed-runtime measurements are recorded separately
in the campaign dataset, with the actual graphics API and device identified.
The second candidate also prepares the selected packed color table once when
the screen model is created: each graded output pixel becomes one indexed
32-bit load/store. It uses an additional 128 KiB for a colored native-texture
window; Raw and LLE do not allocate that table. The cache is immutable and
owned by its screen settings, so it needs no frame polling or invalidation.

The input remains the original read-only RGB888 PPU/presentation surface. Raw
retains all eight source bits, including filtered HD colors. Screen simulation
recovers the same upper-five-bit LUT index and emits exactly the original
graded channels. Alpha is 255 and texture blending is disabled. Native SDL
pixel values use integer channel definitions, without host-endian byte casts,
unaligned loads, assembly or CPU feature assumptions.

No guest computation, audio, time, canonical pixel, snapshot, view geometry,
sampling density, resize or UI behavior is intentionally changed. Existing
large-texture reduction occurs before the fused conversion and keeps its
established source selection. Resize/enhanced texture replacement retains the
selected format. If a native format is unavailable or its initial allocation
fails, this host keeps the working RGB24 route. SDL-free and web hosts retain
their existing implementations. There is no new player-facing backend selector.

The selection enters compatibility identity conservatively; this is not a
cross-build netplay qualification claim. Startup logs identify the selected
package and the SDL window logs the actual texture format and packing choice.

Focused checks compare all 32,768 hardware colors plus non-hardware low-bit
variants for every screen model and both display targets. An SDL dummy-video
software-renderer check compares actual RGB24/ARGB8888/ABGR8888 uploads and
readback, with no visible window or sound. These checks passed in the initial
HLE and LLE builds (two checks each). The HLE selection also compiles SDL-free
host-window/runtime objects and passes the color check. Current-title full
continuations, current-build save/load, adaptive resize and HD checks pass.
Actual hidden Direct3D11 and NVIDIA GTX760 OpenGL readback also match for all
screen models and both native formats. Representative built-game OpenGL
comparisons preserve full state/images and show no reproducible regression;
noisy results remain inconclusive. See the campaign's exact source/dependency
identities and case dispositions rather than attributing a bundle gain to G16.
