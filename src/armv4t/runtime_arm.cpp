// runtime_arm.cpp — implementation of the C-ABI surface that the
// recompiled cart code calls into.
//
// The interpreter encodes the canonical ARM/THUMB semantics; this
// file is the parallel surface for recompiled code. Helpers here
// must produce the SAME bit-level result as the interpreter would
// for the same inputs — verify any new helper against the
// interpreter's case in src/armv4t/interpreter.cpp.

#include "runtime_arm.h"
#include "symbol_lookup.h"

#include <cstdio>
#include <cstdlib>

// runtime_dispatch_miss / runtime_unimplemented_op default-abort
// implementations live in src/runtime/runtime_arm_default_aborts.cpp
// (part of gbarecomp_runtime), NOT here. The split exists because
// MinGW PE-COFF weak symbols don't reliably resolve from static
// archives, so we can't use weak overrides for those. Splitting
// puts the abort versions in the runtime library that production
// builds link, while tests/codegen/ links only gbarecomp_armv4t
// and supplies its own non-aborting stubs.
//
// runtime_swi STAYS here — its production behavior is exactly the
// exception-entry shape every consumer needs; tests verify that
// shape rather than override it.

// Forward decls of the per-game dispatch table. Each game's
// generated/dispatch_table.cpp defines these.
struct DispatchEntry {
    uint32_t addr;
    uint8_t thumb;
    void (*fn)(void);
};
extern "C" const DispatchEntry kDispatchTable[];
extern "C" const unsigned kDispatchTableLen;

// BIOS dispatch table. Lives in src/runtime/generated_bios/
// bios_dispatch_table.cpp — placeholder until `gba_recompile --bios`
// is run. runtime_dispatch consults this FIRST for PC < 0x4000.
extern "C" const DispatchEntry kBiosDispatchTable[];
extern "C" const unsigned kBiosDispatchTableLen;

// Stage-2 self-heal third dispatch tier. Defined in the runtime lib
// (src/runtime/overlay_loader.cpp); test binaries that link only
// gbarecomp_armv4t supply a null-returning stub (tests/codegen/stubs.cpp).
// runtime_dispatch consults it after the static tables miss; returns 1 iff a
// healed native overlay ran. armv4t must not depend on the runtime lib, so the
// link is a plain extern "C" symbol, mirroring runtime_dispatch_miss.
extern "C" int overlay_try_dispatch(uint32_t pc, int thumb);

// ── CPU state ──────────────────────────────────────────────────────

extern "C" ArmCpuState g_cpu = {};
extern "C" RuntimeThumbAluImmediateOverride
    g_runtime_thumb_alu_imm_override = nullptr;
extern "C" RuntimeRamDispatchHook g_runtime_ram_dispatch_hook = nullptr;

// VBlank-start counter (defined in src/runtime/runtime_bus_bridge.cpp).
// Used to frame-gate the mem-write watchpoint.
extern "C" unsigned long long g_runtime_vblank_starts;

namespace {

constexpr uint32_t kTraceSize = 4096u;
RuntimeTraceEntry g_trace[kTraceSize] = {};
uint32_t g_trace_write = 0;
uint32_t g_trace_count = 0;
uint32_t g_trace_seq = 0;

constexpr uint32_t kCallReturnStackSize = 1024u;
uint32_t g_call_return_stack[kCallReturnStackSize] = {};
uint32_t g_call_return_depth = 0;
// Barrier into the GLOBAL guest call-return stack. An IRQ is dispatched on top
// of whatever mainline (or enclosing-IRQ) call frames are live, but the handler's
// guest returns/cancels must NOT match or pop those interrupted frames — doing so
// makes a handler return-cancel unwind into the mainline, skipping the rest of
// the handler (e.g. LeafGreen's VBlankIntr tail `gMain.intrCheck |= VBLANK` is
// never reached, so WaitForVBlank spins forever). runtime_irq() raises this floor
// to the live depth for the duration of the handler; should_return/cancel_return
// never look below it. Saved/restored across nested IRQs by runtime_irq().
uint32_t g_call_return_floor = 0;

const char* trace_kind_name(uint32_t kind) {
    switch (kind) {
        case RUNTIME_TRACE_DISPATCH:  return "dispatch";
        case RUNTIME_TRACE_EXCHANGE:  return "exchange";
        case RUNTIME_TRACE_SWI:       return "swi";
        case RUNTIME_TRACE_MEM_WRITE: return "mem_w";
        case RUNTIME_TRACE_BRANCH:    return "branch";
        case RUNTIME_TRACE_IRQ:       return "irq";
        case RUNTIME_TRACE_CALL:      return "call";
        case RUNTIME_TRACE_MEM_READ:  return "mem_r";
        default:                      return "unknown";
    }
}

}  // namespace

