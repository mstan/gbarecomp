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
## Current real-adaptive result (2026-10-07)

One corrected current-pin pair used real SDL borderless desktop 3440x1440 and
actual logical width 382x160, not a forced drawable. Both arms completed the
same active frames 10016 to 11215, 1200 presents, 321101 steps and 2116299579
cycles, final PC `0x0806134a`, with zero dispatch misses/interpreted/healed,
unmapped or unhandled-I/O counts. LLE loop time was 3.782089 s (317.285 FPS);
HLE was 3.538319 s (339.144 FPS): 6.889% more FPS and 6.445% less loop time.
This is a modest gain below the roughly 10% planning goal, not a target-met or
automatic-default claim. No repeat was made. Ordinary render/audio/present work
was retained, observer OFF, uncapped pacing only. Previously active foreign
PSX build contention is disclosed; no isolated-host inference is made.

One HLE extended race image is clear: kart, road/scenery and HUD render normally.
Normal-paced real-adaptive HLE and the LLE alternative are staged with exact
binary hashes/arguments in the private review manifest. Owner value/feel decision
is pending, and title PR 10 remains draft/default LLE as requested. No game is
launched while the owner is away. The historical native 14.09% FPS result stays
separate; the earlier wrong-width arms are setup evidence only.
## MMZ corrected active adaptive result (2026-10-07)

The maintained BIOS configuration repairs the previously rejected floor without
stubbing dispatch or adding guessed seeds. Exact title `84449cf`, framework
`a263ff2`, core `763b922` and matching freshly generated ROM/BIOS corpus cold
boot completed 12000 presents with zero static misses/interpreted/healed and
unmapped/unhandled-I/O counts. An exact existing `campaign_clear_key` extension
then progressed beyond dialogue and saved a fresh active checkpoint at 16016.
One new scene image shows Zero/buster/healthbar/Ciel in the actual industrial
level. Emerald's planned third sample is deferred by the owner's lightweight
scope; it is not a missing promotion gate.

One real adaptive borderless pair completed frames 16016 to 17215, 1200 presents,
actual 382x160 on real 3440x1440. Both report 131310 steps, cycles 62191750, final PC
`0x080c88ee`, VCOUNT 25 and identical idle-elision counts with zero misses or
interpreter/heal/unhandled-I/O activity. LLE loop was 3.158846 s (379.886 FPS),
HLE 2.971153 s (403.884 FPS): +6.317% FPS/-5.942% loop time. This is modest and below
the planning target, with no automatic repeat/default/merge. A single HLE image
is clear; the existing provider intentionally pillarboxes unsupported margins,
so this does not claim expanded authored world content. Normal SDL/audio/present
work is retained, observer OFF, uncapped pacing only, no forced drawable. Exact
normal-paced adaptive HLE/LLE review hashes and launch arguments are staged
privately; owner value/feel decision remains pending. No more GBA runs/builds.