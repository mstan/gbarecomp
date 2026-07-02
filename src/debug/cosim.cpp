// cosim.cpp — GBA first-divergence co-simulation engine + minimal TCP server.
// See COSIM_ORACLE.md. Active ONLY in the GBA_COSIM build (empty TU otherwise, so
// it is safe in the shared source list). Mirrors the psxrecomp gold standard
// (_wt-tomba2/psxrecomp/runtime/src/cosim.c), adapted to the ARM7 master clock.
//
// Model: the guest runs normally on the main thread. runtime_tick (the shared
// per-instruction master-clock advance for BOTH backends) calls cosim_on_tick() at
// each instruction boundary. When the guest cycle crosses a stride boundary it
// folds a full-state hash into a cumulative chain, stamps a ring entry, and — when
// the coordinator's step budget is exhausted — PARKS the guest thread at that
// deterministic cycle so the coordinator can read a quiescent machine over TCP,
// compare, and resume. Two instances (recomp + force-interp) driven in lockstep by
// oracle/gba_cosim.py converge on the first divergent checkpoint.
//
// Parking is at FIXED cycle-stride boundaries (never a wall-time "notice a stop
// flag" point): two processes noticing an async flag at different wall-times would
// park at different cycles — a harness nondeterminism masquerading as a guest
// divergence. Stride is fixed at launch (env) before either process runs.
#ifdef GBA_COSIM

#include "cosim.h"
#include "cosim_state.h"
#include "runtime_arm.h"          // g_cpu (ArmCpuState), g_runtime_cycles
#include "gba_bus.h"              // GbaBus / GbaIo (dev field dump)
#include "runtime_bus_bridge.h"   // gbarecomp::active_bus()

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET sock_t;
#define COSIM_SLEEP(ms) Sleep(ms)
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <pthread.h>
typedef int sock_t;
#define COSIM_SLEEP(ms) usleep((ms) * 1000)
#endif

using gbarecomp::cosim::SubHashes;

