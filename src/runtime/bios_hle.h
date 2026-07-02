// bios_hle.h — opt-in High-Level Emulation of GBA BIOS SWI calls.
//
// The recompiled real BIOS (LLE) is this project's default and remains the
// correctness oracle (PRINCIPLES.md "BIOS is sacred — recompiled and
// dispatched"). HLE is an *alternative*, selected per-game via the config
// (`[bios].hle = true`), the `--bios-hle` CLI flag, or the GBARECOMP_BIOS_HLE
// env var. It is the PRINCIPLES.md "verified-enhancement HLE" carve-out: opt-in,
// LLE stays load-bearing, and any SWI the HLE layer does not implement
// transparently falls through to the recompiled BIOS (so HLE never breaks a
// game and is never load-bearing beyond what it covers).
//
// Mechanism: bios_hle_set_mode() installs (or clears) the runtime_swi hook
// g_bios_hle_hook (declared in runtime_arm.h). With HLE on, runtime_swi calls
// bios_hle_dispatch() BEFORE the SVC-mode exception entry; a handled SWI resumes
// the caller at LR with no BIOS dispatch. Default = Off = pure LLE, byte for
// byte identical to a build compiled without any HLE.
//
// The SWI semantics here are ported from mGBA's src/gba/bios.c (MPL-2.0,
// © Jeffrey Pfau) and GBATEK; see THIRD_PARTY_ATTRIBUTION.md.

#pragma once

#include <cstdint>

namespace gba {

enum class BiosHleMode {
    Off,   // pure LLE — recompiled BIOS services every SWI (default)
    On,    // HLE where implemented, LLE fallback otherwise
};

// Install or clear the runtime_swi HLE hook. Off clears g_bios_hle_hook back to
// nullptr (pure LLE). Safe to call once at runtime bring-up.
void bios_hle_set_mode(BiosHleMode mode);

// Current mode (for the startup banner / diagnostics).
BiosHleMode bios_hle_mode();

// Human-readable label for the banner.
const char* bios_hle_mode_name(BiosHleMode mode);

// Boot HLE — skip the BIOS intro. Instead of running the recompiled boot ROM
// (logo + chime) from reset, synthesize the machine state the real BIOS leaves
// at cart handoff (per-mode stack pointers, System mode, cleared registers, the
// zeroed user-IRQ-handler pointer at 0x03007FFC) and set PC to `cart_entry`
// (normally 0x08000000). The recompiled BIOS stays linked and still services
// IRQs (vector 0x18) and any SWIs HLE does not cover — only the boot ANIMATION
// is skipped. Call once, on a fresh boot (never when resuming a savestate),
// AFTER reset_recomp_cpu(). LLE (the default) always plays the real intro.
void bios_hle_boot_skip(uint32_t cart_entry);

}  // namespace gba
