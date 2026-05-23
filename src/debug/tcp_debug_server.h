// tcp_debug_server.h — native debug TCP server.
//
// Mirrors the mGBA oracle's command shape (oracle/main.cpp) but
// against the NATIVE runtime's CPU/bus/PPU state. The diff harness
// can speak the same line-delimited JSON to both sides on different
// ports (default 19842 native, 19843 oracle).
//
// Operates command-driven: the server's run() blocks on a single
// client at a time, dispatching commands until the client sends
// "quit". Frame stepping is explicit — `step` runs one PPU frame,
// other commands inspect current state. This is the shape diff_frame
// needs to sync native and oracle on the same PPU-frame boundary.
//
// Lifetime: pointers passed in `Context` must outlive the run() call.

#pragma once

#include <cstdint>
#include <functional>

namespace armv4t {
struct CPUState;
class  Bus;
}

namespace gba {
class GbaBus;
class GbaPpu;
}

namespace gbarecomp::debug {

class TcpDebugServer {
public:
    // Callback the server invokes when the client sends `step`. The
    // host runs one PPU frame and updates its own counters; returns
    // false if the run terminated abnormally (e.g. NotImplemented).
    using StepFn = std::function<bool()>;

    struct Context {
        armv4t::CPUState* cpu = nullptr;
        gba::GbaBus*      bus = nullptr;   // owns the regions
        gba::GbaPpu*      ppu = nullptr;
        StepFn            step;       // advances one PPU frame
        StepFn            step_inst;  // advances one CPU instruction
        // Mirror of the host's counters so `counters` queries can
        // report them. Host updates these between commands.
        uint64_t* irq_entries        = nullptr;
        uint64_t* swi_entries        = nullptr;
        uint64_t* halt_steps         = nullptr;
        uint64_t* vblank_irqs_raised = nullptr;
        uint64_t* steps              = nullptr;
    };

    TcpDebugServer();
    ~TcpDebugServer();

    // Bind 127.0.0.1:port, accept one client, dispatch commands until
    // the client closes or sends `quit`. Blocking. Returns once done.
    // Returns true on clean exit, false if listen/bind/accept failed.
    bool run(int port, const Context& ctx);
};

}  // namespace gbarecomp::debug
