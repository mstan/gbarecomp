// tests/codegen/test_cases.cpp — the L1 test corpus.
//
// One entry per per-IrOp shape we want to pin. The architecture is
// the deliverable for the first pass; expanding the corpus to every
// addressing mode + every shift type is incremental work.
//
// Encoding cheat sheet for the curious (all 32-bit ARM words):
//   DP imm:    cond 00 1 opc S Rn Rd  rot4 imm8
//   DP reg:    cond 00 0 opc S Rn Rd  shift_imm  st 0 Rm
//   DP rsr:    cond 00 0 opc S Rn Rd  Rs   0 st 1 Rm
//   LDR/STR:   cond 01 I P U B W L Rn Rd  offset12-or-shifted-reg
//   LDRH/STRH: cond 00 0 P U I W L Rn Rd  0000 1 S H 1 Rm-or-imm
//   B/BL:      cond 101 L  imm24
//   BX:        cond 0001 0010 1111 1111 1111 0001 Rm
//   MUL:       cond 0000 00 0 S Rd 0000 Rs 1001 Rm
//   MLA:       cond 0000 00 1 S Rd Rn   Rs 1001 Rm
//   UMULL:     cond 0000 10 0 S RdHi RdLo Rs 1001 Rm
//   SMULL:     cond 0000 11 0 S RdHi RdLo Rs 1001 Rm
//   SWP:       cond 0001 0 0 00 Rn Rd 0000 1001 Rm
//   MRS CPSR:  cond 0001 0000 1111 Rd  0000 0000 0000
//   MSR CPSR:  cond 0001 0010 mask 1111 0000 0000 Rm  (reg form)
//   SWI:       cond 1111 imm24
//
// (`L`=load-bit, `S`=sign-extend, `H`=halfword, etc.)

#include "test_cases.h"

namespace {

// ── Memory init blocks (file-scope so we can take their addresses)

const TestMemInit mem_word_at_1000_dead[] = {
    {0x1000u, 0xDEADBEEFu},
};

const TestMemInit mem_word_at_1004_cafe[] = {
    {0x1004u, 0xCAFEF00Du},
};

const TestMemInit mem_word_at_1000_misaligned_pattern[] = {
    // a 32-bit word the LDR test will rotate
    {0x1000u, 0x11223344u},
};

const TestMemInit mem_block_ldm_pattern[] = {
    {0x2000u, 0x00000111u},
    {0x2004u, 0x00000222u},
    {0x2008u, 0x00000333u},
    {0x200Cu, 0x00000444u},
};

const TestMemInit mem_word_at_2000_target[] = {
    {0x2000u, 0x00001234u},
};

const TestMemInit mem_swp_init[] = {
    {0x3000u, 0xAAAA5555u},
};

}  // namespace

// CPSR helpers expressed inline — the codegen test cases are
// allowed to start with N/Z/C/V cleared and System mode (0x1F) which
// has no banking weirdness. SVC (0x13) is used for exception-return
// tests.
//   System mode: 0x1F
#define MODE_SYSTEM 0x1Fu
#define MODE_FIQ    0x11u