extern "C" void runtime_trace_event(uint32_t kind, uint32_t pc,
                                     uint32_t addr, uint32_t value,
                                     uint32_t aux) {
    RuntimeTraceEntry& e = g_trace[g_trace_write];
    e.seq = ++g_trace_seq;
    e.cycles = g_runtime_cycles;
    e.kind = kind;
    e.pc = pc;
    e.cpsr = g_cpu.cpsr;
    e.addr = addr;
    e.value = value;
    e.aux = aux;
    e.r0 = g_cpu.R[0];
    e.r1 = g_cpu.R[1];
    e.r2 = g_cpu.R[2];
    e.r3 = g_cpu.R[3];
    e.r4 = g_cpu.R[4];
    e.r5 = g_cpu.R[5];
    e.r12 = g_cpu.R[12];
    e.r13 = g_cpu.R[13];
    e.r14 = g_cpu.R[14];

    g_trace_write = (g_trace_write + 1u) % kTraceSize;
    if (g_trace_count < kTraceSize) ++g_trace_count;

    static int abort_on_bios_write = -1;
    if (abort_on_bios_write < 0) {
        abort_on_bios_write =
            std::getenv("GBARECOMP_ABORT_ON_BIOS_WRITE") ? 1 : 0;
    }
    if (abort_on_bios_write &&
        kind == RUNTIME_TRACE_MEM_WRITE &&
        addr < 0x00004000u) {
        std::fprintf(stderr,
                     "runtime_trace: generated BIOS-region write "
                     "pc=0x%08X addr=0x%08X value=0x%08X width=%u\n",
                     pc, addr, value, aux);
        runtime_trace_dump_recent(96);
        std::abort();
    }

    static uint32_t abort_mem_addr = 0xFFFFFFFFu;
    if (abort_mem_addr == 0xFFFFFFFFu) {
        const char* env = std::getenv("GBARECOMP_ABORT_ON_MEM_WRITE_ADDR");
        abort_mem_addr = env ? static_cast<uint32_t>(std::strtoul(env, nullptr, 0))
                             : 0xFFFFFFFEu;
    }
    // How many trailing trace events the abort handlers dump. Default 160;
    // raise (up to the ring size) to capture the full call chain back to a
    // main loop. GBARECOMP_TRACE_DUMP_DEPTH=4000.
    static uint32_t abort_dump_depth = 0u;
    if (abort_dump_depth == 0u) {
        const char* env = std::getenv("GBARECOMP_TRACE_DUMP_DEPTH");
        abort_dump_depth = env ? static_cast<uint32_t>(std::strtoul(env, nullptr, 0))
                               : 160u;
        if (abort_dump_depth == 0u) abort_dump_depth = 160u;
    }
    // Optional frame gate: suppress the mem-write abort until this many
    // VBlank-starts have elapsed. Lets a watchpoint skip the identical
    // early frames and fire on the write in the frame where the value
    // actually diverges (e.g. MC-HP-002's frame-40 onset). 0 = no gate.
    static uint64_t abort_min_vblank = 0xFFFFFFFFFFFFFFFFull;
    if (abort_min_vblank == 0xFFFFFFFFFFFFFFFFull) {
        const char* env = std::getenv("GBARECOMP_ABORT_ON_MEM_WRITE_MIN_FRAME");
        abort_min_vblank = env ? std::strtoull(env, nullptr, 0) : 0ull;
    }
    // Optional value gate: only abort when the written value matches. Lets a
    // watchpoint skip a correct write to an address and fire on the specific
    // erroneous value (e.g. MC-HP-002: a per-frame countdown is written 0x0C
    // first — correct — then 0x0B by the extra tick; watch value=0x0B to land
    // on the duplicate write, not the legitimate one). -1 = match any value.
    static long long abort_mem_value = -2;
    if (abort_mem_value == -2) {
        const char* env = std::getenv("GBARECOMP_ABORT_ON_MEM_WRITE_VALUE");
        abort_mem_value = env ? static_cast<long long>(std::strtoull(env, nullptr, 0))
                              : -1;
    }
    if (kind == RUNTIME_TRACE_MEM_WRITE && addr == abort_mem_addr &&
        g_runtime_vblank_starts >= abort_min_vblank &&
        (abort_mem_value < 0 || value == static_cast<uint32_t>(abort_mem_value))) {
        std::fprintf(stderr,
                     "runtime_trace: mem-write-addr abort pc=0x%08X "
                     "addr=0x%08X value=0x%08X width=%u (vblanks=%llu)\n",
                     pc, addr, value, aux,
                     static_cast<unsigned long long>(g_runtime_vblank_starts));
        runtime_trace_dump_recent(abort_dump_depth);
        std::abort();
    }

    static int abort_on_high_mem_read = -1;
    if (abort_on_high_mem_read < 0) {
        abort_on_high_mem_read =
            std::getenv("GBARECOMP_ABORT_ON_MEM_READ_HIGH") ? 1 : 0;
    }
    if (abort_on_high_mem_read &&
        kind == RUNTIME_TRACE_MEM_READ &&
        (addr >> 24) >= 0x0Eu) {
        std::fprintf(stderr,
                     "runtime_trace: high mem-read abort pc=0x%08X "
                     "addr=0x%08X value=0x%08X width=%u\n",
                     pc, addr, value, aux);
        runtime_trace_dump_recent(160);
        std::abort();
    }

    static int branch_abort_after = -2;
    if (branch_abort_after == -2) {
        const char* env = std::getenv("GBARECOMP_ABORT_AFTER_BRANCHES");
        branch_abort_after = env ? std::atoi(env) : -1;
    }
    if (kind == RUNTIME_TRACE_BRANCH && branch_abort_after >= 0) {
        if (branch_abort_after == 0) {
            std::fprintf(stderr,
                         "runtime_trace: branch abort at pc=0x%08X "
                         "target=0x%08X\n",
                         pc, addr);
            runtime_trace_dump_recent(96);
            std::abort();
        }
        --branch_abort_after;
    }

    static uint32_t abort_branch_pc = 0xFFFFFFFFu;
    if (abort_branch_pc == 0xFFFFFFFFu) {
        const char* env = std::getenv("GBARECOMP_ABORT_ON_BRANCH_PC");
        abort_branch_pc = env ? static_cast<uint32_t>(std::strtoul(env, nullptr, 0))
                              : 0xFFFFFFFEu;
    }
    if (kind == RUNTIME_TRACE_BRANCH && pc == abort_branch_pc) {
        std::fprintf(stderr,
                     "runtime_trace: branch-pc abort at pc=0x%08X "
                     "target=0x%08X\n",
                     pc, addr);
        runtime_trace_dump_recent(160);
        std::abort();
    }
}

extern "C" void runtime_trace_reset(void) {
    g_trace_write = 0;
    g_trace_count = 0;
    g_trace_seq = 0;
    g_runtime_cycles = 0;  // cycle clock shares the machine-reset lifecycle
    // Arm per-instruction fingerprinting for the whole run if requested, so the
    // ring is always-on from reset (no arm-then-run latency gap) and we query it
    // after the fact. Also settable via the TCP `insn_trace` command.
    const char* it = std::getenv("GBARECOMP_INSN_TRACE");
    g_runtime_insn_trace = (it && it[0] && it[0] != '0') ? 1u : 0u;
    runtime_fp_reset();
}

extern "C" void runtime_trace_dump_recent(uint32_t max_entries) {
    if (max_entries > g_trace_count) max_entries = g_trace_count;
    std::fprintf(stderr, "runtime_trace: last %u event(s)\n", max_entries);
    uint32_t start = (g_trace_write + kTraceSize - max_entries) % kTraceSize;
    for (uint32_t i = 0; i < max_entries; ++i) {
        const RuntimeTraceEntry& e = g_trace[(start + i) % kTraceSize];
        // Annotate the PC with the nearest recompiled function name, e.g.
        // <UpdateAnimationVariableFrames+0x10>, when a symbol map is linked.
        char symbuf[96];
        symbuf[0] = '\0';
        uint32_t off = 0;
        const char* sym = gba_symbol_lookup(e.pc, &off);
        if (sym) {
            std::snprintf(symbuf, sizeof(symbuf), " <%s+0x%X>", sym, off);
        }
        std::fprintf(stderr,
                     "  #%u %-8s pc=0x%08X%s cpsr=0x%08X "
                     "addr=0x%08X value=0x%08X aux=0x%X "
                     "r0=0x%08X r1=0x%08X r2=0x%08X r3=0x%08X "
                     "r4=0x%08X r5=0x%08X r12=0x%08X "
                     "sp=0x%08X lr=0x%08X\n",
                     e.seq, trace_kind_name(e.kind), e.pc, symbuf, e.cpsr,
                     e.addr, e.value, e.aux, e.r0, e.r1, e.r2, e.r3,
                     e.r4, e.r5, e.r12, e.r13, e.r14);
    }
}

extern "C" uint32_t runtime_trace_copy_recent(RuntimeTraceEntry* out,
                                               uint32_t max_entries) {
    if (!out || max_entries == 0) return 0;
    if (max_entries > g_trace_count) max_entries = g_trace_count;
    uint32_t start = (g_trace_write + kTraceSize - max_entries) % kTraceSize;
    for (uint32_t i = 0; i < max_entries; ++i) {
        out[i] = g_trace[(start + i) % kTraceSize];
    }
    return max_entries;
}

// ── Per-instruction fingerprint ring ───────────────────────────────
// A large bounded ring of pre-execution architectural fingerprints, one per
// guest instruction, captured when armed. Lives here (not the runtime bridge)
// so the codegen test harness — which links the armv4t lib but not the runtime
// — gets the symbols without a stub. See runtime_arm.h for the contract.

extern "C" unsigned g_runtime_insn_trace = 0;

// General function-entry hook (see runtime_arm.h). nullptr = disabled; a host
// debug probe assigns its own handler. Called from the generated function
// prologue with the guest entry PC while R0..R3 still hold the AAPCS args.
extern "C" void (*g_runtime_fn_entry_hook)(uint32_t) = nullptr;
extern "C" uint32_t g_runtime_resume_pc = 0u;

// BIOS-HLE hook (see runtime_arm.h). nullptr = disabled = pure LLE (the
// recompiled BIOS handles every SWI; byte-identical to the un-hooked build).
// When installed (gba::bios_hle_set_mode), runtime_swi consults it BEFORE the
// SVC-mode exception entry; a return of 1 means the SWI was serviced in HLE and
// the guest resumes at LR with no BIOS dispatch. 0 falls through to LLE.
extern "C" int (*g_bios_hle_hook)(uint32_t swi_num) = nullptr;

