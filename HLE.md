# GBA HLE implementation policy

The shared [HLE policy](../../recomp-template/HLE.md) applies verbatim.
Use [OPTIMIZATION.md](../../recomp-template/OPTIMIZATION.md) to distinguish
exact host-cost removal, exact replacement and deliberate approximation.

Maintain the functioning real-BIOS/guest/device LLE configuration in current
source. Developers choose replacements at build time, per component. Qualified
HLE may be the preferred build. Define actual caller inputs, results, required
registers/memory, continuation, completion and ordering. Whole services and
different private state are allowed; individual original instructions, scratch
buffers and internal stalls need not be reproduced. Small practically
unnoticeable differences are allowed for substantial measured benefit.

No live HLE/LLE switching, continuous comparison, duplicate LLE state, automatic
per-call fallback, or cross-build savestate compatibility is required. Existing
legacy BIOS flags and audio shadows are not the architecture for new services.
Retain them truthfully until separately migrated; they are not newly qualified
by this documentation update.

`GBARECOMP_SCANLINE_IMPLEMENTATION=HLE|LLE` selects the shared compositor at
configure time. HLE uses tile-span decoding, per-line window preparation and
native-color candidate composition. This replacement preserves the same pixel
and timing contract; it needs exact comparisons despite its historical HLE name.
LLE retains the original per-pixel renderer. The build selection enters netplay
build identity; no cross-build compatibility is inferred from compilation.

Internal resolution is a separate optional presentation feature. It leaves the
native framebuffer, guest memory, emulated time and simulation state unchanged.
Host pixels, surface allocation and interpolation history are not serialized
into canonical snapshots. Reset/restore/resize invalidates incomplete enhanced
frames. Unsupported scenes retain their ordinary rendered content.
See [the implementation and validation guide](docs/INTERNAL_RESOLUTION.md).

`GBARECOMP_HD_SAMPLER_IMPLEMENTATION=HLE|LLE` selects the enhanced affine
sampler. The qualified cached HLE sampler is the default; the direct tiled LLE
sampler remains buildable. This backend preference applies to consuming titles
without enabling enhanced resolution automatically. The owner approved the
measured MKSC Windows gain; other titles and platforms do not inherit that
specific performance qualification merely by selecting the same backend.

For netplay, follow [NETPLAY.md](../../recomp-template/NETPLAY.md). Qualified
peers agree on simulation semantics and actual capabilities before frame 0;
host clocks, thread completion and GPU readiness must never decide guest
results. Local quality can differ only while entirely downstream of simulation.
Do not assume HLE/LLE cross-play or expose unqualified replacements online.

Use focused tests, meaningful boundary comparisons and bounded game samples.
Measure actual emulation/presentation work per useful frame. Record limitations
and failed experiments; a smaller instruction count alone is not a speedup.

`GBARECOMP_BIOS_MATH_IMPLEMENTATION=LLE|HLE` selects a qualification package for
the complete Div, DivArm and ArcTan2 operations. It defaults to LLE. The HLE
build uses the existing native algorithms directly before BIOS exception entry,
independently of the full legacy catalog and boot-intro policy. Zero-denominator
division declines untouched to the real BIOS; other services, including affine,
retain the original route unless the separate grandfathered catalog is enabled.
Sliced link and shadow owners retain BIOS execution until they grant a complete
native-service window; no native-math link speedup is claimed.
There is no new runtime math selector or reference execution beneath a handled
math call. The selection enters source compatibility identity; mixed builds
are not qualified peers. The inherited nominal timing model remains approximate.
Original-BIOS caller tests and actual matched game behavior/cost qualification
are required before adopting this package; merely compiling it is not a gain.
