# TCC_BACKEND.md — toolchain-free Stage-2 self-heal producer

**Status: implemented (2026-06-25).** Replaces the former in-process sljit JIT
producer. This doc describes how a SHIPPED, compiler-less player box still
produces a validated native shard for an uncovered PC — by compiling the same
overlay C the gcc path emits with a bundled **TinyCC (tcc)**.

Companion docs: `PRINCIPLES.md` (honest self-healing, coverage honesty,
interpreter never load-bearing), `docs/ARCHITECTURE.md`, `docs/ROADMAP.md`.
Sibling precedent: `../../psxrecomp/psxrecomp/` ships the same gcc-or-tcc overlay
model (`runtime/src/overlay_backend.c`, `tools/compile_overlays.py`).

---

## 0. Why tcc, not sljit

The self-heal tier needs a way to produce the native shard **without a host
toolchain**, because a shipped consumer build has no g++. The previous answer
was an in-process ARM→sljit machine-code emitter. It carried a permanent,
dominant cost: a *second* codegen path that had to stay bit- and cycle-identical
to the C codegen forever (a mis-compile is silent + fatal).

tcc removes that cost entirely. It compiles **the exact C the gcc path already
emits** (`overlay_emit.cpp`), so there is no parallel emitter to keep in
lockstep — one code source, validated once. tcc is self-contained (its own
linker + headers), sub-second per function, and BSD-licensed. The tradeoff
(baseline-quality codegen vs gcc -O2) is irrelevant here: tcc only carries a
given player's not-yet-folded long tail, and is still vastly faster than the
interpreter (the actual status-quo alternative for a toolchain-less user).

---

## 1. Tier model (unchanged except the 2b producer)

```
Tier 1  STATIC NATIVE   AOT-recompiled corpus. kDispatchTable / kBiosDispatchTable.
Tier 2  HEAL SHARD      produced the first time a PC would fall to the interpreter;
                        registered in g_healed, cached on disk, warm-loaded next run.
       2a gcc DLL        emit C (overlay_emit) -> g++ -> .dll. DEV DEFAULT + release.
       2b tcc DLL        emit the SAME C -> bundled tcc -> .dll. PLAYER-BOX PRODUCER.
Tier 3  INTERP FLOOR    runtime_dispatch_miss() bridges through the interpreter,
                        loudly logged + recorded. Correctness floor + diff oracle.
```

Both 2a and 2b produce the identical loadable artifact (`overlay_abi`,
`overlay_init(cb)`, `func_<pc>`), reached through the same `GbaOverlayCallbacks`
ABI and the same `overlay_try_dispatch` hot-path tier. The worker thread does
all compilation; the game thread never compiles (no audio stall).

---

## 2. Backend resolution (`overlay_loader.cpp::resolve_backend`)

`GBARECOMP_HEAL_BACKEND = gcc | tcc | auto` (default **auto**).

- `auto` → **gcc if a real g++/gcc/cc/clang is reachable on PATH** (a dev /
  production box, `gcc_toolchain_available()`), else **tcc** (the bundled,
  toolchain-free fallback).
- `gcc` / `tcc` force that producer.

Resolved once at init and logged: `self_heal_recompile=ENABLED backend=… …`.

---

## 3. The emitted overlay is dual C / C++ (`overlay_emit.cpp`)

The per-function body the recompiler emits is plain C (`g_cpu.R[..] = …`,
C-style casts, C99 mixed declarations, labels, calls to the shim thunks). The
ABI/shim headers (`overlay_abi.h`, `overlay_runtime_arm.h`, `runtime_arm_types.h`)
are already `#ifdef __cplusplus`-guarded. So the emitted file compiles under
**both** g++ (as C++) and tcc (as C). The three entry points are wrapped in:

```c
#ifdef _WIN32
#define OVL_DLLEXPORT __declspec(dllexport)
#else
#define OVL_DLLEXPORT __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
#define OVL_EXPORT extern "C" OVL_DLLEXPORT
#else
#define OVL_EXPORT OVL_DLLEXPORT
#endif
```

