// overlay_loader.cpp — see overlay_loader.h.

#include "overlay_loader.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "overlay_abi.h"
#include "overlay_compile.h"
#include "overlay_sljit_heal.h"   // in-process sljit producer (toolchain-less)
#include "sljit_gate.h"           // P6 differential gate (validate shard vs interp)
#include "runtime_arm.h"          // g_cpu, g_runtime_*, every runtime/bus/arm fn
#include "runtime_bus_bridge.h"   // active_bus
#include "../gba/gba_bus.h"       // rom_ptr / rom_size
#include "../gba/gba_bios.h"      // GbaBios snapshot

namespace fs = std::filesystem;

namespace gbarecomp {

namespace {

// ── Key: (pc & ~1) << 1 | thumb ────────────────────────────────────
inline uint64_t heal_key(uint32_t pc, bool thumb) {
    return (static_cast<uint64_t>(pc & ~1u) << 1) | (thumb ? 1u : 0u);
}

// ── Healed native function (game-thread-only) ──────────────────────
struct HealedEntry {
    uint32_t addr  = 0;
    bool     thumb = false;
    void   (*fn)(void) = nullptr;
    void*    module = nullptr;   // gcc: HMODULE; sljit: the JIT code block
    uint32_t crc = 0;
    uint32_t end = 0;
    uint64_t native_calls = 0;
    bool     sljit = false;      // produced in-process (no DLL) vs a gcc DLL
    bool     leaf  = false;      // sljit only: makes no calls → P6-gate-eligible
};

// ── Worker → game-thread result ────────────────────────────────────
struct ReadyEntry {
    uint64_t key = 0;
    bool     ok = false;
    OverlayCompiled c;
};

// ── State ──────────────────────────────────────────────────────────
bool                     s_active = false;     // feature on AND inited
bool                     s_ever_active = false;  // ever inited this session (sticky)
std::string              s_cache_dir;          // cache_root/<image_sha1>
std::vector<uint8_t>     s_bios_bytes;         // 16 KB BIOS snapshot (base 0)
GbaOverlayCallbacks      g_callbacks{};

// Game-thread-only:
std::unordered_map<uint64_t, HealedEntry> g_healed;
std::unordered_set<uint64_t>              s_inflight;
std::unordered_set<uint64_t>              s_failed;
uint64_t                                  s_native_calls_total = 0;

// Production backend for the heal tier. When sljit, a dispatch miss is healed
// SYNCHRONOUSLY in-process (no toolchain, no DLL, no worker) — see
// overlay_request_compile. Resolved once at init from GBARECOMP_HEAL_BACKEND
// (=sljit), so the gcc DLL path stays the default + byte-identical.
bool                                      s_use_sljit = false;
uint64_t                                  s_sljit_healed = 0;

// Force-heal demo (GBARECOMP_SLJIT_FORCE_HEAL=N): force the first N supported,
// dispatch-reached functions to MISS the static tables so they self-heal via
// sljit even on a fully-static build (a witnessed heal without a regen). s_force_yes
// = chosen (always force-missed thereafter → served by the JIT'd shard); s_force_no
// = already rejected (unsupported / undiscoverable), cached so we don't re-decode.
unsigned                                  s_force_heal_limit = 0;
std::unordered_set<uint64_t>              s_force_yes;
std::unordered_set<uint64_t>              s_force_no;

// Cross-thread work/ready queues:
std::mutex                  s_work_mtx;
std::condition_variable     s_work_cv;
std::deque<OverlayWorkItem> s_work;
std::thread                 s_worker;
std::atomic<bool>           s_stop{false};

std::mutex                  s_ready_mtx;
std::deque<ReadyEntry>      s_ready;
std::atomic<int>            s_ready_pending{0};

// ── Region resolution: the immutable code image a PC lives in ──────
bool region_bytes(uint32_t pc, const uint8_t** bytes, std::size_t* size,
                  uint32_t* base) {
    if (pc < 0x00004000u) {
        if (s_bios_bytes.empty()) return false;
        *bytes = s_bios_bytes.data();
        *size  = s_bios_bytes.size();
        *base  = 0u;
        return true;
    }
    gba::GbaBus* bus = active_bus();
    if (!bus || !bus->rom_ptr() || bus->rom_size() == 0) return false;
    *bytes = bus->rom_ptr();
    *size  = bus->rom_size();
    *base  = 0x08000000u;
    return true;
}

// ── DLL callback table ─────────────────────────────────────────────
// runtime_should_yield returns bool (C++); the ABI field is int. Adapt rather
// than reinterpret_cast a differing return type.
int ovl_should_yield(void) { return runtime_should_yield() ? 1 : 0; }

void fill_callbacks() {
    g_callbacks.abi_version = GBA_OVERLAY_ABI_VERSION;

    g_callbacks.cpu                = &g_cpu;
    g_callbacks.runtime_insn_trace = &g_runtime_insn_trace;
    g_callbacks.runtime_cycles     = &g_runtime_cycles;
    g_callbacks.runtime_break_pc   = &g_runtime_break_pc;

    g_callbacks.bus_read_u32  = bus_read_u32;
    g_callbacks.bus_read_u16  = bus_read_u16;
    g_callbacks.bus_read_u8   = bus_read_u8;
    g_callbacks.bus_write_u32 = bus_write_u32;
    g_callbacks.bus_write_u16 = bus_write_u16;
    g_callbacks.bus_write_u8  = bus_write_u8;

    g_callbacks.arm_cond_passes   = arm_cond_passes;
    g_callbacks.arm_shift_lsl     = arm_shift_lsl;
    g_callbacks.arm_shift_lsr     = arm_shift_lsr;
    g_callbacks.arm_shift_asr     = arm_shift_asr;
    g_callbacks.arm_shift_ror     = arm_shift_ror;
    g_callbacks.arm_set_nz        = arm_set_nz;
    g_callbacks.arm_set_nzc_logic = arm_set_nzc_logic;
    g_callbacks.arm_set_nzcv_add  = arm_set_nzcv_add;
    g_callbacks.arm_set_nzcv_adc  = arm_set_nzcv_adc;
    g_callbacks.arm_set_nzcv_sub  = arm_set_nzcv_sub;
    g_callbacks.arm_set_nzcv_sbc  = arm_set_nzcv_sbc;

    g_callbacks.runtime_dispatch               = runtime_dispatch;
    g_callbacks.runtime_dispatch_with_exchange = runtime_dispatch_with_exchange;
    g_callbacks.runtime_call_push_return       = runtime_call_push_return;
    g_callbacks.runtime_call_should_return     = runtime_call_should_return;
    g_callbacks.runtime_call_cancel_return     = runtime_call_cancel_return;

    g_callbacks.runtime_tick       = runtime_tick;
    g_callbacks.runtime_should_yield = ovl_should_yield;
    g_callbacks.runtime_mem_cycles = runtime_mem_cycles;
    g_callbacks.runtime_mul_cycles = runtime_mul_cycles;

    g_callbacks.runtime_swi                  = runtime_swi;
    g_callbacks.runtime_irq                  = runtime_irq;
    g_callbacks.runtime_mrs_cpsr             = runtime_mrs_cpsr;
    g_callbacks.runtime_mrs_spsr             = runtime_mrs_spsr;
    g_callbacks.runtime_msr_cpsr             = runtime_msr_cpsr;
    g_callbacks.runtime_msr_spsr             = runtime_msr_spsr;
    g_callbacks.runtime_read_user_reg        = runtime_read_user_reg;
    g_callbacks.runtime_write_user_reg       = runtime_write_user_reg;
    g_callbacks.runtime_exception_return     = runtime_exception_return;
    g_callbacks.runtime_restore_cpsr_from_spsr = runtime_restore_cpsr_from_spsr;

    g_callbacks.runtime_insn_fp          = runtime_insn_fp;
    g_callbacks.runtime_trace_event      = runtime_trace_event;
    g_callbacks.runtime_unimplemented_op = runtime_unimplemented_op;
}

// ── Worker thread ──────────────────────────────────────────────────
void worker_main() {
    for (;;) {
        OverlayWorkItem w;
        {
            std::unique_lock<std::mutex> lk(s_work_mtx);
            s_work_cv.wait(lk, [] { return s_stop.load() || !s_work.empty(); });
            if (s_stop.load() && s_work.empty()) return;
            w = s_work.front();
            s_work.pop_front();
        }

        ReadyEntry r;
        r.key = heal_key(w.pc, w.thumb);
        std::string err;
        r.ok = overlay_compile_one(w, s_cache_dir, &g_callbacks,
                                   /*compile_if_missing=*/true, &r.c, &err);
        if (r.ok) {
            std::fprintf(stderr,
                "self_heal: HEALED 0x%08X (%s) -> native (crc=%08X, "
                "[0x%08X,0x%08X)); the interpreter bridge stops for this PC.\n",
                w.pc, w.thumb ? "thumb" : "arm", r.c.crc, w.pc, r.c.end);
        } else {
            std::fprintf(stderr,
                "self_heal: compile FAILED for 0x%08X (%s): %s — staying on the "
                "interpreter bridge this session.\n",
                w.pc, w.thumb ? "thumb" : "arm", err.c_str());
        }

        {
            std::lock_guard<std::mutex> lk(s_ready_mtx);
            s_ready.push_back(r);
            s_ready_pending.fetch_add(1, std::memory_order_release);
        }
    }
}

bool env_truthy(const char* name) {
    const char* v = std::getenv(name);
    if (!v || !v[0]) return false;
    return !(std::strcmp(v, "0") == 0 || std::strcmp(v, "false") == 0 ||
             std::strcmp(v, "off") == 0);
}

// Warm-load every cached DLL for this image (load-only: never compiles at
// startup). Filenames are "<pc:08X>_<crc:08X>_<a|t>.dll"; we recover (pc, mode)
// and let overlay_compile_one re-derive the extent + validate the CRC.
int warm_load_cache() {
    std::error_code ec;
    if (!fs::exists(s_cache_dir, ec)) return 0;
    int loaded = 0;
    std::unordered_set<uint64_t> seen;
    for (const auto& de : fs::directory_iterator(s_cache_dir, ec)) {
        if (ec) break;
        if (!de.is_regular_file()) continue;
        const std::string fn = de.path().filename().string();
        // "PPPPPPPP_CCCCCCCC_m.dll" == 8 + 1 + 8 + 1 + 1 + 4 = 23 chars.
        if (fn.size() != 23 || fn.compare(19, 4, ".dll") != 0) continue;
        if (fn[8] != '_' || fn[17] != '_') continue;
        char mode = fn[18];
        if (mode != 'a' && mode != 't') continue;
        const uint32_t pc =
            static_cast<uint32_t>(std::strtoul(fn.substr(0, 8).c_str(), nullptr, 16));
        const bool thumb = (mode == 't');
        const uint64_t key = heal_key(pc, thumb);
        if (!seen.insert(key).second) continue;

        OverlayWorkItem w;
        w.pc = pc;
        w.thumb = thumb;
        if (!region_bytes(pc, &w.bytes, &w.size, &w.base)) continue;

        OverlayCompiled c;
        std::string err;
        if (overlay_compile_one(w, s_cache_dir, &g_callbacks,
                                /*compile_if_missing=*/false, &c, &err)) {
            HealedEntry h;
            h.addr = pc; h.thumb = thumb; h.fn = c.fn; h.module = c.module;
            h.crc = c.crc; h.end = c.end;
            g_healed[key] = h;
            ++loaded;
        }
    }
    return loaded;
}

}  // namespace

void overlay_loader_init(const std::string& cache_root,
                         const std::string& image_sha1,
                         const gba::GbaBios* bios) {
    if (!env_truthy("GBARECOMP_SELFHEAL_RECOMPILE")) {
        s_active = false;
        std::printf("self_heal_recompile=DISABLED "
                    "(set GBARECOMP_SELFHEAL_RECOMPILE=1 to heal misses to "
                    "native; this session is a pure Stage-1 interpreter "
                    "bridge)\n");
        return;
    }

    const char* root_env = std::getenv("GBARECOMP_HEAL_CACHE");
    const std::string root =
        (root_env && root_env[0]) ? root_env
                                  : (cache_root.empty() ? "recomp_cache"
                                                        : cache_root);
    s_cache_dir = (fs::path(root) / image_sha1).string();
    std::error_code ec;
    fs::create_directories(s_cache_dir, ec);

    // Snapshot the 16 KB BIOS as the immutable code image for BIOS heals.
    s_bios_bytes.clear();
    if (bios && bios->loaded()) {
        s_bios_bytes.resize(gba::GbaBios::kSize);
        for (std::size_t i = 0; i < gba::GbaBios::kSize; ++i) {
            s_bios_bytes[i] = bios->read8(static_cast<uint32_t>(i));
        }
    }

    fill_callbacks();
    s_active = true;
    s_ever_active = true;  // sticky: survives shutdown for the exit report

    // Production backend: sljit (in-process, toolchain-less) when requested,
    // else the gcc DLL path (default). gcc stays byte-identical.
    {
        const char* be = std::getenv("GBARECOMP_HEAL_BACKEND");
        s_use_sljit = (be != nullptr && std::strcmp(be, "sljit") == 0);
        const char* fh = std::getenv("GBARECOMP_SLJIT_FORCE_HEAL");
        s_force_heal_limit = fh ? static_cast<unsigned>(std::strtoul(fh, nullptr, 10)) : 0u;
        if (s_force_heal_limit && s_use_sljit)
            std::printf("self_heal: FORCE-HEAL demo armed for up to %u functions\n",
                        s_force_heal_limit);
    }

    const int warm = warm_load_cache();
    s_stop.store(false);
    s_worker = std::thread(worker_main);

    std::printf("self_heal_recompile=ENABLED backend=%s cache=\"%s\" warm_loaded=%d\n",
                s_use_sljit ? "sljit" : "gcc", s_cache_dir.c_str(), warm);
}

void overlay_loader_shutdown() {
    if (s_worker.joinable()) {
        s_stop.store(true);
        s_work_cv.notify_all();
        s_worker.join();
    }
    // DLLs are left loaded (immutable code, process-lifetime); the OS reclaims
    // them at exit. Drain any last results so counters/banner are accurate.
    overlay_drain_ready();
    s_active = false;
}

// Produce + register an sljit shard for (pc, thumb) into g_healed (the caller
// has already resolved region_bytes + dedup). Returns false if the emitter
// declines. Internal-linkage so overlay_should_force_miss can call it too.
static bool register_sljit_heal(uint32_t pc, bool thumb, const uint8_t* bytes,
                                std::size_t size, uint32_t base, const char* tag) {
    void (*fn)(void) = nullptr;
    void* code = nullptr;
    uint32_t end = 0;
    bool leaf = false;
    if (!overlay_sljit_produce(pc, thumb, bytes, size, base, &fn, &code, &end, &leaf))
        return false;
    HealedEntry h;
    h.addr = pc; h.thumb = thumb; h.fn = fn; h.module = code;
    h.end = end; h.sljit = true; h.leaf = leaf;
    g_healed[heal_key(pc, thumb)] = h;
    ++s_sljit_healed;
    std::printf("self_heal: %s 0x%08X (%s) [0x%08X,0x%08X) — sljit native\n",
                tag, pc, thumb ? "thumb" : "arm", pc, end);
    return true;
}

void overlay_request_compile(uint32_t pc, bool thumb) {
    if (!s_active) return;
    pc &= ~1u;
    const uint64_t key = heal_key(pc, thumb);
    if (g_healed.count(key) || s_inflight.count(key) || s_failed.count(key)) {
        return;
    }

    const uint8_t* bytes = nullptr;
    std::size_t size = 0;
    uint32_t base = 0;
    if (!region_bytes(pc, &bytes, &size, &base)) {
        // No immutable image (e.g. a RAM PC) — Stage 4 territory. Don't retry.
        s_failed.insert(key);
        return;
    }

    if (s_use_sljit) {
        // In-process JIT on the game thread: produce now, register now — the
        // next hit of this PC dispatches native. No worker, no DLL, no toolchain.
        if (!register_sljit_heal(pc, thumb, bytes, size, base, "sljit-JIT'd")) {
            // Emitter declined (an op it can't lower yet, or undiscoverable):
            // stay on the honest interpreter bridge for this PC this session.
            s_failed.insert(key);
            std::printf("self_heal: sljit DECLINED 0x%08X (%s) — interp bridge "
                        "(emitter gap; precision over recall)\n",
                        pc, thumb ? "thumb" : "arm");
        }
        return;
    }

    // gcc path: hand the region to the worker thread for an async compile.
    OverlayWorkItem w;
    w.pc = pc; w.thumb = thumb;
    w.bytes = bytes; w.size = size; w.base = base;
    s_inflight.insert(key);
    {
        std::lock_guard<std::mutex> lk(s_work_mtx);
        s_work.push_back(w);
    }
    s_work_cv.notify_one();
}

void overlay_drain_ready() {
    if (s_ready_pending.load(std::memory_order_acquire) == 0) return;
    std::deque<ReadyEntry> local;
    {
        std::lock_guard<std::mutex> lk(s_ready_mtx);
        local.swap(s_ready);
        s_ready_pending.store(0, std::memory_order_release);
    }
    for (const ReadyEntry& r : local) {
        s_inflight.erase(r.key);
        if (r.ok) {
            HealedEntry h;
            h.addr = r.c.pc; h.thumb = r.c.thumb; h.fn = r.c.fn;
            h.module = r.c.module; h.crc = r.c.crc; h.end = r.c.end;
            g_healed[r.key] = h;
        } else {
            s_failed.insert(r.key);
        }
    }
}

bool overlay_query(uint32_t pc, bool thumb, uint64_t* native_calls) {
    auto it = g_healed.find(heal_key(pc, thumb));
    if (it == g_healed.end()) return false;
    if (native_calls) *native_calls = it->second.native_calls;
    return true;
}

void overlay_counters(uint64_t* healed_native, uint64_t* native_calls_total,
                      uint64_t* inflight, uint64_t* failed) {
    if (healed_native)      *healed_native      = g_healed.size();
    if (native_calls_total) *native_calls_total = s_native_calls_total;
    if (inflight)           *inflight           = s_inflight.size();
    if (failed)             *failed             = s_failed.size();
}

bool overlay_enabled() { return s_active; }

// Sticky: true once the heal feature initialized this session, and stays true
// after overlay_loader_shutdown() so the exit coverage report (which runs
// after shutdown drains/joins the worker) honestly states the feature was on.
bool overlay_was_enabled() { return s_ever_active; }

}  // namespace gbarecomp

