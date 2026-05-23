// gba_timers.h — STUB. Four timers (TIMER0..3).
//
// Each timer has a reload value, a counter, and a control bit set
// including prescaler (1/64/256/1024), cascade-from-previous, and
// IRQ-on-overflow. Timers 0 and 1 drive the sound FIFOs via DMA. The
// scheduler must order timer overflows correctly with DMA so sound
// FIFO refills happen on the precise cycle hardware does them.
//
// Reference: GBATEK § "GBA Timers".

#pragma once

namespace gba {

class GbaTimers {
public:
    GbaTimers();
    ~GbaTimers();
};

}  // namespace gba