namespace {
// ~8M instructions of history (~60+ PPU-frames). The recomp runs several frames
// ahead of the interp oracle in cumulative cycle (boot step_frame overshoot), so
// a 1M (~8-frame) ring barely overlapped the interp's at the same frame number;
// the MC-HP-002 divergence onset (~f3644, deep in the run) needs a wider window
// so one dump spans both the pre-divergence anchor and the first divergent insn.
// 80 bytes/entry → ~640 MB, only touched when armed (GBARECOMP_INSN_TRACE=1).
constexpr uint32_t kFpSize = 1u << 23;
RuntimeFpEntry* g_fp = nullptr;       // lazily allocated on first arm
uint32_t        g_fp_write = 0;
uint32_t        g_fp_count = 0;
}  // namespace

extern "C" void runtime_insn_fp(void) {
    if (!g_fp) {
        g_fp = static_cast<RuntimeFpEntry*>(
            std::calloc(kFpSize, sizeof(RuntimeFpEntry)));
        if (!g_fp) { g_runtime_insn_trace = 0; return; }  // OOM → disarm quietly
    }
    RuntimeFpEntry& e = g_fp[g_fp_write];
    e.cycles = g_runtime_cycles;
    e.pc = g_cpu.R[15];
    e.cpsr = g_cpu.cpsr;
    for (int i = 0; i < 16; ++i) e.r[i] = g_cpu.R[i];
    g_fp_write = (g_fp_write + 1u) % kFpSize;
    if (g_fp_count < kFpSize) ++g_fp_count;
}

extern "C" void runtime_fp_reset(void) {
    g_fp_write = 0;
    g_fp_count = 0;
}

extern "C" uint32_t runtime_fp_count(void) { return g_fp_count; }

extern "C" uint32_t runtime_fp_save_file(const char* path) {
    if (!path || !g_fp || g_fp_count == 0) return 0;
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return 0;
    uint32_t magic = 0x31504647u;  // 'GFP1'
    uint32_t esz = static_cast<uint32_t>(sizeof(RuntimeFpEntry));
    unsigned long long count = g_fp_count;
    std::fwrite(&magic, sizeof(magic), 1, f);
    std::fwrite(&esz, sizeof(esz), 1, f);
    std::fwrite(&count, sizeof(count), 1, f);
    uint32_t start = (g_fp_write + kFpSize - g_fp_count) % kFpSize;
    for (uint32_t i = 0; i < g_fp_count; ++i) {
        std::fwrite(&g_fp[(start + i) % kFpSize], sizeof(RuntimeFpEntry), 1, f);
    }
    std::fclose(f);
    return g_fp_count;
}

// Dump the LAST `n` fingerprint records (oldest-first within the window) as a
// human-readable CSV. This is the freeze's self-documenting execution history:
// the always-on hang watchdog calls it the moment it trips, so the PCs the
// game thread executed leading INTO the freeze are on disk without any live
// interaction (PRINCIPLES.md "always-on ring first" — query the ring for the
// window of interest, never arm-then-capture). Returns records written.
// Requires GBARECOMP_INSN_TRACE armed (else the ring is empty → 0). Caller is
// the game thread at the watchdog trip, so this is single-threaded w.r.t. the
// ring writes.
extern "C" uint32_t runtime_fp_save_tail_csv(const char* path, uint32_t n) {
    if (!path || !g_fp || g_fp_count == 0) return 0;
    if (n == 0 || n > g_fp_count) n = g_fp_count;
    std::FILE* f = std::fopen(path, "w");
    if (!f) return 0;
    std::fprintf(f, "idx,cycles,pc,cpsr,r0,r1,r2,r3,r4,r5,r6,r7,r8,r9,r10,r11,"
                    "r12,sp,lr,r15\n");
    // The most recent record is at (g_fp_write-1); walk back n, then forward.
    uint32_t start = (g_fp_write + kFpSize - n) % kFpSize;
    for (uint32_t i = 0; i < n; ++i) {
        const RuntimeFpEntry& e = g_fp[(start + i) % kFpSize];
        std::fprintf(f, "%u,%llu,0x%08X,0x%08X", i,
                     static_cast<unsigned long long>(e.cycles), e.pc, e.cpsr);
        for (int k = 0; k < 16; ++k) std::fprintf(f, ",0x%08X", e.r[k]);
        std::fputc('\n', f);
    }
    std::fclose(f);
    return n;
}

// Filter the insn-fingerprint ring by guest PC: collect up to `max_hits`
// cumulative-cycle stamps (oldest-first) for every recorded execution of `pc`.
// The Δ between consecutive returned stamps is the offset-cancelled cycle ruler
// (Axis 2) peered against the NBA oracle's cyc_anchor. Non-mutating; requires
// the ring armed (GBARECOMP_INSN_TRACE) — empty otherwise.
extern "C" uint32_t runtime_fp_query_pc(uint32_t pc, uint32_t max_hits,
                                        unsigned long long* out_cycles) {
    if (!g_fp || g_fp_count == 0 || !out_cycles || max_hits == 0) return 0;
    uint32_t start = (g_fp_write + kFpSize - g_fp_count) % kFpSize;
    uint32_t found = 0;
    for (uint32_t i = 0; i < g_fp_count && found < max_hits; ++i) {
        const RuntimeFpEntry& e = g_fp[(start + i) % kFpSize];
        if (e.pc == pc) out_cycles[found++] = e.cycles;
    }
    return found;
}

// Read-only current recompiled-CPU PC, for always-on observability taps that
// live outside the armv4t lib (the gba_io MMIO write-trace ring).
extern "C" uint32_t runtime_current_pc(void) { return g_cpu.R[15]; }

// ── IRQ-vector log (MC-HP-002) ─────────────────────────────────────
// A dedicated, always-on, cycle-stamped log of EVERY IRQ vectoring. The
// general trace ring (4096) is diluted by mem-writes (thousands/frame) so it
// spans only a fraction of a frame; this log records one entry per IRQ vector
// (~1-2/frame) so it spans ~10^4 frames — enough to find, on a fresh boot, the
// frame where the recomp vectors an EXTRA VBlank IRQ vs the interp oracle (the
// one-extra-M4A-tick that drifts the sequencer into the f3656 spin). The
// `from_halt` flag distinguishes the wake-from-HALT delayed path from the
// running immediate path. Dumped as CSV on exit when GBARECOMP_IRQ_LOG is set.
namespace {
constexpr uint32_t kIrqLogSize = 1u << 17;   // 131072 vectors (~32k+ frames)
using IrqLogEntry = RuntimeIrqLogEntry;       // public type (runtime_arm.h)
IrqLogEntry* g_irq_log = nullptr;
uint32_t     g_irq_log_write = 0;
uint32_t     g_irq_log_count = 0;
}  // namespace
// Set by runtime_tick's wake-from-HALT path just before runtime_irq; cleared
// after. Tells the log whether this vector woke the CPU from HALT.
extern "C" uint32_t g_runtime_irq_from_halt = 0;

extern "C" void runtime_irq_log_record(uint32_t src, uint32_t ret, uint32_t cpsr) {
    // Always-on ring (Release too): recording is unconditional so the live
    // irq_cap TCP query always has the window of interest without arming. The
    // env GBARECOMP_IRQ_LOG only governs the separate on-exit CSV dump. Purely
    // additive bookkeeping — does not alter IRQ behavior.
    if (!g_irq_log) {
        g_irq_log = static_cast<IrqLogEntry*>(
            std::calloc(kIrqLogSize, sizeof(IrqLogEntry)));
        if (!g_irq_log) return;
    }
    IrqLogEntry& e = g_irq_log[g_irq_log_write];
    e.cycles = g_runtime_cycles;
    e.src = src;
    e.ret = ret;
    e.cpsr = cpsr;
    e.from_halt = g_runtime_irq_from_halt;
    g_irq_log_write = (g_irq_log_write + 1u) % kIrqLogSize;
    if (g_irq_log_count < kIrqLogSize) ++g_irq_log_count;
}

