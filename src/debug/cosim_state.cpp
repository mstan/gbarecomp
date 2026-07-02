// cosim_state.cpp — full guest-architectural-state canonical hash. See
// COSIM_ORACLE.md and cosim_state.h. Active ONLY in the GBA_COSIM build; the whole
// translation unit is empty otherwise, so it is safe in the shared source list.
//
// Principle: hash exactly the state a real GBA exposes to the running program plus
// the device-timing quantities that determine WHEN a guest-visible bit flips —
// never our emulator's host-only bookkeeping. This list is refined by the
// injected-divergence and recomp-vs-recomp gates.
//
// v1 note: memories are hashed DIRECTLY every checkpoint (no incremental
// page-hash). That is more robust (no dirty-tracking bugs) and cheap enough for an
// oracle at coarse stride; incremental page-hash-on-write is a future perf item if
// fine-stride drilling needs it.
#ifdef GBA_COSIM

#include "cosim_state.h"

#include "gba_bus.h"
#include "gba_ppu.h"
#include "runtime_arm.h"            // g_cpu (ArmCpuState), g_runtime_cycles
#include "runtime_bus_bridge.h"     // gbarecomp::active_bus() / active_ppu()
#include "snapshot.h"               // gbarecomp::debug::SnapshotWriter

#include <cstddef>
#include <cstdint>

namespace gbarecomp::cosim {
namespace {

constexpr uint64_t kFnvOff = 1469598103934665603ull;
constexpr uint64_t kFnvPrm = 1099511628211ull;

inline uint64_t fnv(uint64_t h, const void* p, std::size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    for (std::size_t i = 0; i < n; ++i) { h ^= b[i]; h *= kFnvPrm; }
    return h;
}
inline uint64_t fnv_u32(uint64_t h, uint32_t v) { return fnv(h, &v, 4); }
inline uint64_t fnv_u64(uint64_t h, uint64_t v) { return fnv(h, &v, 8); }

// Hash a subsystem's canonical serialize() output as an opaque blob. Reuses the
// version-locked save-state serializers (audited in COSIM_ORACLE.md §2).
template <class Fn>
uint64_t hash_serialized(Fn&& serialize_into) {
    gbarecomp::debug::SnapshotWriter w;
    serialize_into(w);
    const auto& buf = w.buffer();
    return fnv(kFnvOff, buf.data(), buf.size());
}

// CPU hash — R[0..14] (PC/R[15] EXCLUDED, see header), cpsr, and the full banked
// register file. No host-only fields exist in ArmCpuState (it is a flat POD ABI
// contract), so the whole struct minus PC is guest-architectural.
uint64_t hash_cpu(const ArmCpuState& c) {
    uint64_t h = kFnvOff;
    h = fnv(h, c.R, sizeof(uint32_t) * 15);  // R[0..14]; EXCLUDE R[15]
    h = fnv_u32(h, c.cpsr);
    h = fnv(h, c.banked_sp,   sizeof c.banked_sp);
    h = fnv(h, c.banked_lr,   sizeof c.banked_lr);
    h = fnv(h, c.banked_spsr, sizeof c.banked_spsr);
    h = fnv(h, c.r8_12_user,  sizeof c.r8_12_user);
    h = fnv(h, c.r8_12_fiq,   sizeof c.r8_12_fiq);
    return h;
}

// ── gate-4 injection (applied to LIVE state so it flows into the hash naturally) ──
int      s_inj_region = -1;   // 0 = IWRAM, 1 = EWRAM
uint32_t s_inj_off    = 0;
uint8_t  s_inj_xor    = 0;
int      s_inj_reg    = -1;   // 0..14 = R[n], 16 = cpsr
uint32_t s_inj_reg_xor = 0;

}  // namespace

void state_reset() {
    s_inj_region = -1;
    s_inj_reg    = -1;
}

void inject_ram(int region, uint32_t off, uint8_t xor_val) {
    s_inj_region = region; s_inj_off = off; s_inj_xor = xor_val;
}
void inject_reg(int reg_index, uint32_t xor_val) {
    s_inj_reg = reg_index; s_inj_reg_xor = xor_val;
}

uint64_t state_hash(SubHashes* sub) {
    SubHashes s;
    gba::GbaBus* bus = gbarecomp::active_bus();
    gba::GbaPpu* ppu = gbarecomp::active_ppu();

    // Apply any pending gate-4 injection to the live machine so it folds into the
    // hash exactly like a real divergence would.
    if (s_inj_region == 0 && bus && s_inj_off < 32u * 1024u) {
        bus->iwram_ptr()[s_inj_off] ^= s_inj_xor;
    } else if (s_inj_region == 1 && bus && s_inj_off < 256u * 1024u) {
        bus->ewram_ptr()[s_inj_off] ^= s_inj_xor;
    }
    s_inj_region = -1;
    if (s_inj_reg >= 0) {
        if (s_inj_reg < 15) g_cpu.R[s_inj_reg] ^= s_inj_reg_xor;
        else if (s_inj_reg == 16) g_cpu.cpsr ^= s_inj_reg_xor;
        s_inj_reg = -1;
    }

    s.cpu = hash_cpu(g_cpu);

    if (bus) {
        s.iwram = fnv(kFnvOff, bus->iwram_ptr(), 32u * 1024u);
        s.ewram = fnv(kFnvOff, bus->ewram_ptr(), 256u * 1024u);
        s.vram  = fnv(kFnvOff, bus->vram_ptr(), 96u * 1024u);
        s.pal   = fnv(kFnvOff, bus->pal_ptr(), 1024u);
        s.oam   = fnv(kFnvOff, bus->oam_ptr(), 1024u);
        s.io    = hash_serialized([&](gbarecomp::debug::SnapshotWriter& w) { bus->io().serialize(w); });
        s.audio = hash_serialized([&](gbarecomp::debug::SnapshotWriter& w) { bus->audio().serialize(w); });
        s.save  = hash_serialized([&](gbarecomp::debug::SnapshotWriter& w) { bus->save().serialize(w); });
        s.prefetch = fnv_u32(kFnvOff, bus->bios_open_bus());
    }
    if (ppu) {
        s.ppu = hash_serialized([&](gbarecomp::debug::SnapshotWriter& w) { ppu->serialize(w); });
    }
    s.clock = fnv_u64(kFnvOff, static_cast<uint64_t>(g_runtime_cycles));

    if (sub) *sub = s;

    // Fold every sub-hash into the top hash.
    return fnv(kFnvOff, &s, sizeof s);
}

}  // namespace gbarecomp::cosim

#endif  // GBA_COSIM
