// interpreter_smoke — feed concrete ARM/THUMB programs through the
// decoder + interpreter and assert the resulting CPU/memory state.
//
// This is the bring-up validation surface: every behavior covered
// here will eventually be cross-checked against jsmolka/gba-tests and
// nba-emu/hw-test, but the in-tree tests give us a tight feedback
// loop while those importers are still being written.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "arm_decode.h"
#include "arm_ir.h"
#include "bus.h"
#include "cpu_state.h"
#include "interpreter.h"
#include "thumb_decode.h"

namespace {

// Flat-memory bus, sized to 64 KB starting at address 0. Plenty for
// the tiny programs in this file. Out-of-range accesses fail loudly so
// a buggy interpreter can't quietly miss.
struct FlatBus : armv4t::Bus {
    std::vector<uint8_t> mem;
    uint32_t base = 0;
    bool oob = false;

    FlatBus(std::size_t size, uint32_t base_addr = 0)
        : mem(size, 0), base(base_addr) {}

    bool in(uint32_t addr, std::size_t w) {
        if (addr < base) return false;
        std::size_t off = addr - base;
        return off + w <= mem.size();
    }

    uint8_t read8(uint32_t addr) override {
        if (!in(addr, 1)) { oob = true; return 0; }
        return mem[addr - base];
    }
    uint16_t read16(uint32_t addr) override {
        if (!in(addr, 2)) { oob = true; return 0; }
        const uint8_t* p = &mem[addr - base];
        return static_cast<uint16_t>(p[0] | (p[1] << 8));
    }
    uint32_t read32(uint32_t addr) override {
        if (!in(addr, 4)) { oob = true; return 0; }
        const uint8_t* p = &mem[addr - base];
        return static_cast<uint32_t>(p[0] | (p[1] << 8) |
                                    (p[2] << 16) | (p[3] << 24));
    }
    void write8(uint32_t addr, uint8_t v) override {
        if (!in(addr, 1)) { oob = true; return; }
        mem[addr - base] = v;
    }
    void write16(uint32_t addr, uint16_t v) override {
        if (!in(addr, 2)) { oob = true; return; }
        uint8_t* p = &mem[addr - base];
        p[0] = static_cast<uint8_t>(v & 0xFF);
        p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    }
    void write32(uint32_t addr, uint32_t v) override {
        if (!in(addr, 4)) { oob = true; return; }
        uint8_t* p = &mem[addr - base];
        p[0] = static_cast<uint8_t>(v & 0xFF);
        p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
        p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
    }
};

armv4t::CPUState make_cpu() {
    armv4t::CPUState cpu{};
    cpu.cpsr.mode = static_cast<uint8_t>(armv4t::Mode::System);
    return cpu;
}

// Run ARM instructions sequentially starting at code_base. The caller
// gives a flat list of 32-bit words; the loop fetches at cpu.R[15],
// decodes, and steps until N instructions have run or PC leaves the
// code region.
int run_arm(armv4t::CPUState& cpu, FlatBus& bus,
            uint32_t code_base,
            const std::vector<uint32_t>& code,
            int max_steps) {
    // Lay code into memory.
    for (std::size_t k = 0; k < code.size(); ++k) {
        bus.write32(code_base + static_cast<uint32_t>(k * 4), code[k]);
    }
    cpu.R[15] = code_base;
    cpu.cpsr.t = false;
    cpu.thumb = false;
    int steps = 0;
    while (steps < max_steps) {
        uint32_t pc = cpu.R[15];
        // Stop if we've walked off the end of the program.
        if (pc < code_base ||
            pc >= code_base + code.size() * 4) {
            break;
        }
        uint32_t word = bus.read32(pc);
        auto i = armv4t::ArmDecoder::decode(word, pc);
        auto r = armv4t::Interpreter::step(cpu, bus, i);
        ++steps;
        if (r == armv4t::Interpreter::Result::Undefined ||
            r == armv4t::Interpreter::Result::NotImplemented ||
            r == armv4t::Interpreter::Result::Swi) {
            break;
        }
    }
    return steps;
}

int run_thumb(armv4t::CPUState& cpu, FlatBus& bus,
              uint32_t code_base,
              const std::vector<uint16_t>& code,
              int max_steps) {
    for (std::size_t k = 0; k < code.size(); ++k) {
        bus.write16(code_base + static_cast<uint32_t>(k * 2), code[k]);
    }
    cpu.R[15] = code_base;
    cpu.cpsr.t = true;
    cpu.thumb = true;
    int steps = 0;
    while (steps < max_steps) {
        uint32_t pc = cpu.R[15];
        if (pc < code_base ||
            pc >= code_base + code.size() * 2) {
            break;
        }
        uint16_t hw = bus.read16(pc);
        auto i = armv4t::ThumbDecoder::decode(hw, pc);
        auto r = armv4t::Interpreter::step(cpu, bus, i);
        ++steps;
        if (r == armv4t::Interpreter::Result::Undefined ||
            r == armv4t::Interpreter::Result::NotImplemented ||
            r == armv4t::Interpreter::Result::Swi) {
            break;
        }
    }
    return steps;
}

int failures = 0;

void check_eq_u32(const char* test, const char* tag,
                  uint32_t got, uint32_t expect) {
    if (got != expect) {
        std::printf("FAIL %s: %s = 0x%08x (expected 0x%08x)\n",
                    test, tag, got, expect);
        ++failures;
    }
}

void check_bool(const char* test, const char* tag,
                bool got, bool expect) {
    if (got != expect) {
        std::printf("FAIL %s: %s = %d (expected %d)\n",
                    test, tag, got ? 1 : 0, expect ? 1 : 0);
        ++failures;
    }
}

// ────────────────────────────────────────────────────────────────────
// Tests
// ────────────────────────────────────────────────────────────────────

void test_arm_mov_add() {
    auto cpu = make_cpu();
    FlatBus bus(64 * 1024);
    // MOV r0, #5 ; MOV r1, #7 ; ADD r2, r0, r1
    std::vector<uint32_t> code = {
        0xE3A00005,
        0xE3A01007,
        0xE0802001,
    };
    run_arm(cpu, bus, 0x100, code, 16);
    check_eq_u32("arm_mov_add", "r0", cpu.R[0], 5);
    check_eq_u32("arm_mov_add", "r1", cpu.R[1], 7);
    check_eq_u32("arm_mov_add", "r2", cpu.R[2], 12);
}

void test_arm_cmp_flags() {
    auto cpu = make_cpu();
    FlatBus bus(64 * 1024);
    // MOV r0, #5 ; MOV r1, #5 ; CMP r0, r1
    std::vector<uint32_t> code = {
        0xE3A00005,
        0xE3A01005,
        0xE1500001,
    };
    run_arm(cpu, bus, 0x100, code, 16);
    check_bool("arm_cmp_flags", "Z", cpu.cpsr.z, true);
    check_bool("arm_cmp_flags", "N", cpu.cpsr.n, false);
    // Subtraction with no borrow → C = 1 on ARM.
    check_bool("arm_cmp_flags", "C", cpu.cpsr.c, true);
    check_bool("arm_cmp_flags", "V", cpu.cpsr.v, false);
}

void test_arm_ldr_str() {
    auto cpu = make_cpu();
    FlatBus bus(64 * 1024);
    // MOV r0, #0x1000 ; MOV r1, #0xAB ; STR r1, [r0] ; LDR r2, [r0]
    std::vector<uint32_t> code = {
        0xE3A00A01,  // MOV r0, #0x1000 (rotate-imm: 0x01 ror 20 = 0x1000)
        0xE3A010AB,  // MOV r1, #0xAB
        0xE5801000,  // STR r1, [r0]
        0xE5902000,  // LDR r2, [r0]
    };
    run_arm(cpu, bus, 0x100, code, 16);
    check_eq_u32("arm_ldr_str", "r0", cpu.R[0], 0x1000);
    check_eq_u32("arm_ldr_str", "r1", cpu.R[1], 0xAB);
    check_eq_u32("arm_ldr_str", "r2", cpu.R[2], 0xAB);
    check_eq_u32("arm_ldr_str", "mem[0x1000]", bus.read32(0x1000), 0xAB);
}

void test_arm_branch() {
    auto cpu = make_cpu();
    FlatBus bus(64 * 1024);
    // MOV r0, #1                    @ 0x100
    // B  +8 (to 0x110, skipping next)@ 0x104   offset = +8
    //   imm24 = (0x110 - (0x104+8)) >> 2 = (0x110 - 0x10C)/4 = 1
    //   encoding: 0xEA000001
    // MOV r0, #2                    @ 0x108   (skipped)
    // MOV r0, #3                    @ 0x10C   (skipped — fallthrough)
    // MOV r1, #9                    @ 0x110
    std::vector<uint32_t> code = {
        0xE3A00001,
        0xEA000000,  // B +0 → branches to next-after, i.e. 0x10C
                     //   target = 0x104 + 8 + (0 << 2) = 0x10C
        0xE3A00002,
        0xE3A01009,
    };
    run_arm(cpu, bus, 0x100, code, 8);
    // r0 should still be 1 (instruction at 0x108 skipped).
    check_eq_u32("arm_branch", "r0", cpu.R[0], 1);
    check_eq_u32("arm_branch", "r1", cpu.R[1], 9);
}

void test_arm_bl_returns_link() {
    auto cpu = make_cpu();
    FlatBus bus(64 * 1024);
    // BL +0 (sets LR to next instruction, branches to 0x10C)
    //   encoding: 0xEB000000 → target = 0x100 + 8 + 0 = 0x108? No,
    //   pc here is 0x100, offset = 0, target = pc+8+0 = 0x108.
    //   So BL at 0x100 sets LR = 0x104 and PC = 0x108.
    //   Then MOV r0, #0xAA at 0x108.
    std::vector<uint32_t> code = {
        0xEB000000,   // 0x100: BL 0x108
        0xE3A00099,   // 0x104: (skipped) MOV r0, #0x99
        0xE3A000AA,   // 0x108: MOV r0, #0xAA
    };
    run_arm(cpu, bus, 0x100, code, 8);
    check_eq_u32("arm_bl_returns_link", "lr", cpu.R[14], 0x104);
    check_eq_u32("arm_bl_returns_link", "r0", cpu.R[0], 0xAA);
}

void test_arm_push_pop() {
    // Build a sequence that pushes r4..r6+lr, modifies r4, then pops.
    // We model PUSH/POP via STMDB / LDMIA with writeback.
    auto cpu = make_cpu();
    FlatBus bus(64 * 1024);
    cpu.R[13] = 0x2000;       // SP at top of small stack
    cpu.R[4]  = 0xDEAD0001;
    cpu.R[5]  = 0xDEAD0002;
    cpu.R[6]  = 0xDEAD0003;
    cpu.R[14] = 0x00CAFE00;
    // STMDB sp!, {r4-r6, lr}    = 0xE92D4070
    // MOV r4, #0
    // LDMIA sp!, {r4-r6, lr}    = 0xE8BD4070
    std::vector<uint32_t> code = {
        0xE92D4070,
        0xE3A04000,
        0xE8BD4070,
    };
    run_arm(cpu, bus, 0x100, code, 8);
    check_eq_u32("arm_push_pop", "sp", cpu.R[13], 0x2000);
    check_eq_u32("arm_push_pop", "r4", cpu.R[4], 0xDEAD0001);
    check_eq_u32("arm_push_pop", "r5", cpu.R[5], 0xDEAD0002);
    check_eq_u32("arm_push_pop", "r6", cpu.R[6], 0xDEAD0003);
    check_eq_u32("arm_push_pop", "lr", cpu.R[14], 0x00CAFE00);
}

void test_arm_mul() {
    auto cpu = make_cpu();
    FlatBus bus(64 * 1024);
    // MOV r1, #6 ; MOV r2, #7 ; MUL r0, r1, r2
    //   MUL: cond 0000 00AS Rd 0000 Rs 1001 Rm
    //   MUL r0, r1, r2 → Rd=r0, Rm=r1, Rs=r2, S=0, A=0:
    //     1110 0000 0000 0000 0000 0010 1001 0001 = 0xE0000291
    std::vector<uint32_t> code = {
        0xE3A01006,
        0xE3A02007,
        0xE0000291,
    };
    run_arm(cpu, bus, 0x100, code, 8);
    check_eq_u32("arm_mul", "r0", cpu.R[0], 42);
}

void test_thumb_add_imm() {
    auto cpu = make_cpu();
    FlatBus bus(64 * 1024);
    // MOV r0, #5  (T fmt3)         0x2005
    // ADD r0, #10 (T fmt3 ADD)     0x300A
    // MOV r1, #3                   0x2103
    // ADD r0, r0, r1 (T fmt2)      0x1840 → Rs=r0, Rd=r0, Rn=r1
    std::vector<uint16_t> code = {
        0x2005,
        0x300A,
        0x2103,
        0x1840,
    };
    run_thumb(cpu, bus, 0x100, code, 8);
    check_eq_u32("thumb_add_imm", "r0", cpu.R[0], 18);
}

void test_thumb_ldr_str() {
    auto cpu = make_cpu();
    FlatBus bus(64 * 1024);
    cpu.R[0] = 0x1000;
    cpu.R[1] = 0x5555;
    // STR r1, [r0, #0]  →  0x6001
    // LDR r2, [r0, #0]  →  0x6802
    std::vector<uint16_t> code = {
        0x6001,
        0x6802,
    };
    run_thumb(cpu, bus, 0x100, code, 8);
    check_eq_u32("thumb_ldr_str", "r2", cpu.R[2], 0x5555);
    check_eq_u32("thumb_ldr_str", "mem[0x1000]", bus.read32(0x1000), 0x5555);
}

void test_thumb_bx_to_arm() {
    auto cpu = make_cpu();
    FlatBus bus(64 * 1024);
    // THUMB code at 0x100:
    //   MOV r0, #0x40        0x2040
    //   ADD r0, PC, #0       0xA000   → r0 = (PC+4) & ~3 = 0x108
    //   Then we point r0 explicitly at the ARM payload below.
    //   Just set r0 directly via a MOV imm and BX r0.
    //   Use ARM payload at 0x200; ARM address has bit 0 = 0.
    //
    // Approach: write THUMB { MOV r0, #0x200; BX r0 }, then put an
    // ARM MOV r2, #0xAB at 0x200.
    //   MOV r0, #0x80  is the largest imm we can encode; but the
    //   THUMB MOV r0, #imm8 is fmt3 (8-bit). 0x80 fits but 0x200
    //   does not. So instead: build 0x200 via two adds.
    //
    // Simpler: put the ARM target word at 0x140 (fits in 8-bit MOV
    // imm? no — 0x140 still doesn't fit in 8 bits). Use 0x80.
    bus.write32(0x80, 0xE3A020AB);     // ARM: MOV r2, #0xAB
    bus.write32(0x84, 0xEAFFFFFE);     // ARM: B self (halt)
    std::vector<uint16_t> code = {
        0x2080,    // MOV r0, #0x80
        0x4700,    // BX  r0  (bit 0 = 0 → switch to ARM)
    };
    run_thumb(cpu, bus, 0x100, code, 8);
    // After the BX, we're in ARM; the loop's run_thumb stopped because
    // PC left the THUMB code region. Continue in ARM manually.
    int more = 0;
    while (more++ < 4) {
        uint32_t pc = cpu.R[15];
        if (pc < 0x80 || pc >= 0x88) break;
        auto i = armv4t::ArmDecoder::decode(bus.read32(pc), pc);
        auto r = armv4t::Interpreter::step(cpu, bus, i);
        if (r != armv4t::Interpreter::Result::Normal &&
            r != armv4t::Interpreter::Result::Branched) break;
    }
    check_bool("thumb_bx_to_arm", "T cleared", cpu.cpsr.t, false);
    check_eq_u32("thumb_bx_to_arm", "r2", cpu.R[2], 0xAB);
}

// Reproduce the BIOS IWRAM-clear loop in miniature. R1 starts
// negative, increments by 4 until 0, BLT branches while negative.
// Encoding matches BIOS bytes at 0x120-0x124:
//   0x5060  STR R0, [R4, R1]
//   0x1D09  ADDS R1, R1, #4
//   0xDBFC  BLT  -4   (back to STR)
void test_thumb_blt_loop_terminates() {
    auto cpu = make_cpu();
    cpu.cpsr.t = true;
    cpu.thumb = true;
    FlatBus bus(64 * 1024);
    cpu.R[0] = 0;
    cpu.R[1] = static_cast<uint32_t>(-32);  // 8 iterations expected
    cpu.R[4] = 0x100;
    bus.write16(0x40, 0x5060);
    bus.write16(0x42, 0x1D09);
    bus.write16(0x44, 0xDBFC);
    cpu.R[15] = 0x40;
    int max_steps = 64;
    int taken = 0;
    while (taken < max_steps) {
        uint32_t pc = cpu.R[15];
        // Loop body covers [0x40, 0x46). We stop the moment PC leaves.
        if (pc < 0x40 || pc >= 0x46) break;
        auto i = armv4t::ThumbDecoder::decode(bus.read16(pc), pc);
        auto r = armv4t::Interpreter::step(cpu, bus, i);
        ++taken;
        if (r == armv4t::Interpreter::Result::NotImplemented ||
            r == armv4t::Interpreter::Result::Undefined) break;
    }
    check_eq_u32("thumb_blt_loop_terminates", "r1", cpu.R[1], 0);
    check_eq_u32("thumb_blt_loop_terminates", "final_pc", cpu.R[15], 0x46u);
    if (taken != 24) {
        std::printf("FAIL thumb_blt_loop_terminates: ran %d steps (expected 24)\n", taken);
        ++failures;
    }
}

}  // namespace

int main() {
    test_arm_mov_add();
    test_arm_cmp_flags();
    test_arm_ldr_str();
    test_arm_branch();
    test_arm_bl_returns_link();
    test_arm_push_pop();
    test_arm_mul();
    test_thumb_add_imm();
    test_thumb_ldr_str();
    test_thumb_bx_to_arm();
    test_thumb_blt_loop_terminates();

    if (failures) {
        std::printf("\n%d failure(s)\n", failures);
        return 1;
    }
    std::printf("interpreter_smoke: OK\n");
    return 0;
}