extern "C" uint32_t runtime_irq_log_save_file(const char* path) {
    if (!path || !g_irq_log || g_irq_log_count == 0) return 0;
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return 0;
    std::fprintf(f, "seq,cycles,src,ret,cpsr,from_halt\n");
    uint32_t start = (g_irq_log_write + kIrqLogSize - g_irq_log_count) % kIrqLogSize;
    for (uint32_t i = 0; i < g_irq_log_count; ++i) {
        const IrqLogEntry& e = g_irq_log[(start + i) % kIrqLogSize];
        std::fprintf(f, "%u,%llu,0x%04x,0x%08x,0x%08x,%u\n",
                     i, e.cycles, e.src, e.ret, e.cpsr, e.from_halt);
    }
    std::fclose(f);
    return g_irq_log_count;
}

extern "C" uint32_t runtime_irq_log_count(void) { return g_irq_log_count; }

// Copy the most recent `max` IRQ-vector entries (oldest-first within the
// window) for the live irq_cap TCP query. Non-mutating.
extern "C" uint32_t runtime_irq_log_copy_recent(RuntimeIrqLogEntry* out,
                                                uint32_t max) {
    if (!out || max == 0 || !g_irq_log || g_irq_log_count == 0) return 0;
    if (max > g_irq_log_count) max = g_irq_log_count;
    uint32_t start = (g_irq_log_write + kIrqLogSize - max) % kIrqLogSize;
    for (uint32_t i = 0; i < max; ++i) {
        out[i] = g_irq_log[(start + i) % kIrqLogSize];
    }
    return max;
}

// ── SWI log (MC-HP-002 milestone-PC sequence) ──────────────────────
// Always-on, cycle-stamped log of every SWI (BIOS call). VBlankIntrWait is
// SWI 5; it runs once per main-loop iteration, so its sequence IS the main-loop
// cadence. Per recomp-template "use the oracle as an order + state + caller
// reference": we capture cycle + caller(ret) + r0/r1 args + the BIOS IntrWait
// flags at 0x03007FF8, so a recomp-vs-interp SWI-5 sequence diff shows an EXTRA
// main-loop iteration (the recomp's 1-frame-ahead slip ~f272) and the IntrWait
// flag state that produced it. Armed by env GBARECOMP_SWI_LOG; CSV on exit.
namespace {
constexpr uint32_t kSwiLogSize = 1u << 17;
struct SwiLogEntry { unsigned long long cycles; uint32_t imm; uint32_t ret;
                     uint32_t r0; uint32_t r1; uint32_t r2; uint32_t lr;
                     uint32_t iwflags; };
SwiLogEntry* g_swi_log = nullptr;
uint32_t     g_swi_log_write = 0;
uint32_t     g_swi_log_count = 0;
}  // namespace

extern "C" void runtime_swi_log_record(uint32_t imm, uint32_t ret, uint32_t r0,
                                       uint32_t r1, uint32_t r2, uint32_t lr,
                                       uint32_t iwflags) {
    static int armed = -1;
    if (armed < 0) armed = std::getenv("GBARECOMP_SWI_LOG") ? 1 : 0;
    if (!armed) return;
    if (!g_swi_log) {
        g_swi_log = static_cast<SwiLogEntry*>(
            std::calloc(kSwiLogSize, sizeof(SwiLogEntry)));
        if (!g_swi_log) { armed = 0; return; }
    }
    SwiLogEntry& e = g_swi_log[g_swi_log_write];
    e.cycles = g_runtime_cycles;
    e.imm = imm; e.ret = ret; e.r0 = r0; e.r1 = r1; e.r2 = r2; e.lr = lr;
    e.iwflags = iwflags;
    g_swi_log_write = (g_swi_log_write + 1u) % kSwiLogSize;
    if (g_swi_log_count < kSwiLogSize) ++g_swi_log_count;
}

extern "C" uint32_t runtime_swi_log_save_file(const char* path) {
    if (!path || !g_swi_log || g_swi_log_count == 0) return 0;
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return 0;
    std::fprintf(f, "seq,cycles,imm,ret,r0,r1,r2,lr,iwflags\n");
    uint32_t start = (g_swi_log_write + kSwiLogSize - g_swi_log_count) % kSwiLogSize;
    for (uint32_t i = 0; i < g_swi_log_count; ++i) {
        const SwiLogEntry& e = g_swi_log[(start + i) % kSwiLogSize];
        std::fprintf(f, "%u,%llu,%u,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x\n",
                     i, e.cycles, e.imm, e.ret, e.r0, e.r1, e.r2, e.lr, e.iwflags);
    }
    std::fclose(f);
    return g_swi_log_count;
}

// ── Bus binding ────────────────────────────────────────────────────

namespace gbarecomp {
namespace runtime_arm {

// We keep this as a void* so the header doesn't need to drag in
// GbaBus. The bus type is known to the implementation file only.
void* g_bus_handle = nullptr;

}  // namespace runtime_arm
}  // namespace gbarecomp

// ── Condition codes ────────────────────────────────────────────────

extern "C" int arm_cond_passes(unsigned cond) {
    // The condition codes are 4 bits. AL / NV are the unconditional
    // bands.
    const uint32_t n = cpsr_n();
    const uint32_t z = cpsr_z();
    const uint32_t c = cpsr_c();
    const uint32_t v = cpsr_v();
    switch (cond & 0xFu) {
        case 0x0: return z != 0;                            // EQ
        case 0x1: return z == 0;                            // NE
        case 0x2: return c != 0;                            // CS/HS
        case 0x3: return c == 0;                            // CC/LO
        case 0x4: return n != 0;                            // MI
        case 0x5: return n == 0;                            // PL
        case 0x6: return v != 0;                            // VS
        case 0x7: return v == 0;                            // VC
        case 0x8: return (c != 0) && (z == 0);              // HI
        case 0x9: return (c == 0) || (z != 0);              // LS
        case 0xA: return n == v;                            // GE
        case 0xB: return n != v;                            // LT
        case 0xC: return (z == 0) && (n == v);              // GT
        case 0xD: return (z != 0) || (n != v);              // LE
        case 0xE: return 1;                                 // AL
        case 0xF: return 0;                                 // NV (ARMv4T: never)
        default:  return 0;
    }
}

// ── Bus accessors ──────────────────────────────────────────────────
//
// The runtime initializer (runtime_init) installs `g_bus_handle` to
// a pointer to the active gba::GbaBus. Calling the bus is the only
// per-instruction host-side work the generated code has to do, so
// these need to be fast.
//
// For first cut we delegate via the bus_handle as a fat pointer. A
// later optimization can compile bus reads inline using the
// runtime's memory map directly.

namespace {

inline uint32_t* bus_ptr32(void* /*h*/, uint32_t /*addr*/) {
    return nullptr;  // not used; placeholder for future inlining
}

}  // namespace

// Bus accessors are NOT defined here — they live in
// src/runtime/runtime_bus_bridge.cpp (part of gbarecomp_runtime),
// which is the only translation unit allowed to include gba_bus.h.
// Anything that links against gbarecomp_runtime gets them; tests
// that link only gbarecomp_armv4t can supply their own stubs.

// ── Shifter helpers ────────────────────────────────────────────────

extern "C" uint32_t arm_shift_lsl(uint32_t v, uint32_t n, int set_carry) {
    if (n == 0) return v;  // ARM ARM A5.1.5: shift by 0 → no carry update
    if (n >= 32) {
        if (set_carry) {
            uint32_t carry = (n == 32) ? (v & 1u) : 0u;
            g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) | (carry ? CPSR_C_BIT : 0u);
        }
        return 0;
    }
    if (set_carry) {
        uint32_t carry = (v >> (32u - n)) & 1u;
        g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) | (carry ? CPSR_C_BIT : 0u);
    }
    return v << n;
}

extern "C" uint32_t arm_shift_lsr(uint32_t v, uint32_t n, int set_carry) {
    if (n == 0) return v;
    if (n >= 32) {
        if (set_carry) {
            uint32_t carry = (n == 32) ? ((v >> 31) & 1u) : 0u;
            g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) | (carry ? CPSR_C_BIT : 0u);
        }
        return 0;
    }
    if (set_carry) {
        uint32_t carry = (v >> (n - 1u)) & 1u;
        g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) | (carry ? CPSR_C_BIT : 0u);
    }
    return v >> n;
}