namespace {

constexpr uint64_t kFnvOff = 1469598103934665603ull;
constexpr uint64_t kFnvPrm = 1099511628211ull;

inline uint64_t fold(uint64_t h, uint64_t v) {
    for (int i = 0; i < 8; ++i) { h ^= static_cast<uint8_t>(v >> (i * 8)); h *= kFnvPrm; }
    return h;
}

constexpr uint32_t kRingN = 8192u;  // bounded reporting window (power of two)
struct Entry {
    uint64_t cp = 0;
    uint32_t pc = 0;
    uint64_t hash = 0;
    uint64_t cycle = 0;
};
Entry    g_ring[kRingN];
uint64_t g_cp      = 0;              // checkpoints crossed so far
uint64_t g_chain   = kFnvOff;        // cumulative FNV over checkpoints
uint64_t g_stride  = 65536;          // guest cycles per checkpoint (coordinator sets)
uint64_t g_next_cp = 0;              // next cycle at which to checkpoint

// Lockstep control (written by TCP thread, read by guest thread). The guest parks
// at every checkpoint boundary and advances only when granted budget via `step N`.
volatile long long g_run_budget = 0;  // checkpoints the guest may cross before parking
volatile int       g_parked     = 0;
volatile int       g_go_token    = 0; // bump to wake a parked guest to re-check budget

// Record one checkpoint of the LIVE machine at `cycle`, then honor the step budget
// (park if exhausted). The guest ALWAYS stops here (a fixed cycle boundary).
void record_checkpoint(uint64_t cycle) {
    const uint64_t h = gbarecomp::cosim::state_hash(nullptr);
    const uint64_t cp = ++g_cp;
    g_chain = fold(fold(g_chain, cycle), h);
    Entry& e = g_ring[cp & (kRingN - 1u)];
    e.cp = cp; e.pc = g_cpu.R[15]; e.hash = h; e.cycle = cycle;

    if (g_run_budget > 0) { g_run_budget = g_run_budget - 1; return; }
    g_parked = 1;
    while (g_run_budget <= 0) {
        const int tok = g_go_token;
        while (g_go_token == tok && g_run_budget <= 0) COSIM_SLEEP(1);
    }
    g_parked = 0;
    g_run_budget = g_run_budget - 1;
}

// ── minimal TCP command server ──────────────────────────────────────────────
int send_line(sock_t s, const char* buf) {
    size_t n = std::strlen(buf), off = 0;
    while (off < n) {
        int sent = send(s, buf + off, static_cast<int>(n - off), 0);
        if (sent <= 0) return 0;
        off += static_cast<size_t>(sent);
    }
    return 1;
}

void handle_line(sock_t s, char* line) {
    char out[2048];
    char cmd[32] = {0};
    if (std::sscanf(line, "%31s", cmd) != 1) { send_line(s, "err empty\n"); return; }

    if (!std::strcmp(cmd, "status")) {
        std::snprintf(out, sizeof out,
            "cp %llu cycle %llu chain %016llx stride %llu parked %d\n",
            (unsigned long long)g_cp, (unsigned long long)g_runtime_cycles,
            (unsigned long long)g_chain, (unsigned long long)g_stride, g_parked);
        send_line(s, out); return;
    }
    if (!std::strcmp(cmd, "chain")) {
        std::snprintf(out, sizeof out, "chain %016llx cp %llu cycle %llu\n",
            (unsigned long long)g_chain, (unsigned long long)g_cp,
            (unsigned long long)g_runtime_cycles);
        send_line(s, out); return;
    }
    if (!std::strcmp(cmd, "stride")) {
        // Fixes checkpoint boundaries; set ONCE before stepping. Refused mid-run.
        unsigned long long n = 0;
        if (std::sscanf(line, "%*s %llu", &n) != 1 || n == 0) { send_line(s, "err stride\n"); return; }
        if (g_cp != 0) { send_line(s, "err stride-after-start\n"); return; }
        g_stride = n; g_next_cp = 0; send_line(s, "ok\n"); return;
    }
    if (!std::strcmp(cmd, "step")) {          // step <N-checkpoints>; parks at cp+N
        unsigned long long n = 1;
        std::sscanf(line, "%*s %llu", &n);
        g_run_budget += static_cast<long long>(n);
        g_go_token = g_go_token + 1;           // wake the parked guest
        int spins = 0;
        while (g_run_budget > 0 && spins < 1200000) { COSIM_SLEEP(1); ++spins; }
        int s2 = 0; while (!g_parked && g_run_budget <= 0 && s2 < 2000) { COSIM_SLEEP(1); ++s2; }
        std::snprintf(out, sizeof out, "%s cp %llu cycle %llu chain %016llx\n",
            g_parked ? "parked" : "running",
            (unsigned long long)g_cp, (unsigned long long)g_runtime_cycles,
            (unsigned long long)g_chain);
        send_line(s, out); return;
    }
    if (!std::strcmp(cmd, "hash")) {
        Entry& e = g_ring[g_cp & (kRingN - 1u)];
        std::snprintf(out, sizeof out, "hash %016llx pc %08x cyc %llu\n",
            (unsigned long long)e.hash, e.pc, (unsigned long long)e.cycle);
        send_line(s, out); return;
    }
    if (!std::strcmp(cmd, "sub")) {
        SubHashes h; gbarecomp::cosim::state_hash(&h);
        std::snprintf(out, sizeof out,
            "cpu %016llx iwram %016llx ewram %016llx vram %016llx pal %016llx "
            "oam %016llx io %016llx audio %016llx ppu %016llx save %016llx "
            "prefetch %016llx clock %016llx\n",
            (unsigned long long)h.cpu,   (unsigned long long)h.iwram,
            (unsigned long long)h.ewram, (unsigned long long)h.vram,
            (unsigned long long)h.pal,   (unsigned long long)h.oam,
            (unsigned long long)h.io,    (unsigned long long)h.audio,
            (unsigned long long)h.ppu,   (unsigned long long)h.save,
            (unsigned long long)h.prefetch, (unsigned long long)h.clock);
        send_line(s, out); return;
    }
    if (!std::strcmp(cmd, "window")) {
        unsigned long long n = 32;
        std::sscanf(line, "%*s %llu", &n);
        if (n > kRingN) n = kRingN;
        if (n > g_cp) n = g_cp;
        for (uint64_t i = 0; i < n; ++i) {
            uint64_t cp = g_cp - (n - 1 - i);
            Entry& e = g_ring[cp & (kRingN - 1u)];
            std::snprintf(out, sizeof out, "row cp %llu pc %08x hash %016llx cyc %llu\n",
                (unsigned long long)e.cp, e.pc, (unsigned long long)e.hash,
                (unsigned long long)e.cycle);
            send_line(s, out);
        }
        send_line(s, "end\n"); return;
    }
    if (!std::strcmp(cmd, "cpu")) {
        // Full CPU field dump for field-diffing a cpu-subsystem divergence.
        char* p = out; size_t rem = sizeof out;
        int w = std::snprintf(p, rem, "cpsr %08x", g_cpu.cpsr); p += w; rem -= w;
        for (int i = 0; i < 16 && rem > 16; ++i) {
            w = std::snprintf(p, rem, " r%d %08x", i, g_cpu.R[i]); p += w; rem -= w;
        }
        for (int i = 0; i < 6 && rem > 24; ++i) {
            w = std::snprintf(p, rem, " sp%d %08x lr%d %08x spsr%d %08x",
                i, g_cpu.banked_sp[i], i, g_cpu.banked_lr[i], i, g_cpu.banked_spsr[i]);
            p += w; rem -= w;
        }
        // r8..r12 User/System bank + FIQ bank — hashed by hash_cpu() but not
        // architecturally visible in the current mode, so a cpu-subhash split
        // with all R[]/banked sp/lr/spsr matching localizes to one of these.
        for (int i = 0; i < 5 && rem > 20; ++i) {
            w = std::snprintf(p, rem, " ur%d %08x", 8 + i, g_cpu.r8_12_user[i]);
            p += w; rem -= w;
        }
        for (int i = 0; i < 5 && rem > 20; ++i) {
            w = std::snprintf(p, rem, " fr%d %08x", 8 + i, g_cpu.r8_12_fiq[i]);
            p += w; rem -= w;
        }
        std::snprintf(p, rem, "\n");
        send_line(s, out); return;
    }
    if (!std::strcmp(cmd, "dev")) {
        // Device-timing field dump (IO subsystem): field-diff which register /
        // timer / DMA-latch split when the `io` sub-hash diverges.
        gba::GbaBus* bus = gbarecomp::active_bus();
        if (!bus) { send_line(s, "err no-bus\n"); return; }
        char io_line[960];
        int n = bus->io().cosim_dump(io_line, static_cast<int>(sizeof io_line));
        if (n > 0 && io_line[n - 1] == '\n') io_line[n - 1] = '\0';  // strip trailing NL
        char big[1088];
        std::snprintf(big, sizeof big, "%s prefetch_raw %08x\n",
                      io_line, bus->bios_open_bus());
        send_line(s, big); return;
    }
    if (!std::strcmp(cmd, "inject")) {
        char what[8] = {0}; unsigned a = 0, b = 0, c = 0;
        int got = std::sscanf(line, "%*s %7s %u %u %u", what, &a, &b, &c);
        if (!std::strcmp(what, "ram") && got == 4) {
            gbarecomp::cosim::inject_ram((int)a, (uint32_t)b, (uint8_t)c);
            send_line(s, "ok\n"); return;
        }
        if (!std::strcmp(what, "reg") && got == 3) {
            gbarecomp::cosim::inject_reg((int)a, (uint32_t)b);
            send_line(s, "ok\n"); return;
        }
        send_line(s, "err inject\n"); return;
    }
    if (!std::strcmp(cmd, "reset")) { gbarecomp::cosim::state_reset(); send_line(s, "ok\n"); return; }
    send_line(s, "err unknown\n");
}

void server_loop(unsigned short port) {
    sock_t ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == (sock_t)-1) return;
    int yes = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof yes);
    sockaddr_in a; std::memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(port);
    if (bind(ls, (sockaddr*)&a, sizeof a) != 0) return;
    listen(ls, 4);
    for (;;) {
        sock_t c = accept(ls, nullptr, nullptr);
        if (c == (sock_t)-1) { COSIM_SLEEP(5); continue; }
        char buf[512]; int used = 0;
        for (;;) {
            int r = recv(c, buf + used, (int)sizeof(buf) - 1 - used, 0);
            if (r <= 0) break;
            used += r; buf[used] = 0;
            char* nl;
            while ((nl = std::strchr(buf, '\n')) != nullptr) {
                *nl = 0;
                char* cr = std::strchr(buf, '\r'); if (cr) *cr = 0;
                handle_line(c, buf);
                size_t rest = std::strlen(nl + 1);
                std::memmove(buf, nl + 1, rest + 1);
                used = (int)rest;
            }
            if (used >= (int)sizeof(buf) - 1) used = 0;  // overlong line: drop
        }
#ifdef _WIN32
        closesocket(c);
#else
        close(c);
#endif
    }
}