// ── Hot-path dispatch tier (extern "C" — reached from gbarecomp_armv4t's
// runtime_dispatch, which must not depend on the runtime lib) ──────────
extern "C" int overlay_try_dispatch(uint32_t pc, int thumb) {
    if (!gbarecomp::s_active) return 0;
    auto it = gbarecomp::g_healed.find(
        gbarecomp::heal_key(pc, thumb != 0));
    if (it == gbarecomp::g_healed.end() || !it->second.fn) return 0;
    gbarecomp::HealedEntry& e = it->second;

    // P6 differential gate (default off): for an sljit shard not yet promoted,
    // run the interpreter pass (kept result) + validate the shard against it.
    if (e.sljit && gbarecomp::sljit_gate::enabled()) {
        switch (gbarecomp::sljit_gate::on_dispatch(pc, thumb != 0, e.leaf, e.fn)) {
            case gbarecomp::sljit_gate::Decision::FallThrough:
                return 0;  // pinned → the interpreter bridge runs it
            case gbarecomp::sljit_gate::Decision::Handled:
                return 1;  // gate already executed the function (via interp)
            case gbarecomp::sljit_gate::Decision::RunBlind:
                break;     // promoted / not gate-eligible → run native below
        }
    }

    ++e.native_calls;
    ++gbarecomp::s_native_calls_total;
    e.fn();
    return 1;
}