extern "C" uint32_t arm_shift_asr(uint32_t v, uint32_t n, int set_carry) {
    if (n == 0) return v;
    if (n >= 32) {
        uint32_t carry = (v >> 31) & 1u;
        if (set_carry) {
            g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) | (carry ? CPSR_C_BIT : 0u);
        }
        return carry ? 0xFFFFFFFFu : 0u;
    }
    if (set_carry) {
        uint32_t carry = (v >> (n - 1u)) & 1u;
        g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) | (carry ? CPSR_C_BIT : 0u);
    }
    return static_cast<uint32_t>(static_cast<int32_t>(v) >> n);
}

extern "C" uint32_t arm_shift_ror(uint32_t v, uint32_t n, int set_carry) {
    if (n == 0) return v;
    n &= 31u;
    if (n == 0) {  // ROR by multiple of 32 → no change but use top bit as carry
        if (set_carry) {
            g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) |
                ((v & 0x80000000u) ? CPSR_C_BIT : 0u);
        }
        return v;
    }
    uint32_t r = (v >> n) | (v << (32u - n));
    if (set_carry) {
        g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) |
            ((r & 0x80000000u) ? CPSR_C_BIT : 0u);
    }
    return r;
}

// ── Flag updaters ──────────────────────────────────────────────────

extern "C" void arm_set_nz(uint32_t r) {
    uint32_t c = g_cpu.cpsr & ~(CPSR_N_BIT | CPSR_Z_BIT);
    if (r & 0x80000000u) c |= CPSR_N_BIT;
    if (r == 0)          c |= CPSR_Z_BIT;
    g_cpu.cpsr = c;
}

extern "C" void arm_set_nzc_logic(uint32_t r, uint32_t shifter_carry) {
    // shifter_carry is the carry-out from the operand2 shifter, or
    // the existing CPSR.C if the shift didn't produce one (encoded
    // by the codegen as `cpsr_c()`).
    uint32_t c = g_cpu.cpsr & ~(CPSR_N_BIT | CPSR_Z_BIT | CPSR_C_BIT);
    if (r & 0x80000000u) c |= CPSR_N_BIT;
    if (r == 0)          c |= CPSR_Z_BIT;
    if (shifter_carry)   c |= CPSR_C_BIT;
    g_cpu.cpsr = c;
}

extern "C" void arm_set_nzcv_add(uint32_t a, uint32_t b, uint32_t r) {
    uint32_t c = g_cpu.cpsr & ~(CPSR_N_BIT | CPSR_Z_BIT |
                                  CPSR_C_BIT | CPSR_V_BIT);
    if (r & 0x80000000u) c |= CPSR_N_BIT;
    if (r == 0)          c |= CPSR_Z_BIT;
    // Carry: unsigned overflow.
    if (r < a)           c |= CPSR_C_BIT;
    // Overflow: same-sign inputs, different-sign result.
    if ((~(a ^ b) & (a ^ r)) & 0x80000000u) c |= CPSR_V_BIT;
    g_cpu.cpsr = c;
}

extern "C" void arm_set_nzcv_adc(uint32_t a, uint32_t b, uint32_t c_in,
                                   uint32_t r) {
    uint32_t c = g_cpu.cpsr & ~(CPSR_N_BIT | CPSR_Z_BIT |
                                  CPSR_C_BIT | CPSR_V_BIT);
    if (r & 0x80000000u) c |= CPSR_N_BIT;
    if (r == 0)          c |= CPSR_Z_BIT;
    // Carry from full 33-bit addition.
    uint64_t wide = static_cast<uint64_t>(a) + b + c_in;
    if (wide >> 32)      c |= CPSR_C_BIT;
    if ((~(a ^ b) & (a ^ r)) & 0x80000000u) c |= CPSR_V_BIT;
    g_cpu.cpsr = c;
}

extern "C" void arm_set_nzcv_sub(uint32_t a, uint32_t b, uint32_t r) {
    uint32_t c = g_cpu.cpsr & ~(CPSR_N_BIT | CPSR_Z_BIT |
                                  CPSR_C_BIT | CPSR_V_BIT);
    if (r & 0x80000000u) c |= CPSR_N_BIT;
    if (r == 0)          c |= CPSR_Z_BIT;
    // Carry: NOT borrow. C=1 if a >= b.
    if (a >= b)          c |= CPSR_C_BIT;
    if (((a ^ b) & (a ^ r)) & 0x80000000u) c |= CPSR_V_BIT;
    g_cpu.cpsr = c;
}

extern "C" void arm_set_nzcv_sbc(uint32_t a, uint32_t b, uint32_t c_in,
                                   uint32_t r) {
    uint32_t c = g_cpu.cpsr & ~(CPSR_N_BIT | CPSR_Z_BIT |
                                  CPSR_C_BIT | CPSR_V_BIT);
    if (r & 0x80000000u) c |= CPSR_N_BIT;
    if (r == 0)          c |= CPSR_Z_BIT;
    // SBC: r = a - b - (1 - c_in). Carry-out = NOT borrow.
    // The 33-bit equivalent: a + ~b + c_in. Overflow if top bit.
    uint64_t wide = static_cast<uint64_t>(a) + (~b & 0xFFFFFFFFu) + c_in;
    if (wide >> 32)      c |= CPSR_C_BIT;
    if (((a ^ b) & (a ^ r)) & 0x80000000u) c |= CPSR_V_BIT;
    g_cpu.cpsr = c;
}

// ── Dispatch ───────────────────────────────────────────────────────

namespace {

// Binary-search a sorted DispatchEntry table for `pc` and current
// instruction-set state. Same numeric addresses can have both ARM
// and THUMB entries, so scan the equal-address run for CPSR.T.
void (*lookup_in(const DispatchEntry* table, unsigned len,
                 uint32_t pc, bool thumb))(void) {
    unsigned lo = 0, hi = len;
    while (lo < hi) {
        unsigned mid = (lo + hi) >> 1u;
        if (table[mid].addr < pc) lo = mid + 1u;
        else                       hi = mid;
    }
    for (unsigned i = lo; i < len && table[i].addr == pc; ++i) {
        if ((table[i].thumb != 0) == thumb) return table[i].fn;
    }
    return nullptr;
}

// PC range covered by the BIOS region (16 KB at 0x0). Anything in
// this range consults the BIOS table; anything else consults the
// cart table. The two ranges don't overlap so the order is just
// "BIOS for <4000, cart otherwise" — no merged search needed.
constexpr uint32_t kBiosRegionEnd = 0x00004000u;

}  // namespace

extern "C" void runtime_dispatch(uint32_t target_pc) {
    // Strip THUMB bit; codegen handles the mode via cpsr_T already.
    uint32_t pc = target_pc & ~1u;
    runtime_trace_event(RUNTIME_TRACE_DISPATCH, pc, target_pc, 0, 0);

    bool thumb = (g_cpu.cpsr & CPSR_T_BIT) != 0;
    if (pc >= 0x02000000u && pc < 0x04000000u &&
        g_runtime_ram_dispatch_hook &&
        g_runtime_ram_dispatch_hook(pc, thumb ? 1 : 0)) {
        return;
    }
    void (*fn)(void) = nullptr;
    if (pc < kBiosRegionEnd) {
        fn = lookup_in(kBiosDispatchTable, kBiosDispatchTableLen, pc, thumb);
    } else {
        fn = lookup_in(kDispatchTable, kDispatchTableLen, pc, thumb);
    }
    if (fn) { fn(); return; }
    // Stage-2 self-heal: third dispatch tier. After the static tables miss,
    // consult the runtime-healed native overlays before bridging. Defined in
    // src/runtime/overlay_loader.cpp (a null stub in tests/codegen/stubs.cpp,
    // since armv4t must not depend on the runtime lib). When the feature is
    // off this is a single bool check and returns 0.
    if (overlay_try_dispatch(pc, thumb ? 1 : 0)) return;
    runtime_dispatch_miss(target_pc);
}

