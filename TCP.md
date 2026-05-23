# TCP.md — Debug Server Protocol

The TCP debug server is the only sanctioned debugging interface for
gbarecomp games. If a piece of state isn't observable over TCP,
extend `src/debug/tcp_debug_server.cpp` — do not work around it with
`printf`.

Two surfaces cohabit:

1. **Always-on surface** — ring-buffered frame snapshots + live
   hardware query commands. Active whenever `debug.ini` is present
   (or the `--verify` / `--oracle` CLI flags are set), including
   Release builds.
2. **Reverse debugger (`rdb_*`)** — synchronous hooks emitted by the
   recompiler when `gba_recompile --reverse-debug` regenerates the C
   and the runtime is built with `-DGBARECOMP_REVERSE_DEBUG=ON`.
   Gives per-store / per-block / per-call attribution, breakpoints,
   watchpoints, and IWRAM/EWRAM reconstruction. Zero cost when off.

---

# PORTS

Default: `127.0.0.1:19842` (native).
Oracle (mGBA bridge) defaults to `19843`.
Configurable per project via `debug.ini`.

Convention: native odd port + 1 = oracle port. Same as in nesrecomp
and snesrecomp.

---

# TRANSPORT

- TCP localhost.
- Line-based: one command per line, terminated by `\n`.
- JSON request preferred:
  `{"cmd":"read_io","addr":"0x04000000","len":2,"id":7}`.
- Bare request accepted for the simplest commands: `ping\n`.
- Single-line JSON response: `{"ok":true,...}` or
  `{"ok":false,"error":"..."}`.
- `id` echoed when supplied.
- Max command line: 8192 bytes.
- One client at a time.

---

# RING BUFFER (CRITICAL)

The always-on surface records a full state snapshot **every frame**
into a ring buffer (default ~36,000 entries, ~10 minutes at 60 fps).

Each frame record contains, at minimum:

- CPU: R0..R15 in current mode, CPSR (with exploded flags N/Z/C/V/I/F
  /T/mode), banked R13/R14 for User/SVC/IRQ/FIQ/ABT/UND, SPSR_irq,
  SPSR_svc, SPSR_fiq.
- Current ARM/THUMB flag (CPSR.T).
- IO snapshot: defined `0x04000000` IO range.
- PPU: DISPCNT, DISPSTAT, VCOUNT, BGxCNT/HOFS/VOFS, affine params,
  WIN0H/V/WIN1H/V/WININ/WINOUT, MOSAIC, BLDCNT/BLDALPHA/BLDY.
- DMA channels 0..3: SAD/DAD/CNT/state.
- Timers 0..3: counter, reload, CNT, active.
- IRQ: IE, IF, IME.
- Sound: SOUNDCNT_L/H/X, FIFO levels, channel state, DMA1/DMA2 audio
  state.
