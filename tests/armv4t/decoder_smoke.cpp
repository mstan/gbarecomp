// decoder_smoke — feed a fixed set of ARM and THUMB encodings through
// the decoders and assert each one produces an expected normalized-IR
// string. Used as a fast regression check; once jsmolka/gba-tests is
// imported we'll diff against the larger conformance corpus too.
//
// Each entry: source mode, raw bytes, address, expected IR string.

#include <cstdio>
#include <cstdint>
#include <string>

#include "arm_decode.h"
#include "thumb_decode.h"
#include "arm_ir.h"

namespace {

struct ArmCase {
    uint32_t word;
    uint32_t pc;
    const char* note;
    const char* expected;
};

struct ThumbCase {
    uint16_t hw;
    uint32_t pc;
    const char* note;
    const char* expected;
};

// ARM encodings. Expected strings derive from arm_ir.cpp's format_ir().
constexpr ArmCase kArm[] = {
    { 0xE3A00000, 0x08000000, "MOV r0, #0",
      "08000000 A mov r0,#0x0" },
    { 0xE3A0103F, 0x08000004, "MOV r1, #0x3F",
      "08000004 A mov r1,#0x3f" },
    { 0xE0810002, 0x08000008, "ADD r0, r1, r2",
      "08000008 A add r0,r1,r2" },
    { 0xE0410002, 0x0800000C, "SUB r0, r1, r2",
      "0800000c A sub r0,r1,r2" },
    { 0xE1A00081, 0x08000010, "MOV r0, r1, lsl #1",
      "08000010 A mov r0,r1,lsl #1" },
    { 0xE5901004, 0x08000014, "LDR r1, [r0, #4]",
      "08000014 A ldr r1,[r0,#0x4]" },
    { 0xE5801004, 0x08000018, "STR r1, [r0, #4]",
      "08000018 A str r1,[r0,#0x4]" },
    { 0xE92D4070, 0x0800001C, "STMDB sp!, {r4,r5,r6,lr}",
      "0800001c A stm r13!,{r4,r5,r6,r14}" },
    { 0xE8BD8070, 0x08000020, "LDM sp!, {r4,r5,r6,pc}",
      "08000020 A ldm r13!,{r4,r5,r6,r15}" },
    { 0xEAFFFFFE, 0x08000024, "B  self",
      "08000024 A b 0x08000024" },
    { 0xEBFFFFFE, 0x08000028, "BL self",
      "08000028 A bl 0x08000028" },
    { 0xE12FFF1E, 0x0800002C, "BX lr",
      "0800002c A bx r14" },
    { 0xEF000010, 0x08000030, "SWI #0x10",
      "08000030 A swi #0x10" },
    { 0xE0000091, 0x08000034, "MUL r0, r1, r0",
      "08000034 A mul r0,r1,r0" },
    { 0xE1500001, 0x08000038, "CMP r0, r1",
      "08000038 A cmps r0,r1" },

    // Shifted-register DP. ADD r0, r1, r2, lsl #3 = 0xE0810182.
    { 0xE0810182, 0x0800003C, "ADD r0,r1,r2,lsl #3",
      "0800003c A add r0,r1,r2,lsl #3" },
    // EOR r0, r1, r2 = 0xE0210002.
    { 0xE0210002, 0x08000040, "EOR r0,r1,r2",
      "08000040 A eor r0,r1,r2" },
    // ORR r0, r1, r2 = 0xE1810002.
    { 0xE1810002, 0x08000044, "ORR r0,r1,r2",
      "08000044 A orr r0,r1,r2" },
    // BIC r0, r1, r2 = 0xE1C10002.
    { 0xE1C10002, 0x08000048, "BIC r0,r1,r2",
      "08000048 A bic r0,r1,r2" },
    // RSB r0, r1, #0 = 0xE2610000  (negate r1 into r0).
    { 0xE2610000, 0x0800004C, "RSB r0,r1,#0",
      "0800004c A rsb r0,r1,#0x0" },
    // MVN r0, r1 = 0xE1E00001.
    { 0xE1E00001, 0x08000050, "MVN r0,r1",
      "08000050 A mvn r0,r1" },
    // LDR r0, [r1, #-8] = 0xE5110008 (U=0, pre-indexed, no writeback).
    { 0xE5110008, 0x08000054, "LDR r0,[r1,#-8]",
      "08000054 A ldr r0,[r1,#-0x8]" },
    // LDR r0, [r1], #4  (post-indexed) = 0xE4910004.
    { 0xE4910004, 0x08000058, "LDR r0,[r1],#4",
      "08000058 A ldr r0,[r1],#0x4" },
    // LDRH r0, [r1, #4] = 0xE1D100B4 (P=1, U=1, I=1, S/H=01 → LDRH,
    // imm = (hi<<4)|lo = 0x04).
    { 0xE1D100B4, 0x0800005C, "LDRH r0,[r1,#4]",
      "0800005c A ldrh r0,[r1,#0x4]" },
    // STRH r0, [r1, #4] = 0xE1C100B4.
    { 0xE1C100B4, 0x08000060, "STRH r0,[r1,#4]",
      "08000060 A strh r0,[r1,#0x4]" },
    // MRS r0, CPSR = 0xE10F0000.
    { 0xE10F0000, 0x08000064, "MRS r0,CPSR",
      "08000064 A mrs r0,cpsr" },
    // MSR CPSR_fc, r0 = 0xE129F000 (mask = 0x9 = flags+control).
    { 0xE129F000, 0x08000068, "MSR CPSR_fc,r0",
      "08000068 A msr cpsr_cf,r0" },
    // UMULL r0, r1, r2, r3 = 0xE0810392
    //   bits: cond=1110 0000 1000 0001 0011 1001 0010
    //   That's S=0, RdHi=r1, RdLo=r0, Rs=r3, Rm=r2.
    { 0xE0810392, 0x0800006C, "UMULL r0,r1,r2,r3",
      "0800006c A umull raw=0xe0810392" },
    // SWP r0, r1, [r2] = 0xE1020091.
    { 0xE1020091, 0x08000070, "SWP r0,r1,[r2]",
      "08000070 A swp raw=0xe1020091" },
    // Reserved/undefined slot: cond=1111 anything → SWI in ARMv4T
    // (the NV condition itself isn't an undefined trap; the dedicated
    // UDF encoding is "0111 1xxx xxxx xxxx xxxx xxx1 xxxx"). Encode
    // one: 0xE7F000F0 = bits 27..25 = 011, bit 4 = 1, with imm = 0xF0.
    // This is the ARMv4T UDF placeholder.
    { 0xE7F000F0, 0x08000074, "UDF (reserved)",
      "08000074 A UND raw=0xe7f000f0" },
};

// THUMB encodings. Includes coverage for formats 7, 8, 10, 11, 12, 13
// added in this pass.
constexpr ThumbCase kThumb[] = {
    { 0x2000, 0x08001000, "MOV r0, #0",
      "08001000 T movs r0,#0x0" },
    { 0x2103, 0x08001002, "MOV r1, #3",
      "08001002 T movs r1,#0x3" },
    { 0x1888, 0x08001004, "ADD r0,r1,r2",
      "08001004 T adds r0,r1,r2" },
    { 0x1A88, 0x08001006, "SUB r0,r1,r2",
      "08001006 T subs r0,r1,r2" },
    { 0x0048, 0x08001008, "LSL r0,r1,#1",
      "08001008 T movs r0,r1,lsl #1" },
    { 0x6841, 0x0800100A, "LDR r1,[r0,#4]",
      "0800100a T ldr r1,[r0,#0x4]" },
    { 0x6041, 0x0800100C, "STR r1,[r0,#4]",
      "0800100c T str r1,[r0,#0x4]" },
    { 0xB570, 0x0800100E, "PUSH {r4-r6,lr}",
      "0800100e T stm r13!,{r4,r5,r6,r14}" },
    { 0xBD70, 0x08001010, "POP  {r4-r6,pc}",
      "08001010 T ldm r13!,{r4,r5,r6,r15}" },
    { 0xE7FE, 0x08001012, "B .self",
      "08001012 T b 0x08001012" },
    { 0xDFAB, 0x08001014, "SWI #0xAB",
      "08001014 T swi #0xab" },
    { 0x4770, 0x08001016, "BX lr",
      "08001016 T bx r14" },

    // Format 7: LDR/STR reg-offset.
    //   0x5888 = 0101 100 010 001 000 → LDR r0,[r1,r2]
    { 0x5888, 0x08001100, "LDR r0,[r1,r2]",
      "08001100 T ldr r0,[r1,+r2]" },
    //   0x5088 → STR r0,[r1,r2]
    { 0x5088, 0x08001102, "STR r0,[r1,r2]",
      "08001102 T str r0,[r1,+r2]" },

    // Format 8: sign-extended halfword/byte reg-offset.
    //   0x5E88 = 0101 1110 10001000 → LDRSH r0,[r1,r2]
    { 0x5E88, 0x08001104, "LDRSH r0,[r1,r2]",
      "08001104 T ldrsh r0,[r1,+r2]" },
    //   0x5688 → LDRSB r0,[r1,r2]
    { 0x5688, 0x08001106, "LDRSB r0,[r1,r2]",
      "08001106 T ldrsb r0,[r1,+r2]" },
    //   0x5A88 → LDRH r0,[r1,r2]
    { 0x5A88, 0x08001108, "LDRH r0,[r1,r2]",
      "08001108 T ldrh r0,[r1,+r2]" },
    //   0x5288 → STRH r0,[r1,r2]
    { 0x5288, 0x0800110A, "STRH r0,[r1,r2]",
      "0800110a T strh r0,[r1,+r2]" },

    // Format 10: LDRH/STRH imm5 (offset scaled by 2).
    //   0x8848 = 1000 1 00001 001 000 → LDRH r0,[r1,#2]
    { 0x8848, 0x0800110C, "LDRH r0,[r1,#2]",
      "0800110c T ldrh r0,[r1,#0x2]" },
    //   0x8048 → STRH r0,[r1,#2]
    { 0x8048, 0x0800110E, "STRH r0,[r1,#2]",
      "0800110e T strh r0,[r1,#0x2]" },

    // Format 11: SP-relative LDR/STR (imm8 scaled by 4).
    //   0x9801 = 1001 1 000 00000001 → LDR r0,[SP,#4]
    { 0x9801, 0x08001110, "LDR r0,[SP,#4]",
      "08001110 T ldr r0,[r13,#0x4]" },
    //   0x9001 → STR r0,[SP,#4]
    { 0x9001, 0x08001112, "STR r0,[SP,#4]",
      "08001112 T str r0,[r13,#0x4]" },

    // Format 12: ADD Rd, PC|SP, #imm8 << 2.
    //   0xA001 = 1010 0 000 00000001 → ADD r0, PC, #4
    { 0xA001, 0x08001114, "ADD r0,PC,#4",
      "08001114 T add r0,r15,#0x4" },
    //   0xA801 → ADD r0, SP, #4
    { 0xA801, 0x08001116, "ADD r0,SP,#4",
      "08001116 T add r0,r13,#0x4" },

    // Format 13: ADD/SUB SP, #imm7 << 2.
    //   0xB004 = 1011 0000 0 0000100 → ADD SP, #16
    { 0xB004, 0x08001118, "ADD SP,#16",
      "08001118 T add r13,r13,#0x10" },
    //   0xB084 = 1011 0000 1 0000100 → SUB SP, #16
    { 0xB084, 0x0800111A, "SUB SP,#16",
      "0800111a T sub r13,r13,#0x10" },
};

int compare(const std::string& got, const char* expected, const char* note) {
    if (got == expected) return 0;
    std::printf("FAIL  %-30s\n  got     : %s\n  expected: %s\n",
                note, got.c_str(), expected);
    return 1;
}

int run() {
    int failures = 0;

    std::printf("== ARM decoder smoke ==\n");
    for (const auto& c : kArm) {
        auto ir = armv4t::ArmDecoder::decode(c.word, c.pc);
        std::string out = armv4t::format_ir(ir);
        std::printf("%-30s -> %s\n", c.note, out.c_str());
        failures += compare(out, c.expected, c.note);
    }

    std::printf("\n== THUMB decoder smoke ==\n");
    for (const auto& c : kThumb) {
        auto ir = armv4t::ThumbDecoder::decode(c.hw, c.pc);
        std::string out = armv4t::format_ir(ir);
        std::printf("%-30s -> %s\n", c.note, out.c_str());
        failures += compare(out, c.expected, c.note);
    }

    if (failures) {
        std::printf("\n%d failure(s)\n", failures);
        return 1;
    }
    std::printf("\nOK (%zu arm + %zu thumb)\n",
                sizeof(kArm) / sizeof(kArm[0]),
                sizeof(kThumb) / sizeof(kThumb[0]));
    return 0;
}

}  // namespace

int main() { return run(); }
