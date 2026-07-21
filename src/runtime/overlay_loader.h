// overlay_loader.h — Stage-2 self-heal dispatch tier + background compiler.
//
// Stage 1 bridges a dispatch miss through the interpreter on EVERY hit. Stage 2
// closes the loop: the first miss for a PC enqueues a background compile (emit
// → g++ → cached DLL → LoadLibrary); once the native function lands, a third
// dispatch tier (overlay_try_dispatch) routes that PC to native code on every
// subsequent hit, and the interpreter bridge stops running for it.
//
// Threading: g_healed (the PC → native-fn table) is written ONLY by the game
// thread (overlay_drain_ready moves finished work in at a frame boundary), so
// the hot-path read in overlay_try_dispatch is lock-free. Only the work/ready
// queues cross to the worker thread (mutex-protected).
//
// The whole feature is gated behind GBARECOMP_SELFHEAL_RECOMPILE: when unset,
// overlay_loader_init no-ops, overlay_try_dispatch always returns 0, and the
// runtime is a pure Stage-1 bridge (used as the A/B baseline for the GFP1 gate).
//
// Doctrine (PRINCIPLES.md): healed-from-cache is STILL NOT fully static — the
// build is done only when the merged TOML makes the static corpus cover it.
// See self_heal.h.
// GBARECOMP_STRICT_STATIC=1 is the acceptance gate: it disables cache loading,
// background compilation, and interpreter bridging; any dispatch miss aborts.

#pragma once

#include <cstdint>
#include <string>

namespace gba { class GbaBios; }

#ifdef __cplusplus
extern "C" {
#endif

// Hot-path dispatch tier (defined in overlay_loader.cpp; a null-returning stub
// in tests/codegen/stubs.cpp). Called from runtime_dispatch in
// src/armv4t/runtime_arm.cpp AFTER the static dispatch tables miss and BEFORE
// runtime_dispatch_miss. `pc` is already THUMB-bit-stripped. Returns 1 (and
// runs the native overlay) iff this PC has healed to native, else 0.
int overlay_try_dispatch(uint32_t pc, int thumb);

#ifdef __cplusplus
}  // extern "C"
#endif

namespace gbarecomp {

// Set the cache root + per-image id and (if GBARECOMP_SELFHEAL_RECOMPILE is on)
// fill the DLL callback table, snapshot the BIOS bytes, warm-load any cached
// DLLs for this image, and start the background compile worker. `image_sha1`
// keys the cache subdir (recomp_cache/<image_sha1>/). `bios` supplies the 16 KB
// BIOS snapshot used as the code image for BIOS-region (pc < 0x4000) heals.
void overlay_loader_init(const std::string& cache_root,
                         const std::string& image_sha1,
                         const gba::GbaBios* bios);

// Stop + join the worker. Safe to call when disabled / never inited.
void overlay_loader_shutdown();

// Enqueue a background compile for the function at (pc, thumb), unless it is
// already healed, in flight, or known-failed. No-op when disabled. Called from
// the Stage-1 miss path (runtime_arm_default_aborts.cpp) right after the miss
// is logged. Returns true only when the PC is dispatchable immediately.
bool overlay_request_compile(uint32_t pc, bool thumb);

// Game-thread: move any worker-finished entries into g_healed (and remember
// failures so they aren't retried). Cheap when idle (one atomic load). Call
// once per step on the game thread.
void overlay_drain_ready();

// True if (pc, thumb) has healed to native; if so and native_calls != null,
// reports how many times the native function has been dispatched.
bool overlay_query(uint32_t pc, bool thumb, uint64_t* native_calls);

// Total wall-time the GAME THREAD has spent compiling shards. Both producers
// (gcc and tcc) run on the worker thread, so the game thread never compiles and
// this is always 0 — kept so the coverage banner's stall metric stays wired.
// Nanoseconds, monotonic.
uint64_t overlay_game_thread_compile_ns();

// Aggregate counters for the coverage banner + the live `misses` TCP command.
// Any out-param may be null.
void overlay_counters(uint64_t* healed_native, uint64_t* native_calls_total,
                      uint64_t* inflight, uint64_t* failed);

// True if the self-heal recompiler is currently active (init'd, not yet shut
// down). Reads false after overlay_loader_shutdown().
bool overlay_enabled();

// True once the heal feature initialized this session; stays true after
// shutdown, so the exit coverage report states it honestly (the loader is
// torn down before the report runs). Prefer this for end-of-run summaries.
bool overlay_was_enabled();

}  // namespace gbarecomp
