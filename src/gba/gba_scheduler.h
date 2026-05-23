// gba_scheduler.h — STUB. Event scheduler.
//
// All time-driven hardware effects route through a single event queue:
// PPU scanline boundaries, timer overflows, DMA stalls, IRQ pending,
// audio FIFO drain. The CPU advances by N cycles, and the scheduler
// pops any events that should have fired during those N cycles. Order
// matters: a timer overflow that triggers sound DMA at cycle K must
// run before an HBlank at cycle K+1.
//
// Reference: hardware behavior (no single docs section).

#pragma once

namespace gba {

class GbaScheduler {
public:
    GbaScheduler();
    ~GbaScheduler();
};

}  // namespace gba