extern "C" void runtime_dispatch_with_exchange(uint32_t target_pc) {
    // Bit 0 of target indicates THUMB.
    if (target_pc & 1u) g_cpu.cpsr |= CPSR_T_BIT;
    else                g_cpu.cpsr &= ~CPSR_T_BIT;
    runtime_trace_event(RUNTIME_TRACE_EXCHANGE, target_pc & ~1u, target_pc, 0, 0);
    runtime_dispatch(target_pc);
}

extern "C" int runtime_has_static_entry(uint32_t pc, int thumb) {
    uint32_t a = pc & ~1u;
    void (*fn)(void) = (a < kBiosRegionEnd)
        ? lookup_in(kBiosDispatchTable, kBiosDispatchTableLen, a, thumb != 0)
        : lookup_in(kDispatchTable, kDispatchTableLen, a, thumb != 0);
    return fn != nullptr ? 1 : 0;
}

extern "C" void runtime_call_push_return(uint32_t return_pc) {
    uint32_t pc = return_pc & ~1u;
    if (g_call_return_depth >= kCallReturnStackSize) {
        std::fprintf(stderr,
                     "runtime_arm: generated call-return stack overflow "
                     "at return_pc=0x%08X\n",
                     pc);
        runtime_trace_dump_recent(96);
        std::abort();
    }
    g_call_return_stack[g_call_return_depth++] = pc;
    runtime_trace_event(RUNTIME_TRACE_CALL, pc, pc, g_call_return_depth, 1u);
}

extern "C" int runtime_call_should_return(uint32_t target_pc) {
    uint32_t pc = target_pc & ~1u;
    // Scan only down to the active IRQ floor — never match an interrupted
    // mainline / enclosing-IRQ return frame.
    for (uint32_t i = g_call_return_depth; i != g_call_return_floor; --i) {
        uint32_t slot = i - 1u;
        if (g_call_return_stack[slot] == pc) {
            runtime_trace_event(RUNTIME_TRACE_CALL, pc, pc,
                                g_call_return_depth,
                                (slot + 1u == g_call_return_depth) ? 2u : 5u);
            g_call_return_depth = slot;
            return 1;
        }
    }
    uint32_t top = g_call_return_depth != 0
        ? g_call_return_stack[g_call_return_depth - 1u]
        : 0xFFFFFFFFu;
    runtime_trace_event(RUNTIME_TRACE_CALL, pc, top, g_call_return_depth, 3u);
    return 0;
}

extern "C" void runtime_call_cancel_return(uint32_t return_pc) {
    uint32_t pc = return_pc & ~1u;
    // Never pop below the active IRQ floor (an interrupted mainline frame).
    if (g_call_return_depth > g_call_return_floor &&
        g_call_return_stack[g_call_return_depth - 1u] == pc) {
        runtime_trace_event(RUNTIME_TRACE_CALL, pc, pc, g_call_return_depth,
                            4u);
        --g_call_return_depth;
    }
}

// ── Save-state accessors for the call-return stack ─────────────────
// The stack + depth are file-local (anonymous namespace above). These
// C-ABI windows let the snapshot orchestrator capture and restore them
// without pulling the debug headers into this translation unit.

extern "C" uint32_t runtime_call_stack_depth(void) {
    return g_call_return_depth;
}

extern "C" const uint32_t* runtime_call_stack_data(void) {
    return g_call_return_stack;
}

extern "C" void runtime_call_stack_restore(const uint32_t* entries,
                                           uint32_t depth) {
    if (depth > kCallReturnStackSize) depth = kCallReturnStackSize;
    g_call_return_depth = depth;
    for (uint32_t i = 0; i < depth; ++i) g_call_return_stack[i] = entries[i];
}

// runtime_dispatch_miss is defined in src/runtime/runtime_arm_default_aborts.cpp
// for production builds, or by test stubs for codegen tests.

// ── PSR transfer + bank machinery ──────────────────────────────────

namespace {

// Map a 5-bit ARM mode to the bank index in g_cpu.banked_*.
unsigned mode_to_bank(uint32_t mode) {
    switch (mode & 0x1Fu) {
        case 0x11u: return ARM_BANK_FIQ;
        case 0x12u: return ARM_BANK_IRQ;
        case 0x13u: return ARM_BANK_SUPERVISOR;
        case 0x17u: return ARM_BANK_ABORT;
        case 0x1Bu: return ARM_BANK_UNDEFINED;
        default:    return ARM_BANK_USER;  // User (0x10) / System (0x1F)
    }
}

// Save the active mode's R13/R14 into its bank; if leaving FIQ,
// also pickle R8..R12 into r8_12_fiq.
void bank_out(unsigned old_bank, uint32_t old_mode) {
    g_cpu.banked_sp[old_bank] = g_cpu.R[13];
    g_cpu.banked_lr[old_bank] = g_cpu.R[14];
    if ((old_mode & 0x1Fu) == 0x11u) {
        for (unsigned i = 0; i < 5; ++i) {
            g_cpu.r8_12_fiq[i] = g_cpu.R[8 + i];
        }
    } else {
        for (unsigned i = 0; i < 5; ++i) {
            g_cpu.r8_12_user[i] = g_cpu.R[8 + i];
        }
    }
}

// Restore the incoming mode's R13/R14; if entering FIQ, also
// pull R8..R12 from r8_12_fiq, else from r8_12_user.
void bank_in(unsigned new_bank, uint32_t new_mode) {
    g_cpu.R[13] = g_cpu.banked_sp[new_bank];
    g_cpu.R[14] = g_cpu.banked_lr[new_bank];
    if ((new_mode & 0x1Fu) == 0x11u) {
        for (unsigned i = 0; i < 5; ++i) {
            g_cpu.R[8 + i] = g_cpu.r8_12_fiq[i];
        }
    } else {
        for (unsigned i = 0; i < 5; ++i) {
            g_cpu.R[8 + i] = g_cpu.r8_12_user[i];
        }
    }
}

}  // namespace

extern "C" uint32_t runtime_read_user_reg(uint32_t reg) {
    reg &= 15u;
    uint32_t mode = g_cpu.cpsr & 0x1Fu;
    if (reg < 8u || reg == 15u) {
        return g_cpu.R[reg];
    }
    if (reg < 13u) {
        return (mode == 0x11u)
            ? g_cpu.r8_12_user[reg - 8u]
            : g_cpu.R[reg];
    }
    if (mode == 0x10u || mode == 0x1Fu) {
        return g_cpu.R[reg];
    }
    return (reg == 13u)
        ? g_cpu.banked_sp[ARM_BANK_USER]
        : g_cpu.banked_lr[ARM_BANK_USER];
}

extern "C" void runtime_write_user_reg(uint32_t reg, uint32_t value) {
    reg &= 15u;
    uint32_t mode = g_cpu.cpsr & 0x1Fu;
    if (reg < 8u || reg == 15u) {
        g_cpu.R[reg] = value;
        return;
    }
    if (reg < 13u) {
        if (mode == 0x11u) {
            g_cpu.r8_12_user[reg - 8u] = value;
        } else {
            g_cpu.R[reg] = value;
        }
        return;
    }
    if (mode == 0x10u || mode == 0x1Fu) {
        g_cpu.R[reg] = value;
    } else if (reg == 13u) {
        g_cpu.banked_sp[ARM_BANK_USER] = value;
    } else {
        g_cpu.banked_lr[ARM_BANK_USER] = value;
    }
}

extern "C" uint32_t runtime_mrs_cpsr(void) {
    return g_cpu.cpsr;
}

extern "C" uint32_t runtime_mrs_spsr(void) {
    unsigned bank = mode_to_bank(g_cpu.cpsr);
    return g_cpu.banked_spsr[bank];
}

