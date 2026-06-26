// overlay_compile.h — compile (or load from cache) ONE self-heal overlay
// function. The mechanical half of Stage 2: given a guest PC + the immutable
// code image it lives in, discover its extent, emit its C (overlay_emit), run
// g++ to a cached DLL, then LoadLibrary + ABI-gate + overlay_init +
// GetProcAddress so the host can call it natively.
//
// This unit is deliberately stateless and thread-agnostic: overlay_loader.cpp
// owns the worker thread, the work/ready queues, and g_healed, and calls
// overlay_compile_one() from the worker (compile_if_missing=true) and from the
// init-time warm-cache scan (compile_if_missing=false, load-only).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "overlay_abi.h"  // GbaOverlayCallbacks

namespace gbarecomp {

// Which compiler turns the emitted overlay C into the cached DLL. Both produce
// the SAME emitted source (overlay_emit makes it C-and-C++-clean) and the SAME
// loadable artifact; they differ only in the toolchain and the cache namespace:
//   * Gcc  — the dev default + release-quality producer. Needs a real g++ on the
//            box; compiles the overlay as C++ (-x c++), optimized (-O2).
//   * Tcc  — the bundled, toolchain-free producer for shipped player boxes.
//            Self-contained (own linker + headers); compiles the overlay as C.
// Consumption is producer-blind (the loader scans both cache namespaces, gcc
// first), so a shipped gcc DLL supersedes a player's local tcc shard.
enum class HealBackend { Gcc, Tcc };

const char* heal_backend_name(HealBackend b);

// One unit of self-heal work: the entry PC + ISA, plus the contiguous code
// image it lives in. `bytes[guest_addr - base]` is the byte at guest_addr;
// the image is immutable for the run (cart ROM or a BIOS snapshot), so the
// pointer is safe to read from the worker thread.
struct OverlayWorkItem {
    uint32_t       pc    = 0;
    bool           thumb = false;
    const uint8_t* bytes = nullptr;
    std::size_t    size  = 0;
    uint32_t       base  = 0;
};

// The result of a successful compile-or-load.
struct OverlayCompiled {
    uint32_t pc    = 0;
    bool     thumb = false;
    uint32_t crc   = 0;      // CRC32 of [pc, end) — keys the cache filename
    uint32_t end   = 0;      // exclusive end of the discovered function
    void*    module = nullptr;  // HMODULE (kept loaded for the process lifetime)
    void   (*fn)(void) = nullptr;
};

// Discover the function at w.pc, derive its cache filename
// "<pc:08X>_<crc:08X>_<a|t>.dll" under cache_dir, and:
//   * if the DLL already exists → load it (skip compile);
//   * else if compile_if_missing → emit the C, run g++, atomic-rename, load;
//   * else (warm-scan, load-only) → return false (let it heal at runtime).
// On a successful load: ABI-gate the DLL, call its overlay_init(cb), resolve
// func_<pc>, fill *out, return true. On any failure: return false and set
// *err (the caller logs it loudly and stays on the interpreter bridge).
//
// `backend` selects the compiler used when compile_if_missing actually builds;
// it is irrelevant on the load-only path (a DLL on disk loads the same way
// whoever produced it). The caller passes the cache_dir already namespaced for
// that backend (recomp_cache/<sha1>/<gcc|tcc>/<os-arch>), so the filename keys
// stay producer-local.
bool overlay_compile_one(const OverlayWorkItem& w,
                         const std::string& cache_dir,
                         const GbaOverlayCallbacks* cb,
                         bool compile_if_missing,
                         HealBackend backend,
                         OverlayCompiled* out,
                         std::string* err);

}  // namespace gbarecomp
