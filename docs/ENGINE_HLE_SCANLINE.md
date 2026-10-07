# Shared scanline HLE experiment

This draft adds a fixed build-time `GBARECOMP_SCANLINE_IMPLEMENTATION=LLE|HLE`
selection, default LLE, for the same shared scanline caller and RGB destination.
It does not intercept title addresses or alter DMA, IRQ or guest scheduling.

## Replaced engine work

Native HLE resolves row window controls once, decodes ordinary background
map/tile rows in spans of at most eight pixels, and retains composition
candidates in native GBA 555 color until final RGB publication. Opaque candidates
hidden by higher layers avoid intermediate RGB expansion. Blend arithmetic
continues in 555 color. No cross-row cache or invalidation protocol is introduced.

Adaptive wide rendering uses the same native-color composition replacement.
Every existing per-pixel margin, remap and authored tile provider call remains
in place. Wide row windows were already resolved once; the native tile-span
optimization is not applied to arbitrary remapped provider samples.

Only `gba_ppu.cpp` and the runtime banner source receive the selection macro.
Generated guest code and the callable ABI are identical between arms. Runtime
`--uncapped` disables pacing while preserving ordinary SDL presentation and
normal audio drain/mix/push. A steady-clock bounded-window row measures only
the gameplay loop, excluding startup, state loading and cleanup/save/PNG.

## Evidence and limits

Actual MMZ discovery found roughly 19% main-thread renderer samples and 15%
tick/yield samples. These are coarse historical attribution, not a benchmark.
The provisional primary-route material goal was roughly 10% total work reduction.

A first functioning MKSC pair used title `79dffea`, framework `e3c834d` and core
`c626f4e`, with verified ROM and maintained real-BIOS configuration. Cold boot
reached an actual Luigi/Mushroom Cup race and saved an in-session state. Both
native arms completed frames 10016 to 11215 and 1200 presents, reporting the
same steps, cycles and final PC with zero dispatch misses/interpreted instructions,
unmapped accesses and unhandled I/O. LLE loop time was 2.503480 s; HLE was
2.194235 s: 14.09% more present FPS, 12.35% less loop time. One HLE race image
was clear. A concurrent foreign PSX build was recorded; this is one route/pair,
not an isolated-host or universal gain claim. Ordinary audio work was preserved
but sample counts were not separately instrumented. No old-frame comparison
or gameplay matrix was required.

The integration preserves current MKSC title `b89fbb4` and its framework base
`29efc028`, including upstream saving, checkpoint and speedometer fixes.
That current-pin qualification and adaptive confirmation are separate from
the historical pair above. Exact binaries, hashes, run commands and raw logs
are retained in the private local review manifest, never generated game assets
or ROMs in the repository. Owner play and default promotion remain pending.

Earlier rejected MMZ attempts and the first MKSC setup exposed an omitted
`bios/gba_bios.toml` generation input. Bare BIOS vector discovery is not the
production floor: use the maintained verified roots and interior resume policy.
The configuration was restored and both selections relinked. No guessed code
seed or BIOS stub was added. The old failures do not establish title defects.

## Validation and handoff

Existing PPU smoke checks cover windows/OBJ, alpha/brightness, non-aligned scroll,
authentic wide center, bitmap margins, selective affine filtering and wide
foreign-overlay clipping. Reuse these focused tests; add only a concrete missing
risk case. One valid active-window LLE/HLE pair and one basic new-image sanity
look precede the owner handoff. Reverse ordering is reserved for actual ambiguity.

The owner plays the selected games using normal pacing, adaptive view where
supported and fresh isolated save paths. Positive feedback plus material gain
supports a scoped Windows default and build-time LLE opt-out. These branches
remain draft/default LLE until that acceptance. Ports are later work.
Current adaptive qualification uses the real borderless desktop viewport, not
a forced drawable or fixed-width simulation. MKSC deliberately caps fixed view
at 240 separately from its validated adaptive maximum 480, and its display mod
owns the adaptive flag. The existing adaptive-view feature is enabled only in
the owned artifact catalog; track-60fps stays disabled. `--fullscreen=1` invokes
real SDL desktop geometry before adaptive synchronization. Earlier requested-320
setups actually remained width 240 and are retained only as rejected geometry
checks, never gain evidence. The corrected current pair remains pending.