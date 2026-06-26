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
#include "overlay_compile.h"      // overlay_compile_one, HealBackend, heal_backend_name
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
    void*    module = nullptr;   // HMODULE of the loaded shard DLL
    uint32_t crc = 0;
    uint32_t end = 0;
    uint64_t native_calls = 0;
};

// ── Worker → game-thread result ────────────────────────────────────
struct ReadyEntry {
    uint64_t key = 0;
    bool     ok = false;
    OverlayCompiled c;
};

// ── Host arch token for the cache namespace ────────────────────────
// Keeps a Windows-x64 gcc DLL and a future Linux-arm64 tcc DLL for the same
// function in separate subdirs so a stale-arch artifact never wins.
const char* overlay_arch_abi() {
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64))
    return "windows-x64";
#elif defined(_WIN32)
    return "windows-x86";
#elif defined(__APPLE__) && defined(__aarch64__)
    return "macos-arm64";
#elif defined(__APPLE__)
    return "macos-x64";
#elif defined(__aarch64__)
    return "linux-arm64";
#else
    return "linux-x64";
#endif
}

// ── State ──────────────────────────────────────────────────────────
bool                     s_active = false;       // feature on AND inited
bool                     s_ever_active = false;   // ever inited this session (sticky)
std::string              s_cache_base;            // cache_root/<image_sha1>
std::string              s_active_cache_dir;      // base/<backend>/<arch> (worker writes here)
HealBackend              s_backend = HealBackend::Gcc;  // resolved producer
std::vector<uint8_t>     s_bios_bytes;            // 16 KB BIOS snapshot (base 0)
GbaOverlayCallbacks      g_callbacks{};

// Game-thread-only:
std::unordered_map<uint64_t, HealedEntry> g_healed;
std::unordered_set<uint64_t>              s_inflight;
std::unordered_set<uint64_t>              s_failed;
uint64_t                                  s_native_calls_total = 0;

// Cross-thread work/ready queues:
std::mutex                  s_work_mtx;
std::condition_variable     s_work_cv;
std::deque<OverlayWorkItem> s_work;
std::thread                 s_worker;
std::atomic<bool>           s_stop{false};

std::mutex                  s_ready_mtx;
std::deque<ReadyEntry>      s_ready;
std::atomic<int>            s_ready_pending{0};

// The cache subdir a given backend writes to / is scanned from:
//   recomp_cache/<image_sha1>/<gcc|tcc>/<os-arch>/<pc>_<crc>_<a|t>.dll
std::string cache_dir_for(HealBackend b) {
    return (fs::path(s_cache_base) / heal_backend_name(b) / overlay_arch_abi())
        .string();
}

// ── Toolchain probe: is a real g++/gcc reachable on PATH? ───────────
// Determines the `auto` backend: a dev/production box (gcc present) produces
// optimized gcc DLLs; a toolchain-less player box falls back to bundled tcc.
bool gcc_toolchain_available() {
    static int s_cached = -1;
    if (s_cached >= 0) return s_cached != 0;
    int found = 0;
    if (const char* path = std::getenv("PATH"); path && *path) {
#ifdef _WIN32
        const char sep = ';';
        static const char* exes[] = {"g++.exe", "gcc.exe", "cc.exe", "clang.exe"};
#else
        const char sep = ':';
        static const char* exes[] = {"g++", "gcc", "cc", "clang"};
#endif
        const char* p = path;
        while (*p && !found) {
            const char* e = std::strchr(p, sep);
            std::size_t dlen = e ? static_cast<std::size_t>(e - p) : std::strlen(p);
            if (dlen > 0 && dlen < 480) {
                for (const char* exe : exes) {
                    char cand[512];
                    std::snprintf(cand, sizeof(cand), "%.*s/%s",
                                  static_cast<int>(dlen), p, exe);
                    if (std::FILE* f = std::fopen(cand, "rb")) {
                        std::fclose(f);
                        found = 1;
                        break;
                    }
                }
            }
            if (!e) break;
            p = e + 1;
        }
    }
    s_cached = found;
    return found != 0;
}