`OVL_EXPORT` gives C linkage under C++ and DLL-exports the symbol under both
toolchains — necessary because tcc has no `-Wl,--export-all-symbols` (the flag
the gcc command relied on). Keep the emitted body dual-toolchain-clean: **no
C++-only syntax** (templates / references / `static_cast` / namespaces / `bool`).

---

## 4. The compile commands (`overlay_compile.cpp`)

One `overlay_compile_one(..., HealBackend backend, ...)` builds the command:

- **gcc**: `g++ -O2 -std=gnu++17 -fno-exceptions -fno-rtti -shared -I…runtime
  -I…armv4t -o out.dll -x c++ in.c -Wl,--export-all-symbols`
- **tcc**: `tcc -shared -I…runtime -I…armv4t -o out.dll in.c`
  (no `-O`, no `-x c++` — tcc is a C compiler; `-shared` exports the dllexport'd
  symbols).

`tcc_path()` resolves the binary: `GBARECOMP_HEAL_TCC` env override >
`<exe_dir>/overlay_toolchain/tcc/tcc.exe` (the staged bundle) > `tcc` on PATH.
`GBARECOMP_HEAL_CXX` overrides the g++ path.

---

## 5. Cache namespacing (`overlay_loader.cpp`)

```
recomp_cache/<image_sha1>/gcc/<os-arch>/<pc>_<crc>_<a|t>.dll
recomp_cache/<image_sha1>/tcc/<os-arch>/<pc>_<crc>_<a|t>.dll
```

The worker writes to the active backend's dir. Warm-load scans **gcc first, then
tcc**, deduped by `(pc,thumb)` so a shipped gcc DLL supersedes a player's local
tcc shard for the same function — **consumption is producer-blind, gcc > tcc**.
The `<os-arch>` token keeps cross-arch artifacts from colliding. The per-file
`GBA_OVERLAY_ABI_VERSION` gate still rejects stale artifacts at load.

---

## 6. Bundling for releases (in-process — simpler than psxrecomp)

psxrecomp re-runs its recompiler via embedded Python to emit overlay C, so its
release bundles python + tcc + the recompiler + `compile_overlays.py` + headers.
gba emits the overlay C **in-process** (`overlay_emit`), so a gba release bundles
only two things next to the exe:

```
<exe>/overlay_toolchain/
  tcc/        TinyCC 0.9.27 — tcc.exe + libtcc.dll + include/ + lib/ (own headers)
  include/    the 3 overlay shim headers: overlay_runtime_arm.h, overlay_abi.h,
              runtime_arm_types.h (flattened; they #include each other by name)
```

`tools/fetch_tcc.ps1 -Toolchain <stage>\overlay_toolchain -EngineRoot <engine>`
stages both; each game's `tools/make_release.ps1` calls it after copying the exe.

At runtime (`overlay_compile.cpp`): `tcc_path()` finds `<exe>/overlay_toolchain/
tcc/tcc.exe`, and `overlay_include_flags()` compiles each healed function against
`<exe>/overlay_toolchain/include` whenever the baked dev `GBARECOMP_SRC_DIR` is
absent (a shipped, source-less box). Both gcc and tcc use this resolution, so the
shipped self-heal path is complete with **no system python or gcc**. A dev box
needs nothing staged — `GBARECOMP_SRC_DIR` exists and `auto` resolves to g++.

---

## 7. What stays (the producer-agnostic spine)

Discovery (`function_finder`), the dispatch tier (`overlay_try_dispatch`), the
miss hook (`overlay_request_compile`), the content-keyed cache + warm-load + ABI
gate, the worker/queue threading, and coverage honesty are all unchanged — tcc
is only a second *code source* behind the existing `HealBackend` seam. The L1
per-IrOp differential harness (`tests/codegen`) validates the C codegen both
producers consume.