const TestCase kTestCases[] = {
    // ── DP imm: MOV ─────────────────────────────────────────────────
    {
        "arm_mov_imm",
        /*thumb*/ false, /*pc*/ 0x100u,
        /*word*/ 0xE3A00005u,  // MOV r0, #5
        /* R */ {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        /* cpsr */ MODE_SYSTEM,
        nullptr, 0,
        /*branches*/ false, 0, 0,
    },

    // ── DP imm: ADD r2, r0, #7 with prior r0 = 5 ────────────────────
    {
        "arm_add_imm",
        false, 0x100u,
        0xE2802007u,  // ADD r2, r0, #7
        {5,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        MODE_SYSTEM,
        nullptr, 0,
        false, 0, 0,
    },

    // ── DP imm with S: CMP r0, #5 when r0=5 (Z=1, C=1 per ARM SBC) ──
    {
        "arm_cmp_imm_eq",
        false, 0x100u,
        0xE3500005u,  // CMP r0, #5
        {5,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        MODE_SYSTEM,
        nullptr, 0,
        false, 0, 0,
    },

    // ── DP imm with S: CMP r0, #5 when r0=4 (Z=0, N=1, C=0) ─────────
    {
        "arm_cmp_imm_lt",
        false, 0x100u,
        0xE3500005u,
        {4,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        MODE_SYSTEM,
        nullptr, 0,
        false, 0, 0,
    },

    // ── DP imm with S: ADDS r0, r0, #0xFFFFFFFF wraps to 0 (Z=1,C=1)
    //    Encoding immediate of 0xFFFFFFFF? Not encodable. Use SUBS
    //    instead: r0=1, SUBS r0, r0, #1 → 0, Z=1, C=1.
    {
        "arm_subs_to_zero",
        false, 0x100u,
        0xE2500001u,  // SUBS r0, r0, #1
        {1,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        MODE_SYSTEM,
        nullptr, 0,
        false, 0, 0,
    },

    // ── DP shifted-register (imm count): MOV r1, r0, LSL #4 ─────────
    {
        "arm_mov_lsl_imm",
        false, 0x100u,
        0xE1A01200u,  // MOV r1, r0, LSL #4
        {0x01234567u,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        MODE_SYSTEM,
        nullptr, 0,
        false, 0, 0,
    },

    // ── DP shifted-register (imm count): MOVS r1, r0, LSR #1 sets C
    //    from the bit shifted out (bit 0).
    {
        "arm_movs_lsr_imm_sets_c",
        false, 0x100u,
        0xE1B010A0u,  // MOVS r1, r0, LSR #1
        {0x80000001u,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        MODE_SYSTEM,
        nullptr, 0,
        false, 0, 0,
    },

    // ── DP shifted-register (imm count): MOVS r1, r0, ASR #1 with
    //    r0 having top bit set — N=1, sign-extended.
    {
        "arm_movs_asr_imm_neg",
        false, 0x100u,
        0xE1B010C0u,  // MOVS r1, r0, ASR #1
        {0x80000000u,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        MODE_SYSTEM,
        nullptr, 0,
        false, 0, 0,
    },

    // ── DP reg-shifted-by-register: MOV r1, r0, LSL r2 with r2=4 ────
    {
        "arm_mov_lsl_reg",
        false, 0x100u,
        0xE1A01210u,  // MOV r1, r0, LSL r2
        {0x01234567u,0,4,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        MODE_SYSTEM,
        nullptr, 0,
        false, 0, 0,
    },

    // ── DP reg-shifted-by-register: shift count > 32 (LSL r2=33 → 0)
    // ARM register-controlled shifts read PC as PC+12 when PC is Rm.
    {
        "arm_mov_pc_lsl_reg_uses_pc_plus_12",
        false, 0x100u,
        0xE1A0001Fu,  // MOV r0, pc, LSL r0
        {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        MODE_SYSTEM,
        nullptr, 0,
        false, 0, 0,
    },

    // ARM register-controlled shifts also make Rn=PC read as PC+12.
    {
        "arm_add_pc_lsl_reg_uses_pc_plus_12",
        false, 0x100u,
        0xE08F0010u,  // ADD r0, pc, r0, LSL r0
        {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        MODE_SYSTEM,
        nullptr, 0,
        false, 0, 0,
    },

    {
        "arm_mov_lsl_reg_overflow",
        false, 0x100u,
        0xE1B01210u,  // MOVS r1, r0, LSL r2  (S=1 to test C-out=0)
        {0xFFFFFFFFu,0,33,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        MODE_SYSTEM,
        nullptr, 0,
        false, 0, 0,
    },

    // ── DP reg-shifted-by-register: count = 32 (LSL): C = Rm bit 0 ──
    {
        "arm_mov_lsl_reg_32",
        false, 0x100u,
        0xE1B01210u,  // MOVS r1, r0, LSL r2
        {0x00000003u,0,32,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        MODE_SYSTEM,
        nullptr, 0,
        false, 0, 0,
    },

    // ── LDR with imm offset: LDR r2, [r0, #0] ───────────────────────
    {
        "arm_ldr_imm",
        false, 0x100u,
        0xE5902000u,  // LDR r2, [r0, #0]
        {0x1000u,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        MODE_SYSTEM,
        mem_word_at_1000_dead, 1,
        false, 0, 0,
    },

    // ── LDR with misaligned address: LDR r2, [r0] where r0=0x1002
    //    should rotate the 32-bit word.
    {
        "arm_ldr_misaligned_rotate",
        false, 0x100u,
        0xE5902000u,
        {0x1002u,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        MODE_SYSTEM,
        mem_word_at_1000_misaligned_pattern, 1,
        false, 0, 0,
    },

    // ── STR with imm offset + writeback (pre-indexed) ───────────────
    //    STR r1, [r0, #4]!  →  store r1 at r0+4, then r0 += 4.
    {
        "arm_str_imm_pre_wb",
        false, 0x100u,
        0xE5A01004u,  // STR r1, [r0, #4]!
        {0x1000u, 0xCAFEBABEu, 0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        MODE_SYSTEM,
        nullptr, 0,
        false, 0, 0,
    },

    // ── LDRB ────────────────────────────────────────────────────────
    {
        "arm_ldrb_imm",
        false, 0x100u,
        0xE5D02001u,  // LDRB r2, [r0, #1]
        {0x1000u,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        MODE_SYSTEM,
        mem_word_at_1000_dead, 1,
        false, 0, 0,
    },

    // ── LDRH ────────────────────────────────────────────────────────
    {
        "arm_ldrh_imm",
        false, 0x100u,
        0xE1D020B2u,  // LDRH r2, [r0, #2]  (imm-form halfword)
        {0x1000u,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        MODE_SYSTEM,
        mem_word_at_1000_dead, 1,
        false, 0, 0,
    },

    // ── LDRSB returns sign-extended byte ────────────────────────────
    {
        "arm_ldrsb_imm",
        false, 0x100u,
        0xE1D020D0u,  // LDRSB r2, [r0, #0]
        {0x1000u,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        MODE_SYSTEM,
        mem_word_at_1000_dead, 1,
        false, 0, 0,
    },

    // ── LDM IA with writeback: LDMIA r0!, {r1-r4} ────────────────────
    {
        "arm_ldmia_wb",
        false, 0x100u,
        0xE8B0001Eu,  // LDMIA r0!, {r1, r2, r3, r4}
        {0x2000u,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        MODE_SYSTEM,
        mem_block_ldm_pattern, 4,
        false, 0, 0,
    },

    // ── STMDB SP! (PUSH-style) ──────────────────────────────────────
    //    STMDB r0!, {r1, r2}  — pre-decrement, writeback.
    {
        "arm_ldmia_wb_base_in_list",
        false, 0x100u,
        0xE8B10006u,  // LDMIA r1!, {r1, r2}
        {0,0x2000u,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        MODE_SYSTEM,
        mem_block_ldm_pattern, 4,
        false, 0, 0,
    },

    {
        "arm_stmdb_wb",
        false, 0x100u,
        0xE9200006u,  // STMDB r0!, {r1, r2}
        {0x2010u, 0xAAAAAAAAu, 0xBBBBBBBBu, 0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        MODE_SYSTEM,
        nullptr, 0,
        false, 0, 0,
    },

    // ── MUL r0, r1, r2 ──────────────────────────────────────────────
    {
        "arm_stmdb_user_regs_from_fiq",
        false, 0x100u,
        0xE9400300u,  // STMDB r0, {r8, r9}^
        {0x2008u,0,0,0, 0,0,0,0, 0x40u,0x63u,0,0, 0,0,0, 0x100u},
        MODE_FIQ,
        nullptr, 0,
        false, 0, 0,
    },

    {
        "arm_stmdb_wb_base_not_first",
        false, 0x100u,
        0xE921000Fu,  // STMDB r1!, {r0, r1, r2, r3}
        {0xAAAAAAAAu,0x2010u,0xBBBBBBBBu,0xCCCCCCCCu, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        MODE_SYSTEM,
        nullptr, 0,
        false, 0, 0,
    },

    {
        "arm_ldmia_empty_wb",
        false, 0x100u,
        0xE8B00000u,  // LDMIA r0!, {}
        {0x2000u,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        MODE_SYSTEM,
        mem_word_at_2000_target, 1,
        true, 0, 0,
    },

    {
        "arm_stmia_empty_wb",
        false, 0x100u,
        0xE8A00000u,  // STMIA r0!, {}
        {0x2000u,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        MODE_SYSTEM,
        nullptr, 0,
        false, 0, 0,
    },

    {
        "arm_stmda_empty_wb",
        false, 0x100u,
        0xE8200000u,  // STMDA r0!, {}
        {0x2040u,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        MODE_SYSTEM,
        nullptr, 0,
        false, 0, 0,
    },

    {
        "arm_stmdb_empty_wb",
        false, 0x100u,
        0xE9200000u,  // STMDB r0!, {}
        {0x2040u,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        MODE_SYSTEM,
        nullptr, 0,
        false, 0, 0,
    },

    {
        "arm_stmib_empty_wb",
        false, 0x100u,
        0xE9A00000u,  // STMIB r0!, {}
        {0x2000u,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        MODE_SYSTEM,
        nullptr, 0,
        false, 0, 0,
    },

    {
        "arm_mul",
        false, 0x100u,
        0xE0000291u,  // MUL r0, r1, r2
        {0, 6, 7, 0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        MODE_SYSTEM,
        nullptr, 0,
        false, 0, 0,
    },

    // ── MLA r0, r1, r2, r3 (r0 = r1*r2 + r3) ────────────────────────
    //    cond 0000 001 S Rd Rn Rs 1001 Rm; here Rd=0, Rn=3, Rs=2, Rm=1
    //    word = 0xE0203291
    {
        "arm_mla",
        false, 0x100u,
        0xE0203291u,
        {0, 6, 7, 10, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        MODE_SYSTEM,
        nullptr, 0,
        false, 0, 0,
    },

    // ── UMULL RdLo, RdHi, Rm, Rs ────────────────────────────────────
    //    UMULL r0, r1, r2, r3  ⇒ {r1, r0} = r2 * r3 (unsigned)
    //    Encoding: cond 0000 100 S RdHi RdLo Rs 1001 Rm
    //    RdHi=1, RdLo=0, Rs=3, Rm=2 → word = 0xE0810392
    {
        "arm_umull",
        false, 0x100u,
        0xE0810392u,
        {0, 0, 0x10000u, 0x20000u, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        MODE_SYSTEM,
        nullptr, 0,
        false, 0, 0,
    },

    // ── SWP r2, r1, [r0] ────────────────────────────────────────────
    //    Encoding: cond 0001 0000 Rn Rd 0000 1001 Rm
    //    Rn=0, Rd=2, Rm=1: word = 0xE1002091
    {
        "arm_swp",
        false, 0x100u,
        0xE1002091u,
        {0x3000u, 0x77770000u, 0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        MODE_SYSTEM,
        mem_swp_init, 1,
        false, 0, 0,
    },

    // ── MRS r0, CPSR ────────────────────────────────────────────────
    //    Encoding: cond 0001 0000 1111 Rd  0000 0000 0000
    //    Rd=0: word = 0xE10F0000
    {
        "arm_mrs_cpsr",
        false, 0x100u,
        0xE10F0000u,
        {0, 0, 0, 0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        // Seed CPSR with a known pattern: N=1, Z=0, C=1, V=0, mode=SVC.
        /* cpsr */ (1u<<31) | (1u<<29) | 0x13u,
        nullptr, 0,
        false, 0, 0,
    },

    // ── Conditional execution: ADDEQ r0, r0, #1 with Z=1 ────────────
    //    Encoding: 0x02800001 (EQ + ADD imm)
    {
        "arm_addeq_taken",
        false, 0x100u,
        0x02800001u,
        {5, 0, 0, 0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        /* cpsr */ (1u<<30) | MODE_SYSTEM,   // Z=1
        nullptr, 0,
        false, 0, 0,
    },

    // ── Conditional execution: ADDEQ r0, r0, #1 with Z=0 (not taken)
    {
        "arm_addeq_notaken",
        false, 0x100u,
        0x02800001u,
        {5, 0, 0, 0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        /* cpsr */ MODE_SYSTEM,
        nullptr, 0,
        false, 0, 0,
    },

    // ── B forward — branch test that exercises runtime_dispatch
    //    via the stub. B at 0x100 with imm24 = 0 → target = 0x100+8.
    //    Encoding: 0xEA000000
    {
        "arm_b_forward",
        false, 0x100u,
        0xEA000000u,
        {0, 0, 0, 0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        MODE_SYSTEM,
        nullptr, 0,
        /*branches*/ true, 0, 0,
    },

    // ── BL: link register set to PC+4, branch target same as B.
    //    Encoding: 0xEB000000 at PC=0x100 → LR=0x104, target=0x108.
    {
        "arm_bl",
        false, 0x100u,
        0xEB000000u,
        {0, 0, 0, 0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        MODE_SYSTEM,
        nullptr, 0,
        true, 0, 0,
    },

    // ── BX rN — interworking. rN's bit 0 selects THUMB.
    //    Encoding: cond 0001 0010 1111 1111 1111 0001 Rm
    //    BX r0: word = 0xE12FFF10
    {
        "arm_blnv_not_taken",
        false, 0x100u,
        0xFB000000u,  // BLNV is never executed on ARMv4T.
        {0, 0, 0, 0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        MODE_SYSTEM,
        nullptr, 0,
        false, 0, 0,
    },

    {
        "arm_bx_to_arm",
        false, 0x100u,
        0xE12FFF10u,
        {0x300u, 0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        MODE_SYSTEM,
        nullptr, 0,
        true, 0, 0,
    },

    // ── BX rN with bit 0 set → switches to THUMB.
    {
        "arm_bx_to_thumb",
        false, 0x100u,
        0xE12FFF10u,
        {0x301u, 0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        MODE_SYSTEM,
        nullptr, 0,
        true, 0, 0,
    },

    {
        "arm_bx_lr_to_thumb_sets_t",
        false, 0x100u,
        0xE12FFF1Eu,
        {0, 0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0x301u, 0x100u},
        MODE_SYSTEM,
        nullptr, 0,
        true, 0, 0,
    },

    {
        "thumb_bx_lr_to_arm_clears_t",
        true, 0x100u,
        0x00004770u,
        {0, 0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0x300u, 0x100u},
        (1u << 5) | MODE_SYSTEM,
        nullptr, 0,
        true, 0, 0,
    },

    // ── THUMB fmt3 MOV r0, #5 (0x2005) ──────────────────────────────
    //    THUMB CPSR.T must be set.
    {
        "thumb_mov_imm",
        /*thumb*/ true, /*pc*/ 0x100u,
        /*word*/ 0x00002005u,
        {0, 0, 0, 0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        /* cpsr */ (1u<<5) | MODE_SYSTEM,
        nullptr, 0,
        false, 0, 0,
    },

    // ── THUMB fmt3 ADD r0, #10 (0x300A) when r0=5 ──────────────────
    {
        "thumb_add_imm",
        true, 0x100u, 0x0000300Au,
        {5, 0, 0, 0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        (1u<<5) | MODE_SYSTEM,
        nullptr, 0,
        false, 0, 0,
    },

    // ── THUMB fmt9 STR r1, [r0, #0]  (0x6001) ───────────────────────
    {
        "thumb_str_imm",
        true, 0x100u, 0x00006001u,
        {0x1000u, 0x5555AAAAu, 0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        (1u<<5) | MODE_SYSTEM,
        nullptr, 0,
        false, 0, 0,
    },

    // ── THUMB fmt9 LDR r2, [r0, #0]  (0x6802) ───────────────────────
    {
        "thumb_ldr_imm",
        true, 0x100u, 0x00006802u,
        {0x1000u, 0, 0, 0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
        (1u<<5) | MODE_SYSTEM,
        mem_word_at_1000_dead, 1,
        false, 0, 0,
    },

    // ── Expanded coverage: every DP imm op with S=0 ─────────────────
    //   Pattern: cond=AL, opc<<21, S=0, Rn=0, Rd=1, rotate=0, imm=0x0F
    //   word = 0xE3 [opc] 0 1 0 0 0 F
    //   AND→0, EOR→1, SUB→2, RSB→3, ADD→4, ADC→5, SBC→6, RSC→7
    //   TST→8, TEQ→9, CMP→A, CMN→B, ORR→C, MOV→D, BIC→E, MVN→F
    //   But TST/TEQ/CMP/CMN encodings require S=1; skip for the S=0 sweep.
    //   Pre: r0 = 0x0F00, carry-in C=1 for ADC/SBC/RSC.
    { "arm_and_imm",  false, 0x100u, 0xE200100Fu,  // AND r1, r0, #0x0F
      {0x0F00u,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      MODE_SYSTEM, nullptr, 0, false, 0, 0 },
    { "arm_eor_imm",  false, 0x100u, 0xE220100Fu,  // EOR r1, r0, #0x0F
      {0x0F00u,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      MODE_SYSTEM, nullptr, 0, false, 0, 0 },
    { "arm_rsb_imm",  false, 0x100u, 0xE260100Fu,  // RSB r1, r0, #0x0F
      {0x0F00u,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      MODE_SYSTEM, nullptr, 0, false, 0, 0 },
    { "arm_adc_imm",  false, 0x100u, 0xE2A0100Fu,  // ADC r1, r0, #0x0F
      {0x0F00u,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      (1u<<29) | MODE_SYSTEM,  // C=1
      nullptr, 0, false, 0, 0 },
    { "arm_sbc_imm",  false, 0x100u, 0xE2C0100Fu,  // SBC r1, r0, #0x0F
      {0x0F00u,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      (1u<<29) | MODE_SYSTEM, nullptr, 0, false, 0, 0 },
    { "arm_rsc_imm",  false, 0x100u, 0xE2E0100Fu,  // RSC r1, r0, #0x0F
      {0x0F00u,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      (1u<<29) | MODE_SYSTEM, nullptr, 0, false, 0, 0 },
    { "arm_orr_imm",  false, 0x100u, 0xE380100Fu,  // ORR r1, r0, #0x0F
      {0x0F00u,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      MODE_SYSTEM, nullptr, 0, false, 0, 0 },
    { "arm_bic_imm",  false, 0x100u, 0xE3C0100Fu,  // BIC r1, r0, #0x0F
      {0x0F0Fu,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      MODE_SYSTEM, nullptr, 0, false, 0, 0 },
    { "arm_mvn_imm",  false, 0x100u, 0xE3E0100Fu,  // MVN r1, #0x0F
      {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      MODE_SYSTEM, nullptr, 0, false, 0, 0 },

    // ── TST/TEQ/CMP/CMN with S=1 (always; these are "set flags only")
    //   TST: r0 & 0x0F when r0=0x0F → nonzero, Z=0
    { "arm_tst_imm_z0", false, 0x100u, 0xE310000Fu,  // TST r0, #0x0F
      {0x0F00u,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      MODE_SYSTEM, nullptr, 0, false, 0, 0 },
    { "arm_teq_imm_eq", false, 0x100u, 0xE330000Fu,  // TEQ r0, #0x0F
      {0x0Fu,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      MODE_SYSTEM, nullptr, 0, false, 0, 0 },
    { "arm_cmn_imm_overflow", false, 0x100u, 0xE3700001u,  // CMN r0, #1
      {0x7FFFFFFFu,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      MODE_SYSTEM, nullptr, 0, false, 0, 0 },

    // ── DP imm-rotate: MOV r0, #0xFF000000 via rot=4 imm=0xFF ───────
    //   cond=AL, opc=D (MOV), Rd=0, rot=4 (rot count = rot*2 = 8),
    //   imm8=0xFF → result = 0xFF rotr 8 = 0xFF000000.
    //   word = 0xE3A0 0 4 FF = 0xE3A0 04 FF → 0xE3A004FF
    { "arm_mov_imm_rotated", false, 0x100u, 0xE3A004FFu,
      {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      MODE_SYSTEM, nullptr, 0, false, 0, 0 },

    // ── DP register: ADD r0, r1, r2 (no shift) ──────────────────────
    //   cond=AL, opc=ADD=4, S=0, Rn=1, Rd=0, shifter = Rm=2 LSL #0
    //   word = 0xE081 0002 → 0xE0810002
    { "arm_add_reg", false, 0x100u, 0xE0810002u,
      {0, 5, 7, 0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      MODE_SYSTEM, nullptr, 0, false, 0, 0 },

    // ── DP register: SUB r0, r1, r2 ─────────────────────────────────
    { "arm_sub_reg", false, 0x100u, 0xE0410002u,  // SUB r0, r1, r2
      {0, 0x100u, 0x40u, 0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      MODE_SYSTEM, nullptr, 0, false, 0, 0 },

    // ── DP register with shifter: MOV r0, r1, ROR #4 ───────────────
    //   word: cond AL=E, 00 0, op=MOV=D, S=0, Rn=0, Rd=0,
    //         shift_imm=4, st=ROR=3, 0, Rm=1
    //   word = 0xE1A0 0 2 6 1 = 0xE1A00261
    { "arm_mov_ror_imm", false, 0x100u, 0xE1A00261u,
      {0, 0x12345678u, 0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      MODE_SYSTEM, nullptr, 0, false, 0, 0 },

    // ── DP register with RRX: MOV r0, r1, RRX ──────────────────────
    //   RRX = ROR #0. word: cond=E, 00 0 D 0 0 0 0 0 0 6 1
    //   imm=0, st=ROR=3, → 0xE1A00061. Carry-in = 1.
    { "arm_mov_rrx_carry1", false, 0x100u, 0xE1A00061u,
      {0, 0x12345678u, 0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      (1u<<29) | MODE_SYSTEM,  // C=1
      nullptr, 0, false, 0, 0 },

    // ── DP reg-shifted-by-register: LSR r2 form ────────────────────
    //   MOVS r1, r0, LSR r2 with r0=0x40, r2=2 → r1=0x10
    //   word: cond=E, 00 0 op=D, S=1, Rn=0, Rd=1, Rs=2, 0 st=LSR=01 1 Rm=0
    //   = 0xE1B0 1 2 3 0
    { "arm_movs_lsr_reg", false, 0x100u, 0xE1B01230u,
      {0x40u, 0, 2, 0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      MODE_SYSTEM, nullptr, 0, false, 0, 0 },

    // ── DP reg-shifted-by-register, count=0: carry unchanged ───────
    //   MOVS r1, r0, LSL r2 with r2=0 → r1=r0, carry preserved
    { "arm_movs_lsl_reg_zero", false, 0x100u, 0xE1B01210u,
      {0x12345678u, 0, 0, 0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      (1u<<29) | MODE_SYSTEM,  // C=1, must stay 1
      nullptr, 0, false, 0, 0 },

    // ── ADDS that overflows: 0x7FFFFFFF + 1 → V=1, N=1 ─────────────
    { "arm_adds_overflow", false, 0x100u, 0xE2901001u,  // ADDS r1, r0, #1
      {0x7FFFFFFFu, 0, 0, 0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      MODE_SYSTEM, nullptr, 0, false, 0, 0 },

    // ── SUBS with no borrow: C=1; with borrow: C=0 ─────────────────
    { "arm_subs_no_borrow", false, 0x100u, 0xE2501005u,  // SUBS r1, r0, #5
      {10, 0, 0, 0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      MODE_SYSTEM, nullptr, 0, false, 0, 0 },
    { "arm_subs_borrow", false, 0x100u, 0xE2501005u,
      {3, 0, 0, 0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      MODE_SYSTEM, nullptr, 0, false, 0, 0 },

    // ── LDR with register offset (LDR r2, [r0, r1, LSL #2]) ────────
    //   cond=AL, 011 P=1 U=1 B=0 W=0 L=1, Rn=0, Rd=2,
    //   imm=2 (=offset_shift_count), st=LSL=00, 0, Rm=1
    //   word = 0xE790 2101
    //
    //   But base+ offset must hit a memory init we seed. Use
    //   r0=0x1000, r1=1 → addr = 0x1000 + (1<<2) = 0x1004 → mem at 0x1004.
    { "arm_ldr_reg_offset_lsl", false, 0x100u, 0xE7902101u,
      {0x1000u, 1, 0, 0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      MODE_SYSTEM, mem_word_at_1004_cafe, 1, false, 0, 0 },

    // ── STRB with imm offset ──────────────────────────────────────
    { "arm_strb_imm", false, 0x100u, 0xE5C01001u,  // STRB r1, [r0, #1]
      {0x1000u, 0xABu, 0, 0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      MODE_SYSTEM, nullptr, 0, false, 0, 0 },

    // ── STRH with imm offset ──────────────────────────────────────
    //   STRH r1, [r0, #2]: cond=E, 000 P=1 U=1 1 W=0 0 Rn=0 Rd=1
    //                       imm_hi=0, 1 S=0 H=1 1 imm_lo=2
    //   word = 0xE1C0 10B2
    { "arm_strh_imm", false, 0x100u, 0xE1C010B2u,
      {0x1000u, 0x1234u, 0, 0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      MODE_SYSTEM, nullptr, 0, false, 0, 0 },

    // ── LDRH misaligned (odd addr) → rotated 8 bits ────────────────
    { "arm_ldrh_misaligned", false, 0x100u, 0xE1D020B1u,
      // LDRH r2, [r0, #1]: addr = 0x1001
      {0x1000u, 0, 0, 0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      MODE_SYSTEM, mem_word_at_1000_misaligned_pattern, 1, false, 0, 0 },

    // ── LDRSH on odd address falls back to LDRSB on the byte ──────
    { "arm_ldrsh_odd", false, 0x100u, 0xE1D020F1u,
      // LDRSH r2, [r0, #1]: addr = 0x1001
      {0x1000u, 0, 0, 0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      MODE_SYSTEM, mem_word_at_1000_misaligned_pattern, 1, false, 0, 0 },

    // ── LDR post-indexed: LDR r2, [r0], #4 ────────────────────────
    //   cond=AL, 010 P=0 U=1 B=0 W=0 L=1, Rn=0, Rd=2, imm12=4
    //   word = 0xE490 2004
    { "arm_ldr_post_inc", false, 0x100u, 0xE4902004u,
      {0x1000u, 0, 0, 0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      MODE_SYSTEM, mem_word_at_1000_dead, 1, false, 0, 0 },

    // ── LDM IB with writeback: LDMIB r0!, {r1-r4} ─────────────────
    //   cond=AL, 100 P=1 U=1 S=0 W=1 L=1, Rn=0, list=0x001E
    //   word = 0xE9B0 001E
    //   IB reads first from addr+4, ascending. Seed at 0x2004..0x2010.
    { "arm_ldmib_wb", false, 0x100u, 0xE9B0001Eu,
      // Base = 0x2000-4? IB advances first then reads, so first read at
      // base + 4 = 0x2000+4 = 0x2004. We have entries at 0x2000..0x200C
      // — read 4 starting at 0x2004 → 0x2004..0x2010. Add a fifth entry.
      {0x1FFCu, 0, 0, 0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      // base=0x1FFC, IB: first read at 0x2000 → 0x2000..0x200C → use the 4-entry block.
      MODE_SYSTEM, mem_block_ldm_pattern, 4, false, 0, 0 },

    // ── STMIA with writeback: STMIA r0!, {r1, r2} ─────────────────
    //   cond=AL, 100 P=0 U=1 S=0 W=1 L=0, Rn=0, list=0b110=0x6
    //   word = 0xE8A0 0006
    { "arm_stmia_wb", false, 0x100u, 0xE8A00006u,
      {0x4000u, 0x11111111u, 0x22222222u, 0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      MODE_SYSTEM, nullptr, 0, false, 0, 0 },

    // ── MULS: flags update ─────────────────────────────────────────
    //   MULS r0, r1, r2 with r1*r2=0 → Z=1
    //   cond=E, 0000 00 0 S=1 Rd=0 0000 Rs=2 1001 Rm=1
    //   word = 0xE010 0291
    { "arm_muls_zero", false, 0x100u, 0xE0100291u,
      {0, 5, 0, 0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      MODE_SYSTEM, nullptr, 0, false, 0, 0 },

    // ── SMULL: signed multiplication ──────────────────────────────
    //   SMULL r0, r1, r2, r3 — RdLo=0, RdHi=1, Rs=3, Rm=2
    //   cond=E, 0000 11 0 S=0 RdHi=1 RdLo=0 Rs=3 1001 Rm=2
    //   word = 0xE0C1 0392
    //   r2 = -1 (0xFFFFFFFF), r3 = 5 → (int64)-5 = 0xFFFFFFFFFFFFFFFB
    //   RdLo=0xFFFFFFFB, RdHi=0xFFFFFFFF
    { "arm_smull_negative", false, 0x100u, 0xE0C10392u,
      {0, 0, 0xFFFFFFFFu, 5, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      MODE_SYSTEM, nullptr, 0, false, 0, 0 },

    // ── SWPB: byte swap (no rotate) ───────────────────────────────
    //   SWPB r2, r1, [r0]: word = 0xE140 2091
    //   cond=E, 0001 0 1 0 0 Rn Rd 0000 1001 Rm → B=1: 0xE140 2091
    { "arm_swpb", false, 0x100u, 0xE1402091u,
      {0x3000u, 0xCCu, 0, 0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      MODE_SYSTEM, mem_swp_init, 1, false, 0, 0 },

    // ── MSR CPSR_f, r0 (flags-only update) ─────────────────────────
    //   cond=E, 0001 0 010 1000 1111 0000 0000 Rm=0
    //   bit 22 = 0 (CPSR), bits 19-16 = mask=8 (flags byte only)
    //   word = 0xE128 F000
    { "arm_msr_cpsr_flags", false, 0x100u, 0xE128F000u,
      // r0 = top 4 flags set + System mode bits (which we mask out below).
      {0xF000001Fu, 0, 0, 0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      MODE_SYSTEM, nullptr, 0, false, 0, 0 },

    // ── B backward: 0xEAFFFFFE = B self ───────────────────────────
    { "arm_b_backward", false, 0x110u, 0xEAFFFFFEu,
      {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x110u},
      MODE_SYSTEM, nullptr, 0, /*branches*/ true, 0, 0 },

    // ── Conditional B not taken (BNE with Z=1) ────────────────────
    //   word = 0x1A 000000 (cond=NE=0001, B, imm=0)
    { "arm_bne_not_taken", false, 0x100u, 0x1A000000u,
      {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      (1u<<30) | MODE_SYSTEM,  // Z=1 → NE false
      // Despite cond failing, recomp leaves R[15] as initial; interp
      // advances by 4. To compare cleanly we mark as non-branching.
      nullptr, 0, /*branches*/ false, 0, 0 },

    // ── THUMB fmt2 ADD reg ────────────────────────────────────────
    //   ADD r0, r0, r1 (THUMB fmt2): 0x1840
    //   r0+=r1
    { "thumb_add_reg", true, 0x100u, 0x00001840u,
      {3, 4, 0, 0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      (1u<<5) | MODE_SYSTEM, nullptr, 0, false, 0, 0 },

    // THUMB high-register MOV PC, Rm stays in THUMB and clears bit 0.
    { "thumb_mov_pc_clears_bit0", true, 0x100u, 0x00004687u,
      {0x201u, 0, 0, 0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      (1u<<5) | MODE_SYSTEM, nullptr, 0, true, 0, 0 },

    // ── THUMB fmt3 CMP imm (CMP r0, #5) ───────────────────────────
    //   word = 0x2805
    { "thumb_cmp_imm", true, 0x100u, 0x00002805u,
      {5, 0, 0, 0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      (1u<<5) | MODE_SYSTEM, nullptr, 0, false, 0, 0 },

    // ── THUMB fmt6 LDR PC-rel: LDR r0, [PC, #0]  (0x4800) ──────────
    //   PC at decode time = 0x100+4 = 0x104, aligned & ~3 = 0x104.
    //   Imm-offset is 0 words → addr = 0x104. Memory must have a
    //   word there.
    { "thumb_ldr_pc_rel", true, 0x100u, 0x00004800u,
      {0, 0, 0, 0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      (1u<<5) | MODE_SYSTEM, mem_word_at_1004_cafe, 1, false, 0, 0 },

    // ── THUMB fmt14 PUSH {r4, r5, lr}  (0xB530) ───────────────────
    { "thumb_push", true, 0x100u, 0x0000B530u,
      // sp starts at 0x3000 (above mem block range to avoid collision)
      {0, 0, 0, 0, 0x1000u, 0x2000u, 0, 0, 0,0,0,0, 0,0x3000u,0xCAFEu, 0x100u},
      (1u<<5) | MODE_SYSTEM, nullptr, 0, false, 0, 0 },

    // THUMB empty STM writes the aligned PC-store value and writebacks by 0x40.
    { "thumb_stmia_empty_wb_pc_store_align", true, 0x102u, 0x0000C000u,
      {0x2000u, 0, 0, 0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x102u},
      (1u<<5) | MODE_SYSTEM, nullptr, 0, false, 0, 0 },

    // ── THUMB fmt19 BL pair: BL +4 (two halfwords) ────────────────
    //   We test only the prefix half here; the suffix is a separate test.
    //   prefix: 0xF000 (offset hi=0) — sets LR for the next.
    //   We can't fully test BL semantics with a single instruction;
    //   verify the prefix updates LR correctly to (PC+4) + 0 = 0x104+0.
    //   Actually THUMB BL prefix is: LR = PC+4 + (sext(imm11) << 12).
    //   With imm11=0, LR = 0x104.
    { "thumb_bl_prefix", true, 0x100u, 0x0000F000u,
      {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      (1u<<5) | MODE_SYSTEM, nullptr, 0, false, 0, 0 },

    // ── THUMB fmt16 conditional branch (BEQ taken / not taken) ────
    //   BEQ +4 at pc=0x100: word = 0xD001 (cond=EQ=0, offset8 = 1)
    //   target = pc + 4 + (1<<1) = 0x106
    { "thumb_beq_taken", true, 0x100u, 0x0000D001u,
      {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      (1u<<5) | (1u<<30) | MODE_SYSTEM,  // T=1, Z=1
      nullptr, 0, /*branches*/ true, 0, 0 },

    { "thumb_beq_not_taken", true, 0x100u, 0x0000D001u,
      {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x100u},
      (1u<<5) | MODE_SYSTEM,  // T=1, Z=0
      nullptr, 0, /*branches*/ false, 0, 0 },
};

const std::size_t kTestCasesCount =
    sizeof(kTestCases) / sizeof(kTestCases[0]);
