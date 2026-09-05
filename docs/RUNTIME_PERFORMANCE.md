# Runtime performance

This note records the low-end runtime optimization pass that removed default diagnostic work from normal gameplay while preserving the interpreter fallback, runtime fallback, cross-state routing, and save-state layout.

The target for this pass was older and low-end hosts, with original Xbox-class hardware as the explicit performance target. No Xbox port or Xbox hardware measurement was performed. The numbers below are Windows host measurements and source-layout estimates from this worktree.

## Normal runtime profile

Normal gameplay should not pay for diagnostic capture unless the corresponding flag is set. The current opt-in diagnostic flags are:

| Surface | Enable flag | Default runtime cost avoided |
| --- | --- | --- |
| Runtime event trace | `GBARECOMP_RUNTIME_TRACE=1` or existing trace/watchpoint env vars | Hot-path trace writes from generated guard sites and runtime helpers |
| MMIO write trace | `GBARECOMP_MMIO_CAP=1` | 262144-entry MMIO ring allocation and committed-IO write logging |
| MMIO CSV dump | `GBARECOMP_MMIO_DUMP=<path>` | Also arms MMIO capture so shutdown dumps contain history |
| FIFO timer trace | `GBARECOMP_AUDIO_FIFO_TRACE=1` | Direct-sound FIFO timer trace entries and storage |
| Present cadence trace | `GBARECOMP_PRESENT_CADENCE=1` | SDL/DWM present timing, cadence ring allocation, periodic summaries, close-time CSV dump |
| Frame-phase trace | `GBARECOMP_FRAME_PHASE=<path>` | Per-presented-frame wall-clock timestamp collection and CSV dump |
| Hang watchdog | `GBARECOMP_HANG_WATCHDOG=1` | Runtime hang trip detection; tune with `GBARECOMP_HANG_SECONDS` |

These flags are observability controls. They do not change guest state by design, and the disabled path must remain the expected path for normal play.

## Correctness boundaries

This pass does not add a mandatory determinism requirement. Guest correctness, fallback behavior, and recoverability remain the requirements. Deterministic replays and trace rings are still useful diagnostic tools, but normal optimization should not preserve diagnostic overhead solely to make every internal trace surface always populated.

Interpreter fallback and runtime fallback routing are unchanged. The save-state layout is unchanged: diagnostic FIFO trace data remains outside serialized guest state, and the MMIO/runtime/present/frame-phase diagnostic rings are host observability state rather than guest state. Cross-state and fallback-adjacent tests remain smoke-level in this tree, so optimization changes should continue to avoid hidden coupling between diagnostic history and guest execution.

Regenerated game code is required to get the generated inline `runtime_trace_enabled()` guard sites. Old generated code can still call the runtime trace helper, but it will not get the new same-line generated guard coverage until the game is regenerated.

Overlay ABI version 5 appends the runtime trace enabled pointer to the callback table. That intentionally changes the overlay cache namespace to `abi5`; stale overlay caches rebuild instead of mixing callback layouts.

## Allocation impact

On the current 64-bit host layout, the combined optional buffers removed or deferred by this pass are approximately:

| Surface | Estimate |
| --- | ---: |
| MMIO capture ring | ~6 MiB |
| Frame-phase ring | ~640 KiB |
| Present-cadence ring | ~512 KiB |
| Raw color LUT | ~96 KiB |
| FIFO trace storage | ~32 KiB |

The total is roughly 7.25 MiB of default optional allocation avoided on this host layout. This combines diagnostic buffers and the raw presentation LUT; it is an allocation estimate, not guaranteed resident RSS. The runtime trace storage itself still has a fixed BSS footprint, so this should not be read as removing all debug memory. The FIFO trace uses a lazily resized `std::vector`, preserving `GbaAudio` copy behavior while keeping the disabled path allocation-free.

## CPU microbenchmark

On September 5, 2026, the validation note in `.local/runtime-validation.md` measured actual runtime functions from this worktree against baseline commit `425d941`. The benchmark used Clang from the `cmake-clang-v1` toolchain with `-O3 -DNDEBUG`. Median ns/call over 10 samples per variant/mode, with 10,000,000 iterations per sample:

| Function path | Baseline default/unset | Current default/unset | Default delta | Baseline with `GBARECOMP_RUNTIME_TRACE=1` | Current with `GBARECOMP_RUNTIME_TRACE=1` | Enabled delta |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `runtime_trace_event` direct call | 3.212 | 1.848 | 42.5% lower | 3.218 | 3.250 | 1.0% higher |
| `runtime_dispatch` miss path | 4.938 | 2.794 | 43.4% lower | 4.899 | 5.457 | 11.4% higher |
| `runtime_call_push_return` + matching `runtime_call_should_return` | 7.576 | 1.661 | 78.1% lower | 7.604 | 7.829 | 3.0% higher |

Trace copy counts matched the intended behavior: baseline default copied entries because it always recorded; current default copied zero entries because the gate is off; baseline and current enabled modes copied trace entries.

The repeated MMIO/FIFO host microbenchmarks in `.local/performance-validation.md` link the actual repository libraries and use 30,000,000 iterations per sample with five disabled/default samples per variant:

