// arm_cpu_bridge.cpp — see arm_cpu_bridge.h.

#include "arm_cpu_bridge.h"

namespace gbarecomp {

void load_arm_cpu_into_interp(const ArmCpuState& g, armv4t::CPUState& cpu) {
    for (int i = 0; i < 16; ++i) cpu.R[i] = g.R[i];

    cpu.cpsr.n    = (g.cpsr >> 31) & 1u;
    cpu.cpsr.z    = (g.cpsr >> 30) & 1u;
    cpu.cpsr.c    = (g.cpsr >> 29) & 1u;
    cpu.cpsr.v    = (g.cpsr >> 28) & 1u;
    cpu.cpsr.i    = (g.cpsr >>  7) & 1u;
    cpu.cpsr.f    = (g.cpsr >>  6) & 1u;
    cpu.cpsr.t    = (g.cpsr >>  5) & 1u;
    cpu.cpsr.mode = g.cpsr & 0x1Fu;

    for (int i = 0; i < armv4t::Bank_Count; ++i) {
        cpu.banked_sp[i]   = g.banked_sp[i];
        cpu.banked_lr[i]   = g.banked_lr[i];
        cpu.banked_spsr[i] = g.banked_spsr[i];
    }
    for (int i = 0; i < 5; ++i) {
        cpu.r8_12_user[i] = g.r8_12_user[i];
        cpu.r8_12_fiq[i]  = g.r8_12_fiq[i];
    }
    cpu.thumb = cpu.cpsr.t;
}

void store_interp_into_arm_cpu(const armv4t::CPUState& cpu, ArmCpuState& g) {
    for (int i = 0; i < 16; ++i) g.R[i] = cpu.R[i];

    uint32_t v = 0;
    if (cpu.cpsr.n) v |= 1u << 31;
    if (cpu.cpsr.z) v |= 1u << 30;
    if (cpu.cpsr.c) v |= 1u << 29;
    if (cpu.cpsr.v) v |= 1u << 28;
    if (cpu.cpsr.i) v |= 1u <<  7;
    if (cpu.cpsr.f) v |= 1u <<  6;
    if (cpu.cpsr.t) v |= 1u <<  5;
    v |= static_cast<uint32_t>(cpu.cpsr.mode) & 0x1Fu;
    g.cpsr = v;

    for (int i = 0; i < armv4t::Bank_Count; ++i) {
        g.banked_sp[i]   = cpu.banked_sp[i];
        g.banked_lr[i]   = cpu.banked_lr[i];
        g.banked_spsr[i] = cpu.banked_spsr[i];
    }
    for (int i = 0; i < 5; ++i) {
        g.r8_12_user[i] = cpu.r8_12_user[i];
        g.r8_12_fiq[i]  = cpu.r8_12_fiq[i];
    }
}

}  // namespace gbarecomp