extern "C" void runtime_msr_cpsr(uint32_t value, uint32_t mask) {
    uint32_t bytewise = 0;
    if (mask & 1u) bytewise |= 0x000000FFu;
    if (mask & 2u) bytewise |= 0x0000FF00u;
    if (mask & 4u) bytewise |= 0x00FF0000u;
    if (mask & 8u) bytewise |= 0xFF000000u;

    // User mode cannot touch the control byte (bits 7..0) — clamp.
    if ((g_cpu.cpsr & 0x1Fu) == 0x10u) {
        bytewise &= 0xFF000000u;
    }

    uint32_t old_cpsr = g_cpu.cpsr;
    uint32_t new_cpsr = (old_cpsr & ~bytewise) | (value & bytewise);

    unsigned old_bank = mode_to_bank(old_cpsr);
    unsigned new_bank = mode_to_bank(new_cpsr);

    g_cpu.cpsr = new_cpsr;

    if (old_bank != new_bank) {
        bank_out(old_bank, old_cpsr);
        bank_in(new_bank, new_cpsr);
    }
}

extern "C" void runtime_msr_spsr(uint32_t value, uint32_t mask) {
    unsigned bank = mode_to_bank(g_cpu.cpsr);
    if (bank == ARM_BANK_USER) {
        // SPSR is undefined in User / System modes — drop silently.
        return;
    }
    uint32_t bytewise = 0;
    if (mask & 1u) bytewise |= 0x000000FFu;
    if (mask & 2u) bytewise |= 0x0000FF00u;
    if (mask & 4u) bytewise |= 0x00FF0000u;
    if (mask & 8u) bytewise |= 0xFF000000u;
    uint32_t old = g_cpu.banked_spsr[bank];
    g_cpu.banked_spsr[bank] = (old & ~bytewise) | (value & bytewise);
}

// ── Exception return ───────────────────────────────────────────────

// IRQ nesting depth (defined below) and the depth at which the most recent
// IRQ-mode exception return (iret) fired. runtime_irq() uses the latter to
// know when its handler has fully unwound — see the re-dispatch loop there.
extern "C" uint32_t g_irq_nest_depth;
extern "C" uint32_t g_irq_iret_depth;

extern "C" void runtime_exception_return(uint32_t new_pc) {
    uint32_t old_cpsr = g_cpu.cpsr;
    uint32_t old_mode = old_cpsr & 0x1Fu;
    if (old_mode == 0x10u || old_mode == 0x1Fu) {
        // User / System have no SPSR; the exception-return form is
        // architecturally undefined. Just set PC.
        g_cpu.R[15] = new_pc;
        return;
    }
    unsigned old_bank = mode_to_bank(old_cpsr);
    uint32_t spsr = g_cpu.banked_spsr[old_bank];

    bank_out(old_bank, old_cpsr);
    g_cpu.cpsr = spsr;
    unsigned new_bank = mode_to_bank(spsr);
    bank_in(new_bank, spsr);

    g_cpu.R[15] = new_pc;

    // Returning FROM IRQ mode (0x12) is the iret that ends a hardware IRQ.
    // Record the nesting depth so runtime_irq()'s drive-to-completion loop
    // can tell when *its* IRQ (vs. a nested inner one) has returned. Set
    // after the bank swap so g_irq_nest_depth still reflects the level being
    // left (runtime_irq decrements only after its loop exits).
    if (old_mode == 0x12u) g_irq_iret_depth = g_irq_nest_depth;
}

extern "C" void runtime_restore_cpsr_from_spsr(void) {
    uint32_t old_cpsr = g_cpu.cpsr;
    uint32_t old_mode = old_cpsr & 0x1Fu;
    if (old_mode == 0x10u || old_mode == 0x1Fu) {
        return;
    }
    unsigned old_bank = mode_to_bank(old_cpsr);
    uint32_t spsr = g_cpu.banked_spsr[old_bank];

    bank_out(old_bank, old_cpsr);
    g_cpu.cpsr = spsr;
    unsigned new_bank = mode_to_bank(spsr);
    bank_in(new_bank, spsr);
}

// ── BIOS / SWI ─────────────────────────────────────────────────────
// Mirror ARM ARM A2.6.4 SWI entry. The recompiled SWI instruction's
// codegen already set g_cpu.R[15] = pc_of_swi + 4 (ARM) or +2 (THUMB)
// before calling this, so R[15] is the return address.
//
// Steps (ARM ARM A2.6.4):
//   LR_svc      ← return_address
//   SPSR_svc    ← CPSR
//   CPSR.mode   ← SVC (0x13)
//   CPSR.T      ← 0           (handler always runs in ARM state)
//   CPSR.I      ← 1           (mask IRQs while in SWI handler)
//   PC          ← 0x00000008
//
// Then we runtime_dispatch(0x08) into the recompiled BIOS SWI
// vector. NO interpreter fallback (PRINCIPLES.md "Interpreter is
// informative, never load-bearing"). If the BIOS dispatch table is
// empty, the dispatch falls through to runtime_dispatch_miss; the
// strong (production) version aborts there — that abort is the
// "BIOS not recompiled" gate.

extern "C" void runtime_swi(uint32_t swi_imm) {
    uint32_t return_address = g_cpu.R[15];
    uint32_t saved_cpsr     = g_cpu.cpsr;
    runtime_trace_event(RUNTIME_TRACE_SWI, return_address, swi_imm, saved_cpsr, 0);
    runtime_swi_log_record(swi_imm, return_address, g_cpu.R[0], g_cpu.R[1],
                           g_cpu.R[2], g_cpu.R[14], bus_read_u32(0x03007FF8u));

    // ── BIOS HLE (opt-in alternative to the recompiled/LLE BIOS) ────────
    // When an HLE handler is installed and it services this SWI, the guest
    // resumes at LR (already in g_cpu.R[15], set by the SWI codegen) with NO
    // SVC-mode entry and NO BIOS dispatch — the hook computes the effect on
    // g_cpu + memory directly and charges its own cycle cost. A return of 0
    // (default, and for any SWI the HLE layer does not implement) falls through
    // to the recompiled BIOS below. LLE therefore stays fully load-bearing and
    // remains the correctness oracle; HLE is purely additive. The SWI number is
    // decoded the way the real BIOS reads it: THUMB from the low byte, ARM from
    // bits 23..16 (arm_decode stores the full 24-bit comment in swi_imm).
    if (g_bios_hle_hook) {
        bool thumb = (saved_cpsr & CPSR_T_BIT) != 0;
        uint32_t swi_num = thumb ? (swi_imm & 0xFFu) : ((swi_imm >> 16) & 0xFFu);
        if (g_bios_hle_hook(swi_num)) return;
    }

    // Switch to SVC mode. SPSR_svc gets the pre-SWI CPSR. LR_svc gets
    // the return address. R8..R12 don't change unless leaving FIQ.
    uint32_t new_cpsr =
        (saved_cpsr & ~(0x1Fu | CPSR_T_BIT))  // clear mode + T
        | 0x13u                                // SVC
        | CPSR_I_BIT;                          // mask IRQs

    unsigned old_bank = mode_to_bank(saved_cpsr);
    unsigned new_bank = mode_to_bank(new_cpsr);
    if (old_bank != new_bank) {
        bank_out(old_bank, saved_cpsr);
        bank_in(new_bank, new_cpsr);
    }

    g_cpu.cpsr                  = new_cpsr;
    g_cpu.banked_spsr[new_bank] = saved_cpsr;
    g_cpu.R[14]                 = return_address;  // LR_svc
    g_cpu.R[15]                 = 0x00000008u;

    // Charge the SWI instruction's own cost (instr_cycle_base(SWI) = 3:
    // 2S+1N). Ticked here — AFTER CPSR.I is masked above — so a VBlank/
    // timer IRQ that becomes pending during these 3 cycles stays masked
    // until the handler re-enables it, matching the interpreter oracle
    // (enter_swi sets I=1, then pump_step(3), then the next-boundary IRQ
    // check sees I=1). The recompiled SWI codegen does not tick this op.
    runtime_tick(3u);

    runtime_dispatch(0x00000008u);
}

