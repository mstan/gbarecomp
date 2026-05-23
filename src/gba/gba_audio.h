// gba_audio.h — STUB. Sound channels + direct-sound FIFOs.
//
// The GBA has the four DMG-era sound channels (1: square w/sweep, 2:
// square, 3: 4-bit wave, 4: noise) plus two 8-bit PCM "direct sound"
// FIFOs (A/B) which are driven by timer-triggered DMA. Even if audio
// *output* is shipped silent initially, the *bookkeeping* (FIFO fill
// levels, timer overflows that trigger refill DMA) must be correct
// because games drive game-logic timing off audio DMA. See PRINCIPLES.
//
// Reference: GBATEK § "GBA Sound Controller", § "GBA Direct Sound".

#pragma once

namespace gba {

class GbaAudio {
public:
    GbaAudio();
    ~GbaAudio();
};

}  // namespace gba
