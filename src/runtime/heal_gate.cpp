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

}  // namespace gbarecomp::heal_gate
