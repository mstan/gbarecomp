# Architecture

## Recomp vs decomp — design boundary

This is a **static recompiler**. We take the original GBA ROM's
ARM/THUMB machine code and lift it into native C/C++ that runs
against a principled GBA hardware/runtime model.

This is **not**:

- A decompilation port. We do not lift the game to high-level C and
  then re-implement gameplay against a host engine.
- A reimplementation. We do not "rewrite Minish Cap in C" — that work
  exists upstream (`zeldaret/tmc`) and is a reference, not our
  artifact.
- An HLE wrapper. We do not stub SWIs to "what the game expects" or
  ship a runner that bypasses VRAM/OAM/DMA/IRQ.

The line is sharp. Decomp resources are valuable; we just pull them
through the right channel.

### Things we may borrow from disassembly / decomp resources

- Symbol maps (function names → addresses).
- Function boundaries (where one function ends and the next begins).
- ROM layout knowledge (sections, alignment, linker behavior).
- Asset / data labels (what a given table is, what a given pointer
  points to).
- Known ROM hashes for version targeting.
- Annotations that document, e.g., "this function is called from
  IRQ" or "this function returns to THUMB."
- Hardware reference behavior from emulators and hardware docs.

### Things we must NOT borrow as truth

- Decompiled C as an execution oracle. The emulator is the oracle;
  the decomp is at best a hypothesis.
- PC-port replacement renderers, audio mixers, input handlers.
- "Convenience" HLE: re-implemented gameplay helpers, fake IRQ
  dispatch, fake DMA fast paths.
- Renderer shortcuts that bypass actual VRAM/OAM/PAL writes.
- Audio shortcuts that bypass FIFO / timer / DMA semantics.
- Anything that makes a specific game "work" without a documented
  hardware basis.

If a behavior is real GBA hardware behavior, it goes in
`src/gba/` after consulting a primary reference (GBATEK, mGBA,
hardware test ROMs). If it's a Minish Cap quirk, it lives in
`MinishCapRecomp/` config or `src/`, never in the core.

---

## Layer separation

```
+---------------------------+   +---------------------------+
|     src/armv4t/           |   |     src/gba/              |
|   ARM7TDMI / ARMv4T:       |   |   GBA-specific hardware:  |
|     - decoders             |   |     - bus + memory map     |
|     - IR                   |   |     - IO registers          |
|     - codegen              |   |     - IRQ / DMA / timers    |
|     - condition codes      |   |     - PPU / audio / input   |
|     - flags / shifter      |   |     - BIOS / SWIs           |
|     - interworking         |   |     - save chip detection   |
|     - block discovery      |   |     - scheduler             |
|                            |   |                            |
|   Reusable for ARM9.       |   |   GBA-only.                 |
+-------------+--------------+   +--------------+--------------+
              |                                 |
              |             +-------------------+
              v             v
        +---------------------------------+
        |        src/runtime/             |
        |  - dispatch_call / dispatch     |
        |  - host_platform glue           |
        |  - generated_dispatch shim       |
        +-------------+-------------------+
                      |
                      v
        +---------------------------------+
        |        src/debug/               |
        |  - TCP debug server             |
        |  - always-on frame ring          |
        |  - rdb_* (per-store/block/call) |
        |  - snapshot / save-state        |
        +---------------------------------+
```

The split is intentional: `src/armv4t/` is portable across any
ARMv4T host (and largely re-usable for ARMv5T after extension).
`src/gba/` is aggressively GBA-specific because the GBA bus / IO
/ PPU / DMA / audio / save model is the entire point.

Don't bleed the layers. ARM9 assumptions don't go in `src/armv4t/`,
GBA-specific shortcuts don't go in `src/gba/`, and neither layer
talks to the debug server directly — they emit events, the debug
server reads them.

---

## BIOS as the boot path

`gbarecomp` runs the **real** GBA BIOS. Every recomped game binary
takes both a BIOS file and a ROM file at launch; the runtime loads
the BIOS into the 16 KB region at `0x00000000`, sets the CPU to
SVC mode with PC=0, and hands off to the interpreter. From there:

1. BIOS reset handler runs.
2. BIOS reads the cartridge's Nintendo-logo bytes at
   `0x08000004..0x080000A0`. If they match the BIOS's internal copy,
   boot continues; otherwise the BIOS halts. **We do not bypass
   this check.**
3. BIOS plays the Nintendo logo intro animation (~2 seconds on real
   hardware). Real hardware ticks the PPU, real timers fire, real
   IRQs are taken. **We do not skip this.**
4. BIOS jumps to the cartridge entry point at `0x08000000`. The
   cartridge's `crt0` takes over.
5. Throughout game execution, SWIs trap to BIOS at `0x00000008`
   and IRQs trap to BIOS at `0x00000018`. The interpreter runs those
   bytes just like any other code.

Per `PRINCIPLES.md` "BIOS is sacred," there is no HLE path. The
BIOS lives at `bios/gba_bios.bin` (user-provided, hash-verified;
see `bios/README.md`).

Implications for the architecture:

- The fetch/decode/step loop never cares whether PC is in BIOS or
  ROM; both are just bytes on the bus.
- Static recompilation (the `gba_recompile` tool, currently a stub)
  initially targets cartridge ROM only. BIOS code stays interpreted
  because (a) it's small, (b) it runs briefly per game launch plus
  occasional SWI/IRQ entries, and (c) it lets us be conservative
  about what's lifted to C. Long term we may also recompile the
  BIOS statically — psxrecomp does — but it's not a Phase 1/2 goal.
- The runtime never assumes the cartridge has run "first frame
  setup" itself. The BIOS's first VBlank IRQ runs through the
  cartridge's IRQ handler installed at `0x03007FFC`.

This matches PSXRecomp's pattern (see `../../psxrecomp/psxrecomp/`):
the BIOS is loaded as a separate file, treated as a peer program
in audit tooling, and executed as real code at runtime.

## End-to-end pipeline

```
BIOS + ROM + game.toml + symbols
        |
        v
+-----------------+
| tools/gba_scan   |   header / version / save-chip detection
+--------+--------+
         |
         v
+--------+--------+
| tools/gba_recompile |  block discovery → IR → codegen
+--------+--------+
         |
         v
+--------+--------+
| MinishCapRecomp/generated/*.c |
+--------+--------+
         |
         v
+--------+--------+   +-------------------------+
| game runtime    +---+ src/gba + src/runtime    |
| (links generated)   + src/debug                |
+--------+--------+   +-------------------------+
         |
         v
+--------+--------+        compare        +--------+--------+
|  native build   | <-----  TCP    -----> |  mGBA oracle    |
+-----------------+                        +-----------------+
```

---

## Generated code conventions

- Each ROM function becomes a C function `fn_arm_<hex>` or
  `fn_thumb_<hex>`.
- Each function takes `(CPUState* cpu)`. CPSR, banked regs, and SPSR
  live in `CPUState`.
- Block labels (`L_<hex>:`) appear at every branch target
  identified by discovery.
- Direct calls go through a typed call wrapper that records the call
  ring (when the runtime is built with reverse-debug).
- Indirect calls go through `gba_dispatch_call(cpu, target, mode)`
  which switches on the target's discovered mode.
- Returns either `return;` (for inlined-return idioms) or use the
  generated tail-fallthrough convention.

We follow the same "generated C is evidence, not authority"
philosophy as nesrecomp/snesrecomp/psxrecomp: never hand-edit.

---

## Stubbing and HLE policy

There is no "stub now, fix later" policy. Every implemented
behavior must:

1. Cite a hardware reference (GBATEK section, CowBite page,
   mGBA source pointer, hardware test ROM result).
2. Be testable from a unit test or oracle comparison.
3. Log loudly on any unmodeled input rather than returning a magic
   value.

The exception is **explicit silence with documentation**: if audio
is not yet implemented, the audio module must say so on the TCP
surface and the docs must say what's missing. Silent return of
zero/0xFFFF is forbidden.