// Force-heal demo hook (off unless GBARECOMP_SLJIT_FORCE_HEAL=N): when it returns
// 1, runtime_dispatch skips the static tables for this PC. We pick the first N
// distinct, supported, dispatch-reached functions; each then misses → bridges
// once through the interpreter → heals via sljit → runs as the JIT'd shard on
// every subsequent hit. Zero cost when the demo is off (one branch). Game-thread
// only (same as runtime_dispatch), so the sets need no locking.
extern "C" int overlay_should_force_miss(uint32_t pc, int thumb_i) {
    if (!gbarecomp::s_active || gbarecomp::s_force_heal_limit == 0) return 0;
    pc &= ~1u;
    if (pc < 0x00004000u) return 0;  // never the BIOS (vectors/handlers are special)
    const bool thumb = (thumb_i != 0);
    const uint64_t key = gbarecomp::heal_key(pc, thumb);
    if (gbarecomp::s_force_yes.count(key)) return 1;  // chosen → keep missing it
    if (gbarecomp::s_force_no.count(key)) return 0;
    if (gbarecomp::g_healed.count(key)) { gbarecomp::s_force_yes.insert(key); return 1; }
    if (gbarecomp::s_force_yes.size() >= gbarecomp::s_force_heal_limit) return 0;

    const uint8_t* bytes = nullptr;
    std::size_t size = 0;
    uint32_t base = 0;
    if (!gbarecomp::region_bytes(pc, &bytes, &size, &base)) {
        gbarecomp::s_force_no.insert(key);
        return 0;
    }
    // Heal NOW and register the shard, so overlay_try_dispatch serves it on THIS
    // hit — bypassing the interpreter bridge (whose stop-address contract does
    // not fit a force-missed function and would otherwise run away).
    if (gbarecomp::register_sljit_heal(pc, thumb, bytes, size, base, "FORCE-HEAL")) {
        gbarecomp::s_force_yes.insert(key);
        return 1;
    }
    gbarecomp::s_force_no.insert(key);
    return 0;
}
