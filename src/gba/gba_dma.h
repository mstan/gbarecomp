// gba_dma.h — STUB. Four DMA channels.
//
// Modes per GBATEK § "GBA DMA Transfers":
//   - DMA0: not allowed to source from cartridge ROM
//   - DMA1/2: sound-FIFO mode at timer 0 / 1 overflow
//   - DMA3: video capture mode at HBlank during VDraw
//   All four support immediate / VBlank / HBlank start timing.
//
// The real implementation lives on the scheduler so timing matches
// hardware (e.g., HBlank-triggered DMA stalls the CPU for N cycles,
// sound FIFO DMA fires exactly when the timer overflows).

#pragma once

namespace gba {

class GbaDma {
public:
    GbaDma();
    ~GbaDma();
};

}  // namespace gba
