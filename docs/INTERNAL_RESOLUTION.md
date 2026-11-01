# Higher internal resolution

Games can authorize finer affine-background sampling with
`RunOptions::high_resolution_affine_provider`. The first pilot is Mario Kart:
Super Circuit's mode-1 BG2, including the pre-HUD course fly-in. This is a local presentation enhancement, disabled
at 1x by default. It does not add detail to original sprites or HUD artwork.

Use the game's optional mod or `--internal-resolution 2|3|4|8|16|32|64|128`. The engine
accepts integer scales from 1 through 128. A capable game
also exposes resolution scale in its in-game display settings. The CLI overrides
the initial mod selection. `[video] internal_resolution = 1` sets the ordinary
configuration default. Unsupported games reject an enhancement request.

## Raster and ownership

`GbaHighResolutionFrame` consumes the effective affine references, current
VRAM/palette and layer candidates at each real published raster row. It samples
the approved BG at fractional positions and reinserts it into the priority,
window, alpha and brightness stack. Other layers retain their original samples.
Foreign presentation overlays use the original composed row instead.

Within a row, vertical subpixel transforms extrapolate from the previous
published transform. Mode/layout changes, reset, restore and new frames break
that history. This is a presentation approximation; it neither reruns HBlank
DMA nor guesses future VRAM. At original pixel positions the unfiltered samples
remain exact. The existing optional affine filter applies to enhanced samples.

The enhanced buffers, interpolation history and counters are excluded from
canonical serialization. Native rendering, guest reads, timers, interrupts and
CPU execution retain their existing path. A full raster must complete before
the enhanced buffer becomes available; partial restored/resized frames use the
ordinary output until a complete frame is ready.

SDL's sampled texture dimensions are separate from logical screen dimensions.
A 382x160 adaptive view at 4x uploads 1528x640 directly. Aspect/layout/touch
coordinates remain in logical units. Color grading operates on the sampled
texture; the native sharp-prescale stage is unnecessary for enhanced textures.
The browser host keeps its existing logical-size shared-memory slots and
samples the supplied raster back onto that native grid. Its overload accepts
the sampled stride correctly; enhanced-density browser display awaits a
separate shared-memory protocol extension. Desktop SDL presents the full raster
when supported by its renderer. If a requested surface exceeds the GPU's
texture limit or allocation fails, the host reduces only the upload until it
fits, logs the effective size, and remembers it for subsequent frames. The
CPU raster, logical geometry and canonical state are unchanged.

Rendering work and surface memory grow with the square of the scale. Relative
to 4x, 8x has four times the pixels and 16x has sixteen times the pixels.
Up to 4x is recommended. Every in-game scale above 4x is labeled
`(experimental)` and may run very slowly. Performance depends on hardware, view
width and scene; higher scales do not guarantee full speed. At native aspect,
16x renders 3840x2560; a 284x160 widescreen view
renders 4544x2560. Sprite and HUD artwork remains native in all cases.

Netplay uses the existing read-only presentation observer and private mirror.
Each canonical PPU remains native. Local scale and window geometry do not enter
session snapshots or hashes. Rollback invalidates presentation history. This
does not authorize incompatible simulation builds to connect.

## Build selection and optimization

`GBARECOMP_SCANLINE_IMPLEMENTATION=HLE` selects the tile-span/native-color
compositor by default. `LLE` retains the original compositor in maintained
source. This is an exact optimization despite its historical HLE name, and is
independent of internal resolution. The choice enters strict netplay source
identity. No player-facing HLE/LLE switch or dual execution is required.

Enhanced row setup hoists signed coordinate divisions and subpixel offsets out
of the pixel loop. Each quotient retains C++ signed truncation in the original
order. Per-pixel coordinates advance by addition. This uses portable C++, with
no CPU-specific dependency or floating-point result.

`GBARECOMP_HD_SAMPLER_IMPLEMENTATION=HLE` selects the persistent indexed-surface
sampler by default for titles consuming this engine revision. `LLE` builds the
maintained direct tile/map/palette sampler. Existing CMake caches retain their
explicit selection. Backend selection does not change a title's optional
enhancement or initial resolution settings. Startup
reports `hd_sampler_backend`; the choice also enters strict source identity.
There is no runtime HLE/LLE control or state handoff. This choice is independent
of the native compositor selection above.

The HLE surface expands the affine tilemap into a linear array of palette
indices. At every published row it compares the relevant character and map
bytes with its private snapshots, then updates cells whose map entry or source
tile changed. It refreshes palette colors every row, including palette bit 15.
Direct memory and DMA writes are observed without relying on bus write hooks.
Layout changes and invalidation discard cached assumptions. None of this cache
is serialized or consulted by guest reads.

Filtered samples use three independent 21-bit lanes in a portable unsigned
64-bit integer. Each lane's maximum weighted sum plus rounding is below
2^21, so the channels cannot carry into one another. Four multiplies replace
twelve while retaining integer rounding, transparency and edge behavior.
Rows without enhanced affine content are expanded horizontally once and copied
to their remaining subrows. Both optimizations belong to the HLE build choice.

The original 4x pilot rejected a different, per-row decoded tile cache. That
result does not assess this persistent surface. A GPU affine service remains
a further opportunity if the measured CPU path cannot meet the desired scale.

## Validation

Build `high_resolution_tests`, `bus_tests` and `multiplayer_view_tests` under
both sampler and compositor selections. The focused raster fixture checks
2x/3x/4x/8x/16x samples,
extra texels absent at native density, original sample parity, HUD/alpha,
negative margins, per-row character/map/palette and layout changes, canonical device-state equality
and reset/partial-raster/resize behavior. The mirror fixture checks independent
local dimensions and canonical state equality through restore.

Opted-in titles can build `<title>ViewProbe` and supply ROM, BIOS, raw paired
session state and an output prefix. It compares every canonical session byte
for native/wide/adaptive views and supported enhanced scales, including a
twelve-frame replay after restore. This is a local deterministic differential,
not a transport test or cross-build/platform qualification.

Bounded runtime diagnostics distinguish `complete_frames`/`affine_lines`/
`samples` from `hd_presented`. Headless raster execution does not establish
actual window presentation. `--uncapped` removes pacing while retaining normal
render/audio work. `--dump-state` writes an explicit final diagnostic snapshot;
`--dump-png` captures the completed enhanced raster. Keep automated audio muted.
The TCP command `presentation_screenshot` inspects the completed enhanced raster
and its sampled dimensions/counters. `screenshot` keeps its native accuracy
surface. Neither query changes simulation state.

See the title's `docs/INTERNAL_RESOLUTION.md` for the measured pilot, fixture
identity, reproducible command and remaining limits.

## Extreme experimental scales

128x is opt-in and unqualified for full-size performance or visual quality.
At native aspect it requests 30720x20480 pixels (629 million pixels). RGBA8
pixel storage alone is about 2.34 GiB per GPU target, before other resources;
widescreen increases that cost. A CPU RGB buffer is about 1.76 GiB, and the
portable renderer may need more than one buffer.

The selected value is the requested sampling scale. Existing GPU texture and
allocation limits can reduce the actual output, which is recorded in the
renderer log. The portable fallback remains available but can be extremely
slow and memory intensive. Choose a lower scale if resources are insufficient.
The 128x validation covers admission, metadata-only capture, allocation-size
arithmetic and shader overflow rejection without allocating a full-size frame;
it does not establish a successful full-resolution 128x game session.
