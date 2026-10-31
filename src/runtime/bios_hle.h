// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.
// Full text: third_party/MPL-2.0.txt
//
// bios_hle.h — opt-in High-Level Emulation of GBA BIOS SWI calls.
//
// The recompiled real BIOS (LLE) is this project's default and remains the
// correctness oracle (PRINCIPLES.md "BIOS is sacred — recompiled and
// dispatched"). HLE is an *alternative*, selected per-game via the config
// (`[bios].hle = true`), the `--bios-hle` CLI flag, or the GBARECOMP_BIOS_HLE
// env var. GBARECOMP_BIOS_MATH_IMPLEMENTATION independently selects the complete
// Div/DivArm/ArcTan2 operation family at build time; it does not enable this
// full legacy catalog, alter boot presentation, or add a live math switch.
// These are existing legacy selectors, not the build-time architecture
// required for new HLE services in HLE.md. Implemented SWIs replace BIOS work;
// unimplemented SWIs follow the original route. This mechanism alone supplies
// no blanket compatibility, timing or netplay qualification guarantee.
//
// Mechanism: bios_hle_set_mode() installs (or clears) the runtime_swi hook
// g_bios_hle_hook (declared in runtime_arm.h). With HLE on, runtime_swi calls
// bios_hle_dispatch() BEFORE the SVC-mode exception entry; a handled SWI resumes
// the caller at LR with no BIOS dispatch. With the default LLE math build,
// catalog Off clears the hook. A native math build keeps only its selected
// math operations active when the legacy catalog is Off.
//
// The SWI semantics here are ported from mGBA's src/gba/bios.c (MPL-2.0,
// © Jeffrey Pfau) and GBATEK; see THIRD_PARTY_ATTRIBUTION.md.

#pragma once

#include <cstdint>

namespace gba {

enum class BiosHleMode {
    Off,   // legacy catalog off; independently built native families remain
    On,    // HLE where implemented, LLE fallback otherwise
};

// Install the catalog or the fixed build-selected family. Safe to call once at
// runtime bring-up. There is no player-facing native-math runtime selector.
void bios_hle_set_mode(BiosHleMode mode);

// Fixed build identity, independent of the grandfathered catalog mode.
bool bios_native_math_enabled();

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
