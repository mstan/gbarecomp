// trace_ring.h — STUB. Always-on event ring buffer.
//
// Per RULE 0b in DEBUG.md: this ring records continuously from process
// start. Probes query for a window of interest; they NEVER arm a trace
// then run a workload. If the event you need isn't recorded, you
// extend the ring (add the new event class to the always-on capture
// path), not the probe.

#pragma once

namespace gbarecomp::debug {

class TraceRing {
public:
    TraceRing();
    ~TraceRing();
};

}  // namespace gbarecomp::debug