#ifdef _WIN32
DWORD WINAPI server_thread(LPVOID p) { server_loop((unsigned short)(uintptr_t)p); return 0; }
#else
void* server_thread(void* p) { server_loop((unsigned short)(uintptr_t)p); return nullptr; }
#endif

}  // namespace

// Per-instruction checkpoint hook (from runtime_tick). Record a checkpoint for
// each stride boundary the master clock has crossed since the last call (the clock
// can jump by >stride on a DMA-steal / HALT-wake window), stamping each with its
// exact stride cycle. Both backends cross identical boundaries with identical
// per-instruction cycle charges, so their checkpoints align until the true
// divergence.
void cosim_on_tick(void) {
    const uint64_t now = g_runtime_cycles;
    const uint64_t stride = g_stride ? g_stride : 1;
    while (now >= g_next_cp) {
        record_checkpoint(g_next_cp);
        g_next_cp += stride;
    }
}

void cosim_init(void) {
    gbarecomp::cosim::state_reset();
    unsigned short port = 19850;
    if (const char* e = std::getenv("GBA_COSIM_PORT"); e && *e) port = (unsigned short)std::atoi(e);
    // Stride fixed at launch (env) so both processes checkpoint at identical cycles
    // before either runs an instruction — no set-stride race.
    if (const char* st = std::getenv("GBA_COSIM_STRIDE"); st && *st) {
        if (unsigned long long v = std::strtoull(st, nullptr, 10); v) g_stride = v;
    }
    if (const char* sc = std::getenv("GBA_COSIM_START_CYCLE"); sc && *sc) {
        g_next_cp = std::strtoull(sc, nullptr, 10);
    }
#ifdef _WIN32
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    CreateThread(nullptr, 0, server_thread, (LPVOID)(uintptr_t)port, 0, nullptr);
#else
    pthread_t t; pthread_create(&t, nullptr, server_thread, (void*)(uintptr_t)port);
#endif
    std::fprintf(stdout, "cosim: first-divergence oracle listening on 127.0.0.1:%u "
                         "(stride %llu)\n", port, (unsigned long long)g_stride);
}

#endif  // GBA_COSIM
