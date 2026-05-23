// snapshot.h — STUB. Save-state and per-frame snapshot writers.
//
// Per PRINCIPLES.md "Control Flow Semantics": save/load must restore
// guest CPU + memory + devices + a well-defined generated-code resume
// boundary. NEVER serialize host C-stack state.

#pragma once

namespace gbarecomp::debug {

class Snapshot {
public:
    Snapshot();
    ~Snapshot();
};

}  // namespace gbarecomp::debug