| Path | Baseline default median | Current default median | Result | Enabled-path check |
| --- | ---: | ---: | --- | --- |
| `GbaAudio::timer_overflow` FIFO trace path | 7.422 ns/op | 5.929 ns/op | 20.1% lower | Trace enabled recorded 1024 entries in one behavior check at 7.710 ns/op |
| `GbaIo::write16/read16` MMIO path | 3.599 ns/op | 3.650 ns/op | Neutral within noise | Capture enabled recorded 30,000,000 writes in one behavior check at 4.115 ns/op |

The MMIO result should be treated as neutral within run-to-run noise, not as a speedup.

Benchmark caveats:

- This is an offline CPU microbenchmark, not a game workload and not an Xbox measurement.
- The dispatch path uses codegen test stub tables, so every dispatch misses and reaches the non-aborting stub `runtime_dispatch_miss`. It measures dispatch/miss plus trace overhead, not generated-function hit throughput.
- Enabled tracing is intentionally allowed to cost more. In the microbenchmark, current enabled dispatch is slower than baseline because the branch guard remains on the hot path while tracing is enabled. That cost is for diagnostics and should not affect default gameplay with trace disabled.
- The MMIO/FIFO benchmark machine was not isolated, so use medians as directional validation and the measured ranges as the noise floor.

## Validation status

The final headless CTest run passed 29/29 tests, recorded in `.local/final-headless-ctest.log`. This includes the focused enabled/disabled diagnostic capture tests, raw color LUT tests, codegen trace guard coverage, interpreter smoke, bus/DMA/timer checks, PPU smoke, audio DRC, mod audio/state, save/config, symbol lookup, and self-heal cluster checks. The named ARM, Thumb, and IRQ test executables currently include stub coverage, so they should not be read as substantive ISA or IRQ validation.

A separate `.local/build-release-sdl` configuration found SDL2 at `C:/msys64/mingw64` and compiled the SDL-backed runtime/`host_window.cpp` path. The SDL dummy host-window smoke was run with present-cadence off and on; the enabled run dumped 4 presents to `.local/present_cadence_smoke.csv`.

The MinishCap baseline-vs-current integration smoke in `.local/runtime-validation.md` built the existing generated MinishCap sources twice: once against baseline engine `425d941`, and once against this worktree. The first smoke ran 60 headless frames and matched the same `final_pc=0x08016e94`, `steps=115`, `cycles=9076584`, `ppu_frames=60`, `unmapped=0`, `io_unhandled=0`, three BIOS ARM fallback misses, and `2,289,244` interpreted instructions. The final `frame60.bmp`, EEPROM save, coverage JSON, and miss fragment were byte-identical by SHA-256.

The stronger MinishCap smoke reused the same generated-game binaries and ran 1200 headless frames to the MinishCap title screen from fresh isolated directories `.local/minish-smoke-baseline-run-1200b` and `.local/minish-smoke-current-run-1200b`. Both runs used `GBARECOMP_SELFHEAL_RECOMPILE=0`, `GBARECOMP_RUNTIME_TRACE=0`, no input, final BMP dump, isolated EEPROM save output, and the existing HLE BIOS configuration with LLE/interpreter fallback. Recompiled BIOS output was absent in both builds, so this does not validate statically recompiled BIOS code.

Both 1200-frame MinishCap runs exited 0 and matched the same guest-visible summary: `final_pc=0x08016e86`, `steps=1730`, `cycles=153932314`, `ppu_frames=1200`, `unmapped=0`, and `io_unhandled=0`. Both reported four fallback misses: BIOS ARM `0x00000008` x1192, BIOS ARM `0x00000018` x2398, BIOS ARM `0x00000138` x2398, and RAM ARM `0x030065A8` x795 near `gf_ram_sub_080ADA04`. Both reported `67,554,453` interpreted instructions with self-heal native compilation disabled. The final `frame1200.bmp`, `minishcap_usa.sav`, `recomp_coverage_BZME.json`, and `recomp_master_misses_BZME.toml.frag` outputs were byte-identical by SHA-256.

These MinishCap runs are generated-game correctness smokes for no-input startup/title-screen paths, not full-game FPS results. They do not validate dynamic overlay load, healed native overlay execution, static recompiled BIOS execution, frame-phase enabled mode in a real game, or deeper gameplay ARM/Thumb cross-state transitions beyond what the 1200-frame headless sequence reaches.

## Next opportunity

A read-only baseline 64-bit PE inspection of the MinishCap build shows `.text` raw size at 37,401,600 bytes, about 35.67 MiB, and `SizeOfImage` at 42,692,608 bytes, about 40.72 MiB. This is not an Xbox measurement, not RSS, and not proof that source codegen alone accounts for every byte, but it is large enough to justify a follow-up pass on generated-code size, section layout, and cold instruction footprint for low-end targets.

## Remaining work

Original Xbox-class performance is the target but remains unvalidated on target hardware. The current evidence covers host-side source changes, microbenchmarks, and MinishCap no-input startup correctness smokes through 1200 frames; it is not a final platform performance report.
