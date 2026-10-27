# README showcase sources

Feature/catalog review: **21 October 2026**. The framework is beta;
per-game renderers, hardware paths and release builds keep their own limits.

- `mario-kart-super-circuit-wide.png` is copied from the game's
  [`internal-resolution-4x-adaptive.png`](https://github.com/mstan/MarioKartSuperCircuitRecomp/blob/main/docs/screenshots/internal-resolution-4x-adaptive.png).
  It depicts adaptive widescreen and 4x perspective-track sampling. Sprites
  and HUD use the original artwork. The [game README](https://github.com/mstan/MarioKartSuperCircuitRecomp)
  describes the separate optional 60 FPS presentation and two-player cable
  netplay. Credit to antimattur for the decompilation and 60 FPS patch work.
- `mega-man-zero-wide.png` is a fresh **384×160** capture from the maintained
  Mega Man Zero validation build. The original BIOS intro runs, the
  `campaign-clear` input profile advances 10,000 frames, and the shipped
  Adaptive Widescreen package is enabled. The runtime reports an effective
  384×160 view with 72-pixel margins, zero dispatch misses, zero interpreted
  instructions and zero healed code for this bounded route. This establishes
  the captured route, not every mission or the 480-pixel research mode.

The Zero capture is reproducible using the game's
[`tools/verify-strict.ps1`](https://github.com/mstan/MegaManZeroRecomp/blob/main/tools/verify-strict.ps1)
input/profile conventions, the supported ROM/BIOS and enabled view package.
The local run used a dummy audio device and isolated output; no BIOS, ROM,
save or generated source is included here. Images contain no synthetic or
retouched gameplay content.

The [multiplayer reference](MULTIPLAYER.md) and [wireless reference](WIRELESS.md)
record two-player application limits and the incomplete RFU trade/battle
qualification. Hardware port counts are not advertised as lobby sizes.
