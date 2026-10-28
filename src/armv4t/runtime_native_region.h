// Portable local register/flag operations for bounded native regions.
// Initial operations are copied verbatim from the maintained ABI helpers;
// mixed-region differential checks use the original ARM interpreter.
#pragma once
#include "runtime_arm_types.h"
#include "runtime_wait_model.h"
#include <cstdint>

struct RuntimeNativeCpu {
    uint32_t R[16]{};
    uint32_t cpsr = 0;
    uint32_t cpsr_n() const { return (cpsr & CPSR_N_BIT) != 0; }
    uint32_t cpsr_z() const { return (cpsr & CPSR_Z_BIT) != 0; }
    uint32_t cpsr_c() const { return (cpsr & CPSR_C_BIT) != 0; }
    uint32_t cpsr_v() const { return (cpsr & CPSR_V_BIT) != 0; }
    int arm_cond_passes(unsigned cond) {
        // The condition codes are 4 bits. AL / NV are the unconditional
        // bands.
        const uint32_t n = cpsr_n();
        const uint32_t z = cpsr_z();
        const uint32_t c = cpsr_c();
        const uint32_t v = cpsr_v();
        switch (cond & 0xFu) {
            case 0x0: return z != 0;                            // EQ
            case 0x1: return z == 0;                            // NE
            case 0x2: return c != 0;                            // CS/HS
            case 0x3: return c == 0;                            // CC/LO
            case 0x4: return n != 0;                            // MI
            case 0x5: return n == 0;                            // PL
            case 0x6: return v != 0;                            // VS
            case 0x7: return v == 0;                            // VC
            case 0x8: return (c != 0) && (z == 0);              // HI
            case 0x9: return (c == 0) || (z != 0);              // LS
            case 0xA: return n == v;                            // GE
            case 0xB: return n != v;                            // LT
            case 0xC: return (z == 0) && (n == v);              // GT
            case 0xD: return (z != 0) || (n != v);              // LE
            case 0xE: return 1;                                 // AL
            case 0xF: return 0;                                 // NV (ARMv4T: never)
            default:  return 0;
        }
    }

    uint32_t arm_shift_lsl(uint32_t v, uint32_t n, int set_carry) {
        if (n == 0) return v;  // ARM ARM A5.1.5: shift by 0 → no carry update
        if (n >= 32) {
            if (set_carry) {
                uint32_t carry = (n == 32) ? (v & 1u) : 0u;
                cpsr = (cpsr & ~CPSR_C_BIT) | (carry ? CPSR_C_BIT : 0u);
            }
            return 0;
        }
        if (set_carry) {
            uint32_t carry = (v >> (32u - n)) & 1u;
            cpsr = (cpsr & ~CPSR_C_BIT) | (carry ? CPSR_C_BIT : 0u);
        }
        return v << n;
    }

    uint32_t arm_shift_lsr(uint32_t v, uint32_t n, int set_carry) {
        if (n == 0) return v;
        if (n >= 32) {
            if (set_carry) {
                uint32_t carry = (n == 32) ? ((v >> 31) & 1u) : 0u;
                cpsr = (cpsr & ~CPSR_C_BIT) | (carry ? CPSR_C_BIT : 0u);
            }
            return 0;
        }
        if (set_carry) {
            uint32_t carry = (v >> (n - 1u)) & 1u;
            cpsr = (cpsr & ~CPSR_C_BIT) | (carry ? CPSR_C_BIT : 0u);
        }
        return v >> n;
    }

    uint32_t arm_shift_asr(uint32_t v, uint32_t n, int set_carry) {
        if (n == 0) return v;
        if (n >= 32) {
            uint32_t carry = (v >> 31) & 1u;
            if (set_carry) {
                cpsr = (cpsr & ~CPSR_C_BIT) | (carry ? CPSR_C_BIT : 0u);
            }
            return carry ? 0xFFFFFFFFu : 0u;
        }
        if (set_carry) {
            uint32_t carry = (v >> (n - 1u)) & 1u;
            cpsr = (cpsr & ~CPSR_C_BIT) | (carry ? CPSR_C_BIT : 0u);
        }
        return static_cast<uint32_t>(static_cast<int32_t>(v) >> n);
    }