// Count of IRQ vectorings performed by the recompiled runtime (every
// runtime_irq call = one exception entry to 0x18). The recomp delivers IRQs
// here (called from runtime_tick), NOT in runtime.cpp's run loop, so this is
// the authoritative per-engine IRQ-entry tally. The TCP `counters` command
// surfaces it (runtime.cpp wires ctx.irq_entries here) so a probe can compare
// IRQ delivery against the interpreter oracle — the MC-HP-002 1-game-frame-lead
// test (does the recomp vector an extra/early VBlank IRQ?). Never reset; probes
// compare per-frame deltas.
extern "C" unsigned long long g_runtime_irq_entries = 0;
// Live host-recursion nesting depth of IRQ delivery (++ on entry, -- after the
// handler unwinds) and the high-water mark. Distinguishes the MC-HP-002 storm
// shape: deep nesting (depth climbs → a long handler spanning the next IRQ
// boundary) vs. flat re-fire (depth stays ~1 → an unacked source re-vectoring).
// Non-static so runtime_should_yield() (runtime_bus_bridge.cpp) can tell whether
// an IRQ handler is currently on the host stack — the cpsr mode alone can't,
// because GBA IRQ dispatchers (e.g. FireRed's intr_main) switch to System mode
// mid-handler to allow nested IRQs.
extern "C" uint32_t      g_irq_nest_depth = 0;
extern "C" unsigned long long g_runtime_irq_max_depth = 0;
// Depth at which the most recent IRQ-mode iret fired (set in
// runtime_exception_return). runtime_irq() resets this to 0 on entry and
// spins its drive-to-completion loop until it equals the IRQ's own depth.
extern "C" uint32_t      g_irq_iret_depth = 0;

extern "C" void runtime_irq(uint32_t return_address) {
    ++g_runtime_irq_entries;
    ++g_irq_nest_depth;
    if (g_irq_nest_depth > g_runtime_irq_max_depth)
        g_runtime_irq_max_depth = g_irq_nest_depth;
    // Debug: dump the always-on ring the moment IRQ nesting reaches a
    // threshold, capturing the storm chain BEFORE later busy execution scrolls
    // it out (MC-HP-002). Env-gated, zero-overhead when unset.
    static int irq_depth_gate = -1;
    if (irq_depth_gate < 0) {
        const char* e = std::getenv("GBARECOMP_ABORT_ON_IRQ_DEPTH");
        irq_depth_gate = e ? std::atoi(e) : 0;
    }
    if (irq_depth_gate > 0 && (int)g_irq_nest_depth >= irq_depth_gate) {
        std::fprintf(stderr,
                     "runtime_irq: nesting depth %u reached (entries=%llu) "
                     "ret=0x%08X cpsr=0x%08X\n",
                     g_irq_nest_depth,
                     (unsigned long long)g_runtime_irq_entries,
                     return_address, g_cpu.cpsr);
        runtime_trace_dump_recent(160);
        std::abort();
    }
    uint32_t saved_cpsr = g_cpu.cpsr;
    // Record the active IRQ source(s) (IE & IF) in the trace addr field and the
    // nesting depth in aux so a ring dump names which interrupt is being
    // vectored and how deep — used to pin the MC-HP-002 IRQ storm. Reading
    // IE/IF is side-effect-free.
    uint32_t irq_src = bus_read_u16(0x04000200u) & bus_read_u16(0x04000202u);
    runtime_trace_event(RUNTIME_TRACE_IRQ, return_address, irq_src, saved_cpsr,
                        g_irq_nest_depth);
    runtime_irq_log_record(irq_src, return_address, saved_cpsr);

    uint32_t new_cpsr =
        (saved_cpsr & ~(0x1Fu | CPSR_T_BIT))
        | 0x12u
        | CPSR_I_BIT;

    unsigned old_bank = mode_to_bank(saved_cpsr);
    unsigned new_bank = mode_to_bank(new_cpsr);
    if (old_bank != new_bank) {
        bank_out(old_bank, saved_cpsr);
        bank_in(new_bank, new_cpsr);
    }

    g_cpu.cpsr                  = new_cpsr;
    g_cpu.banked_spsr[new_bank] = saved_cpsr;
    g_cpu.R[14]                 = return_address + 4u;
    g_cpu.R[15]                 = 0x00000018u;

    // Drive the IRQ handler to completion.
    //
    // A single runtime_dispatch(0x18) finishes the handler ONLY if nothing
    // inside it triggers a "return-to-top" unwind. A BIOS SWI executed inside
    // the handler does exactly that: the SWI return path leaves R15 at the
    // after-SWI PC and unwinds the host C stack via the per-call-site cancel
    // cascade (runtime_call_cancel_return) — the SAME mechanism a SWI in the
    // main thread uses to unwind back to step_frame's dispatch loop, which
    // then re-dispatches R15. runtime_irq has no such loop, so the cascade
    // would abandon the handler mid-flight: e.g. FireRed's VBlank handler
    // (VBlankCB_Copyright -> LoadOam -> CpuSet/SWI) never reaches intr_main's
    // post-handler `REG_IE = saved` restore, leaving VBlank masked in IE
    // forever -> the game's VBlank busy-wait spins -> permanent freeze.
    //
    // Mirror the main loop here: keep re-dispatching R15 until THIS IRQ's
    // handler has executed its iret (an IRQ-mode runtime_exception_return at
    // our nesting depth). g_irq_nest_depth stays > 0 across the whole loop so
    // runtime_should_yield() never unwinds us mid-handler. Nested IRQs are
    // handled because the iret depth is matched to this frame's depth; the
    // saved/restored g_irq_iret_depth keeps an enclosing IRQ's loop intact.
    uint32_t my_depth      = g_irq_nest_depth;
    uint32_t saved_iret    = g_irq_iret_depth;
    g_irq_iret_depth       = 0u;  // sentinel: this IRQ has not iret'd yet
    // Raise the call-return floor so the handler's guest returns/cancels can
    // unwind back to here but never match/pop the interrupted mainline (or an
    // enclosing IRQ's) frames — see g_call_return_floor. Restored below.
    uint32_t saved_floor   = g_call_return_floor;
    g_call_return_floor    = g_call_return_depth;
    runtime_dispatch(0x00000018u);
    constexpr uint32_t kMaxIrqDispatches = 4'000'000u;
    uint32_t irq_guard = 0u;
    while (g_irq_iret_depth != my_depth) {
        if (++irq_guard >= kMaxIrqDispatches) {
            std::fprintf(stderr,
                "runtime_irq: handler at depth %u did not iret after %u "
                "dispatches (R15=0x%08X, cpsr=0x%08X) — abandoning\n",
                my_depth, irq_guard, g_cpu.R[15], g_cpu.cpsr);
            runtime_trace_dump_recent(160);
            break;
        }
        runtime_dispatch(g_cpu.R[15]);
    }
    g_irq_iret_depth = saved_iret;  // restore an enclosing IRQ's expectation
    g_call_return_floor = saved_floor;
    --g_irq_nest_depth;
}

// runtime_unimplemented_op is defined in
// src/runtime/runtime_arm_default_aborts.cpp for production builds,
// or by test stubs for codegen tests.

// ── Lifecycle ──────────────────────────────────────────────────────

extern "C" void runtime_init(void* bus_handle) {
    gbarecomp::runtime_arm::g_bus_handle = bus_handle;
    g_call_return_depth = 0;
}

extern "C" void runtime_shutdown(void) {
    gbarecomp::runtime_arm::g_bus_handle = nullptr;
    g_call_return_depth = 0;
}