- Save chip: detected type, mode/state.
- Full IWRAM (32 KB), PAL (1 KB), OAM (1 KB), VRAM (96 KB).
- EWRAM (256 KB): sampled in anchor snapshots, not every frame.
- Last entered function name + PC + ARM/THUMB mode.
- Last SWI taken; last DMA fired; last IRQ taken.
- Up to N verify-mode diffs vs oracle for this frame.
- Game-specific hook (32 bytes filled by the game's hook).

All retroactive inspection commands read from this buffer. If you
cannot answer a question from live state, query the history. If the
question requires data not in the ring, extend the ring.

The `rdb_*` surface adds finer-grained history: a per-store ring
covering armed address ranges, a call ring, a block ring, and
periodic IWRAM/EWRAM anchors that let you reconstruct memory at any
past block index.

---

# ALWAYS-ON COMMANDS

## Monitoring & inspection

```
ping                    frame
get_registers           get_cpsr              get_banked_regs
read_iwram              read_ewram            read_vram             read_pal      read_oam
read_io                 io_state              read_rom
read_save               save_state            scheduler_state
ppu_state               dma_state             timer_state           irq_state
audio_state             screenshot
```

## Ring-buffer queries

```
history                 get_frame             frame_range          frame_timeseries
read_frame_iwram        read_frame_ewram      read_frame_vram      read_frame_io
restore_frame
```

## Execution control

```
pause                   continue              step                 run_to_frame
run_to_pc               run_to_vblank         run_to_swi
set_input               press                 clear_input          quit
```

## Comparison & verification

```
frame_diff              memory_diff           first_failure        framebuf_diff
io_diff                 ppu_diff              dma_diff             timer_diff
irq_diff
```

## Diagnostics

```
call_stack              (requires runtime stack tracking)
dispatch_miss_info      (also written to dispatch_misses.log)
watchdog_status         unmapped_io_log
unknown_swi_log
```

---

# REVERSE DEBUGGER COMMANDS (rdb_*)

Gated on `GBARECOMP_REVERSE_DEBUG=1` at both generator and runtime
build time. All commands return `{"ok":true,...}` or
`{"ok":false,"error":"..."}`.

## Tier 1 — Synchronous bus-write ring

Records every store to armed address ranges with
`(block_idx, frame, addr, val, pc, func, mode)`. 1 M entries.

```
rdb_status                          unified view of every rdb_* subsystem
rdb_range     {lo, hi}              arm an address-range filter (up to 8)
rdb_range_clear                     drop all armed ranges
rdb_reset                           clear the store ring
rdb_count                           entries / write_idx
rdb_dump      {start, max}          dump entries as JSON
```

## Tier 1.5 — Call ring

Every function prologue logs `(frame, func, caller, mode)`. Caller is
read from the ARM/THUMB return stack — best-effort.

```
trace_calls                         arm
trace_calls_reset                   reset ring
get_call_trace {from, to, max}      dump filtered by func range
```

## Tier 2 — Block-level trace

Every basic-block entry records
`(frame, pc, func, r0..r15, cpsr, mode)`.

```
trace_blocks                        arm
trace_blocks_reset                  reset ring
trace_blocks_range {lo, hi}         restrict to a PC range (up to 8)
get_block_trace {from, to, max}     dump
```

## Tier 2.5 — Breakpoints & watchpoints

```
rdb_break        {pc[, mode]}       set a block-PC breakpoint (16 slots)
rdb_break_clear                     clear all
rdb_break_list                      list
rdb_step_block                      one-shot break on next block
rdb_break_continue                  release

rdb_watch_add    {addr[, val]}      watchpoint on any memory region, optional value match
rdb_watch_clear                     clear all
rdb_watch_list                      list
rdb_watch_continue                  release

rdb_parked                          unified park report
```

## Tier 3 — Memory anchors & reconstruction

```
rdb_anchor_on     {interval, region}    snapshot every N blocks (64 slots).
rdb_anchor_off                          stop snapshotting (existing anchors kept)
rdb_anchor_status                       active / count / interval / current block_idx
rdb_iwram_at_block {block}              reconstruct IWRAM at block
rdb_ewram_at_block {block}              reconstruct EWRAM at block
```

---

# DISPATCH MISSES

Logged to `dispatch_misses.log` next to the executable. This file is
the PRIMARY source — check it after every game run.
`dispatch_miss_info` via TCP returns the same data live.

A dispatch miss means `call_by_address(addr, mode)` found no
generated function. The game skips that subroutine. This is a
SILENT GAME-BREAKING BUG.

Resolution: add entries to `[functions]` in `game.toml`, regenerate,
rebuild.

---

# UNKNOWN IO / UNKNOWN SWI

Both are observable:

- `unmapped_io_log` — every unmapped or partial-decode IO access
  (read or write), with PC, mode, value.
- `unknown_swi_log` — every SWI number we executed that wasn't
  modeled, with caller PC + register state.

Neither is allowed to be silenced. If something gets noisy, decide
deliberately: implement it, or document why it's safe and add it to
an allowlist that is itself logged at startup.

---

# ORACLE COMMANDS (requires GBARECOMP_MGBA_ORACLE)

```
emu_registers           emu_screenshot        framebuf_diff
read_emu_iwram          read_emu_ewram        read_emu_vram         read_emu_pal
read_emu_oam            emu_ppu_state         emu_dma_state         emu_timer_state
emu_irq_state           emu_swi_count         emu_vblank_count
emu_step                emu_step_to_vblank    emu_step_to_swi
```

---

# ADDING A NEW COMMAND

1. Add a handler in `src/debug/tcp_debug_server.cpp` — or, for
   `rdb_*`, in `src/debug/reverse_debug.cpp`.
2. Register it in the dispatch table.
3. Mirror on the oracle side if it inspects emulator-internal state.
4. Document it in this file under the right section.
5. Rebuild the runtime.
6. **Never** add a side-channel debug log. If TCP can't see it, TCP
   needs to grow until it can.