    uint32_t arm_shift_ror(uint32_t v, uint32_t n, int set_carry) {
        if (n == 0) return v;
        n &= 31u;
        if (n == 0) {  // ROR by multiple of 32 → no change but use top bit as carry
            if (set_carry) {
                cpsr = (cpsr & ~CPSR_C_BIT) |
                    ((v & 0x80000000u) ? CPSR_C_BIT : 0u);
            }
            return v;
        }
        uint32_t r = (v >> n) | (v << (32u - n));
        if (set_carry) {
            cpsr = (cpsr & ~CPSR_C_BIT) |
                ((r & 0x80000000u) ? CPSR_C_BIT : 0u);
        }
        return r;
    }

    void arm_set_nz(uint32_t r) {
        uint32_t c = cpsr & ~(CPSR_N_BIT | CPSR_Z_BIT);
        if (r & 0x80000000u) c |= CPSR_N_BIT;
        if (r == 0)          c |= CPSR_Z_BIT;
        cpsr = c;
    }

    void arm_set_nzc_logic(uint32_t r, uint32_t shifter_carry) {
        // shifter_carry is the carry-out from the operand2 shifter, or
        // the existing CPSR.C if the shift didn't produce one (encoded
        // by the codegen as `cpsr_c()`).
        uint32_t c = cpsr & ~(CPSR_N_BIT | CPSR_Z_BIT | CPSR_C_BIT);
        if (r & 0x80000000u) c |= CPSR_N_BIT;
        if (r == 0)          c |= CPSR_Z_BIT;
        if (shifter_carry)   c |= CPSR_C_BIT;
        cpsr = c;
    }

    void arm_set_nzcv_add(uint32_t a, uint32_t b, uint32_t r) {
        uint32_t c = cpsr & ~(CPSR_N_BIT | CPSR_Z_BIT |
                                      CPSR_C_BIT | CPSR_V_BIT);
        if (r & 0x80000000u) c |= CPSR_N_BIT;
        if (r == 0)          c |= CPSR_Z_BIT;
        // Carry: unsigned overflow.
        if (r < a)           c |= CPSR_C_BIT;
        // Overflow: same-sign inputs, different-sign result.
        if ((~(a ^ b) & (a ^ r)) & 0x80000000u) c |= CPSR_V_BIT;
        cpsr = c;
    }

    void arm_set_nzcv_adc(uint32_t a, uint32_t b, uint32_t c_in,
                                       uint32_t r) {
        uint32_t c = cpsr & ~(CPSR_N_BIT | CPSR_Z_BIT |
                                      CPSR_C_BIT | CPSR_V_BIT);
        if (r & 0x80000000u) c |= CPSR_N_BIT;
        if (r == 0)          c |= CPSR_Z_BIT;
        // Carry from full 33-bit addition.
        uint64_t wide = static_cast<uint64_t>(a) + b + c_in;
        if (wide >> 32)      c |= CPSR_C_BIT;
        if ((~(a ^ b) & (a ^ r)) & 0x80000000u) c |= CPSR_V_BIT;
        cpsr = c;
    }

    void arm_set_nzcv_sub(uint32_t a, uint32_t b, uint32_t r) {
        uint32_t c = cpsr & ~(CPSR_N_BIT | CPSR_Z_BIT |
                                      CPSR_C_BIT | CPSR_V_BIT);
        if (r & 0x80000000u) c |= CPSR_N_BIT;
        if (r == 0)          c |= CPSR_Z_BIT;
        // Carry: NOT borrow. C=1 if a >= b.
        if (a >= b)          c |= CPSR_C_BIT;
        if (((a ^ b) & (a ^ r)) & 0x80000000u) c |= CPSR_V_BIT;
        cpsr = c;
    }

