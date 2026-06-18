// heal_gate.cpp — see heal_gate.h. Full-RAM snapshot/restore/diff primitives.

#include "heal_gate.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "gba_bus.h"

namespace gbarecomp::heal_gate {

namespace {

// Region (ptr, size) views, in a stable order matching GateRegion. Mutable and
// const overloads so capture reads and restore writes share one description.
struct RegionView {
    uint8_t* data;
    std::size_t size;
    const char* name;
};

RegionView region_view(gba::GbaBus& bus, GateRegion r) {
    switch (r) {
        case GateRegion::Ewram: return {bus.ewram_ptr(), 256u * 1024u, "EWRAM"};
        case GateRegion::Iwram: return {bus.iwram_ptr(),  32u * 1024u, "IWRAM"};
        case GateRegion::Pal:   return {bus.pal_ptr(),          1024u, "PAL"};
        case GateRegion::Vram:  return {bus.vram_ptr(),   96u * 1024u, "VRAM"};
        case GateRegion::Oam:   return {bus.oam_ptr(),          1024u, "OAM"};
    }
    return {nullptr, 0, "?"};
}

// Map a journal's BusWriteRegion to its backing (ptr, size) for rollback.
void region_ptr(gba::GbaBus& bus, gba::BusWriteRegion r, uint8_t*& p,
                std::size_t& cap) {
    switch (r) {
        case gba::BusWriteRegion::Ewram: p = bus.ewram_ptr(); cap = 256u * 1024u; return;
        case gba::BusWriteRegion::Iwram: p = bus.iwram_ptr(); cap =  32u * 1024u; return;
        case gba::BusWriteRegion::Pal:   p = bus.pal_ptr();   cap =        1024u; return;
        case gba::BusWriteRegion::Vram:  p = bus.vram_ptr();  cap =  96u * 1024u; return;
        case gba::BusWriteRegion::Oam:   p = bus.oam_ptr();   cap =        1024u; return;
        case gba::BusWriteRegion::Device: break;
    }
    p = nullptr;
    cap = 0;
}

std::vector<uint8_t>& snapshot_region(StateSnapshot& s, GateRegion r) {
    switch (r) {
        case GateRegion::Ewram: return s.ewram;
        case GateRegion::Iwram: return s.iwram;
        case GateRegion::Pal:   return s.pal;
        case GateRegion::Vram:  return s.vram;
        case GateRegion::Oam:   return s.oam;
    }
    return s.ewram;  // unreachable
}

const std::vector<uint8_t>& snapshot_region(const StateSnapshot& s, GateRegion r) {
    return snapshot_region(const_cast<StateSnapshot&>(s), r);
}

// Capacity guard so an over-flooding diff stays readable in the heal log.
constexpr std::size_t kMaxNotes = 12;

void note(StateDiff& d, const char* fmt, ...) {
    d.clean = false;
    if (d.notes.size() >= kMaxNotes) return;
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    d.notes.emplace_back(buf);
}

}  // namespace

StateSnapshot capture_full(const ArmCpuState& cpu, uint64_t cycles,
                           const gba::GbaBus& bus) {
    StateSnapshot s;
    s.cpu = cpu;
    s.cycles = cycles;
    // const_cast is safe: region_view only reads here (the pointers are the same
    // backing arrays the const accessors expose).
    auto& mbus = const_cast<gba::GbaBus&>(bus);
    for (int i = 0; i < kGateRegionCount; ++i) {
        const auto r = static_cast<GateRegion>(i);
        RegionView v = region_view(mbus, r);
        snapshot_region(s, r).assign(v.data, v.data + v.size);
    }
    return s;
}

void restore_full(const StateSnapshot& s, ArmCpuState& cpu, uint64_t& cycles,
                  gba::GbaBus& bus) {
    cpu = s.cpu;
    cycles = s.cycles;
    for (int i = 0; i < kGateRegionCount; ++i) {
        const auto r = static_cast<GateRegion>(i);
        RegionView v = region_view(bus, r);
        const auto& src = snapshot_region(s, r);
        if (src.size() == v.size) std::memcpy(v.data, src.data(), v.size);
    }
}

StateDiff diff_full(const StateSnapshot& a, const StateSnapshot& b) {
    StateDiff d;

    for (int r = 0; r < 16; ++r) {
        if (a.cpu.R[r] != b.cpu.R[r])
            note(d, "R[%d]: a=0x%08X b=0x%08X", r, a.cpu.R[r], b.cpu.R[r]);
    }
    if (a.cpu.cpsr != b.cpu.cpsr)
        note(d, "CPSR: a=0x%08X b=0x%08X", a.cpu.cpsr, b.cpu.cpsr);

    // Banked state (a function may switch mode). Compare the whole tail of the
    // struct after cpsr in one shot; report generically since it is rare.
    if (std::memcmp(a.cpu.banked_sp, b.cpu.banked_sp, sizeof(a.cpu.banked_sp)) ||
        std::memcmp(a.cpu.banked_lr, b.cpu.banked_lr, sizeof(a.cpu.banked_lr)) ||
        std::memcmp(a.cpu.banked_spsr, b.cpu.banked_spsr, sizeof(a.cpu.banked_spsr)) ||
        std::memcmp(a.cpu.r8_12_user, b.cpu.r8_12_user, sizeof(a.cpu.r8_12_user)) ||
        std::memcmp(a.cpu.r8_12_fiq, b.cpu.r8_12_fiq, sizeof(a.cpu.r8_12_fiq)))
        note(d, "banked register state differs");

    if (a.cycles != b.cycles)
        note(d, "cycles: a=%llu b=%llu", (unsigned long long)a.cycles,
             (unsigned long long)b.cycles);

    static const GateRegion kRegions[kGateRegionCount] = {
        GateRegion::Ewram, GateRegion::Iwram, GateRegion::Pal,
        GateRegion::Vram, GateRegion::Oam};
    static const char* kNames[kGateRegionCount] = {"EWRAM", "IWRAM", "PAL",
                                                   "VRAM", "OAM"};
    for (int i = 0; i < kGateRegionCount; ++i) {
        const auto& ra = snapshot_region(a, kRegions[i]);
        const auto& rb = snapshot_region(b, kRegions[i]);
        if (ra.size() != rb.size()) {
            note(d, "%s: size mismatch %zu vs %zu", kNames[i], ra.size(),
                 rb.size());
            continue;
        }
        for (std::size_t off = 0; off < ra.size(); ++off) {
            if (ra[off] != rb[off]) {
                note(d, "%s[0x%X]: a=0x%02X b=0x%02X", kNames[i],
                     static_cast<unsigned>(off), ra[off], rb[off]);
                break;  // first divergence per region is enough to flag it
            }
        }
    }
    return d;
}

// ── Journal ────────────────────────────────────────────────────────────────

void Journal::arm(gba::GbaBus& bus, Mode mode) {
    clear();
    mode_ = mode;
    bus.set_write_observer(this);
}

void Journal::disarm(gba::GbaBus& bus) { bus.set_write_observer(nullptr); }

void Journal::clear() {
    ram_writes_.clear();
    io_touched_ = false;
    io_first_addr_ = 0;
}

bool Journal::on_bus_write(gba::BusWriteRegion region, uint32_t off,
                           uint32_t addr, uint8_t width, uint32_t old_value,
                           uint32_t new_value) {
    if (region == gba::BusWriteRegion::Device) {
        if (!io_touched_) {
            io_touched_ = true;
            io_first_addr_ = addr;
        }
        // RECORD (interp pass) applies device writes — they belong to the kept
        // result. SHADOW (shard validation) traps them so a mis-compile can't
        // fire a real IO/DMA side effect; the trap is itself a divergence signal.
        return mode_ == Mode::Shadow;
    }
    ram_writes_.push_back({region, width, off, old_value, new_value});
    return false;  // RAM writes always apply
}

void Journal::rollback(gba::GbaBus& bus) const {
    // Reverse order so overlapping writes restore to the earliest old value.
    for (auto it = ram_writes_.rbegin(); it != ram_writes_.rend(); ++it) {
        uint8_t* p = nullptr;
        std::size_t cap = 0;
        region_ptr(bus, it->region, p, cap);
        if (!p || it->off + it->width > cap) continue;
        const uint32_t v = it->old_value;
        p[it->off] = static_cast<uint8_t>(v);
        if (it->width >= 2) p[it->off + 1] = static_cast<uint8_t>(v >> 8);
        if (it->width >= 4) {
            p[it->off + 2] = static_cast<uint8_t>(v >> 16);
            p[it->off + 3] = static_cast<uint8_t>(v >> 24);
        }
    }
}

}  // namespace gbarecomp::heal_gate