// Resolve the production heal backend: GBARECOMP_HEAL_BACKEND = gcc | tcc |
// auto (default auto). `auto` prefers gcc when a real toolchain is reachable
// (the dev/release-quality producer), else the bundled, toolchain-free tcc.
HealBackend resolve_backend() {
    const char* be = std::getenv("GBARECOMP_HEAL_BACKEND");
    if (be && be[0]) {
        if (std::strcmp(be, "gcc") == 0) return HealBackend::Gcc;
        if (std::strcmp(be, "tcc") == 0) return HealBackend::Tcc;
        // auto-no-gcc: pretend this is a toolchain-less player box even though
        // gcc is present — force tcc + the bundled include (heal_simulate_shipped
        // in overlay_compile.cpp). Mirrors psxrecomp's OVERLAY_BACKEND_AUTO_NO_GCC.
        if (std::strcmp(be, "auto-no-gcc") == 0) return HealBackend::Tcc;
        // anything else (incl. "auto") → auto-resolve below
    }
    return gcc_toolchain_available() ? HealBackend::Gcc : HealBackend::Tcc;
}

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
    g_callbacks.runtime_fn_entry_hook = &g_runtime_fn_entry_hook;

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
// The game thread NEVER compiles. Every miss hands the immutable region to the
// worker, which emits the overlay C and runs the resolved compiler (gcc or
// tcc) to a cached DLL; the game thread installs it into g_healed at a frame
// boundary (overlay_drain_ready). Keeps the 60 fps loop + audio stall-free.
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
        r.ok = overlay_compile_one(w, s_active_cache_dir, &g_callbacks,
                                   /*compile_if_missing=*/true, s_backend,
                                   &r.c, &err);
        if (r.ok) {
            std::fprintf(stderr,
                "self_heal: HEALED 0x%08X (%s) -> native via %s (crc=%08X, "
                "[0x%08X,0x%08X)); the interpreter bridge stops for this PC.\n",
                w.pc, w.thumb ? "thumb" : "arm", heal_backend_name(s_backend),
                r.c.crc, w.pc, r.c.end);
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

// Warm-load every cached DLL in `dir` (load-only: never compiles at startup).
// Filenames are "<pc:08X>_<crc:08X>_<a|t>.dll"; we recover (pc, mode) and let
// overlay_compile_one re-derive the extent + validate the CRC. `seen` dedups
// across the gcc and tcc namespaces so the higher-priority one (scanned first)
// wins — consumption is producer-blind.
int warm_load_cache_dir(const std::string& dir, HealBackend backend,
                        std::unordered_set<uint64_t>& seen) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) return 0;
    int loaded = 0;
    for (const auto& de : fs::directory_iterator(dir, ec)) {
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
        if (!seen.insert(key).second) continue;  // already covered (gcc wins)

        OverlayWorkItem w;
        w.pc = pc;
        w.thumb = thumb;
        if (!region_bytes(pc, &w.bytes, &w.size, &w.base)) continue;

        OverlayCompiled c;
        std::string err;
        if (overlay_compile_one(w, dir, &g_callbacks,
                                /*compile_if_missing=*/false, backend, &c, &err)) {
            HealedEntry h;
            h.addr = pc; h.thumb = thumb; h.fn = c.fn; h.module = c.module;
            h.crc = c.crc; h.end = c.end;
            g_healed[key] = h;
            ++loaded;
        }
    }
    return loaded;
}

// Warm-load both producer namespaces, gcc first so a shipped gcc DLL supersedes
// a player's local tcc shard for the same function (gcc > tcc consumption).
int warm_load_cache() {
    std::unordered_set<uint64_t> seen;
    int loaded = 0;
    loaded += warm_load_cache_dir(cache_dir_for(HealBackend::Gcc),
                                  HealBackend::Gcc, seen);
    loaded += warm_load_cache_dir(cache_dir_for(HealBackend::Tcc),
                                  HealBackend::Tcc, seen);
    return loaded;
}

}  // namespace

void overlay_loader_init(const std::string& cache_root,
                         const std::string& image_sha1,
                         const gba::GbaBios* bios) {
    // Self-improving native healing is ON by default so released games heal
    // interpreter misses to native AND persist them across launches (the cached
    // DLLs warm-load next session). Opt out for pure-interpreter runs (oracle
    // diff / cycle-accurate trace) with GBARECOMP_SELFHEAL_RECOMPILE=0.
    const char* sh_env = std::getenv("GBARECOMP_SELFHEAL_RECOMPILE");
    const bool sh_off = sh_env && (std::strcmp(sh_env, "0") == 0 ||
                                   std::strcmp(sh_env, "false") == 0 ||
                                   std::strcmp(sh_env, "off") == 0);
    if (sh_off) {
        s_active = false;
        std::printf("self_heal_recompile=DISABLED "
                    "(GBARECOMP_SELFHEAL_RECOMPILE=0 — pure Stage-1 interpreter "
                    "bridge this session)\n");
        return;
    }

    const char* root_env = std::getenv("GBARECOMP_HEAL_CACHE");
    const std::string root =
        (root_env && root_env[0]) ? root_env
                                  : (cache_root.empty() ? "recomp_cache"
                                                        : cache_root);
    s_cache_base = (fs::path(root) / image_sha1).string();

    // Resolve the producer (env > auto) and create its namespaced cache dir.
    s_backend = resolve_backend();
    s_active_cache_dir = cache_dir_for(s_backend);
    std::error_code ec;
    fs::create_directories(s_active_cache_dir, ec);

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

    int warm = warm_load_cache();
    s_stop.store(false);
    s_worker = std::thread(worker_main);

    std::printf("self_heal_recompile=ENABLED backend=%s cache=\"%s\" warm_loaded=%d\n",
                heal_backend_name(s_backend), s_active_cache_dir.c_str(), warm);
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

    // Production rule: the game thread NEVER compiles. Hand the region to the
    // worker thread; the game thread keeps interpreting this PC until the shard
    // is installed at a frame boundary (overlay_drain_ready). First hit
    // enqueues — it does not compile.
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
            // Game-thread install: publish the worker-produced code into the
            // dispatch map. This validates + installs only — it never compiles.
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

uint64_t overlay_game_thread_compile_ns() {
    // The game thread never compiles any more (both gcc and tcc run on the
    // worker thread). Always 0 — kept so the coverage banner's stall metric
    // stays wired and reads as a clean zero.
    return 0;
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

    ++e.native_calls;
    ++gbarecomp::s_native_calls_total;
    e.fn();
    return 1;
}