    void arm_set_nzcv_sbc(uint32_t a, uint32_t b, uint32_t c_in,
                                       uint32_t r) {
        uint32_t c = cpsr & ~(CPSR_N_BIT | CPSR_Z_BIT |
                                      CPSR_C_BIT | CPSR_V_BIT);
        if (r & 0x80000000u) c |= CPSR_N_BIT;
        if (r == 0)          c |= CPSR_Z_BIT;
        // SBC: r = a - b - (1 - c_in). Carry-out = NOT borrow.
        // The 33-bit equivalent: a + ~b + c_in. Overflow if top bit.
        uint64_t wide = static_cast<uint64_t>(a) + (~b & 0xFFFFFFFFu) + c_in;
        if (wide >> 32)      c |= CPSR_C_BIT;
        if (((a ^ b) & (a ^ r)) & 0x80000000u) c |= CPSR_V_BIT;
        cpsr = c;
    }
};

struct RuntimeNativeRegion;
extern "C" bool runtime_native_region_begin(RuntimeNativeRegion*, uint32_t instructions,
                                             uint32_t code_pc, uint32_t thumb);
extern "C" void runtime_native_region_end(RuntimeNativeRegion*);

// Borrowed RAM and timing state last only until commit. The runtime admits a
// region strictly before its next event, with every active observer excluded.
// Unsupported memory commits the completed prefix and resumes the maintained
// body AT the unexecuted instruction; it neither replays stores nor rolls back.
struct RuntimeNativeRegion {
    RuntimeNativeCpu cpu;
    RuntimeWaitTable waits{};
    uint8_t* ewram = nullptr;
    uint8_t* iwram = nullptr;
    uint32_t cycles = 0;
    uint32_t writes = 0;
    uint32_t bound = 0;
    uint32_t instructions = 0;
    static constexpr unsigned insn_trace = 0;
    static constexpr bool runtime_should_yield() { return false; }
    static constexpr bool runtime_trace_enabled() { return false; }
    static void runtime_insn_fp() {}
    bool ram_range(uint32_t address, uint32_t bytes) const {
        const uint32_t region = address >> 24;
        if (region != 2 && region != 3) return false;
        const uint32_t mask = region == 2 ? 0x3ffffu : 0x7fffu;
        return (address & mask) <= mask + 1u - bytes;
    }
    uint8_t* ram(uint32_t address) const {
        return (address >> 24) == 2 ? ewram + (address & 0x3ffffu)
                                   : iwram + (address & 0x7fffu);
    }
    uint32_t bus_read_u8(uint32_t address) const { return *ram(address); }
    uint32_t bus_read_u16(uint32_t address) const {
        const auto* p = ram(address);
        return uint32_t(p[0]) | (uint32_t(p[1]) << 8);
    }
    uint32_t bus_read_u32(uint32_t address) const {
        const auto* p = ram(address);
        return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
               (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    }
    void bus_write_u8(uint32_t address,uint32_t value) { *ram(address)=uint8_t(value); ++writes; }
    void bus_write_u16(uint32_t address,uint32_t value) {
        auto* p=ram(address); p[0]=uint8_t(value); p[1]=uint8_t(value>>8); ++writes;
    }
    void bus_write_u32(uint32_t address,uint32_t value) {
        auto* p=ram(address);
        for(unsigned i=0;i!=4;++i) p[i]=uint8_t(value>>(8*i));
        ++writes;
    }
    uint32_t runtime_mem_cycles(uint32_t address,uint32_t width,uint32_t sequential) const {
        const unsigned region=address>>24;
        return 1u + (width==4 ? (sequential ? waits.s32[region] : waits.n32[region])
                                : (sequential ? waits.s16[region] : waits.n16[region]));
    }
    uint32_t runtime_prefetch_adjust(uint32_t wait,uint32_t address,uint32_t pc,uint32_t thumb) {
        if(!waits.prefetch || address>=0x08000000u) return 0;
        return uint32_t(rwt_prefetch_stall(&waits,int32_t(wait),pc,thumb)-int32_t(wait));
    }
    void runtime_tick(uint32_t elapsed) { cycles += elapsed; }
    void commit() { runtime_native_region_end(this); }
};
