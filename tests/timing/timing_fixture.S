/* Original CPU timing fixture. No BIOS or cartridge assets.
 *
 * Measures, with free-running timer 0 at F/1, the cycles of small ARM and
 * THUMB kernels executing from cartridge ROM, IWRAM and EWRAM under several
 * WAITCNT / EWRAM-control settings. Every kernel is position independent and
 * runs from all three places: the driver copies the kernel blob to IWRAM and
 * EWRAM (the recompiler maps the copies with [[code_copy]]).
 *
 * A kernel reads TM0CNT_L (via r12, or r2 in THUMB) before and after its
 * workload and returns the 16-bit difference in r0: the cycles from the
 * start of the first timer read to the start of the second. The driver
 * stores one word per (setting, region, kernel) at 02020000h and writes the
 * marker 0x600D to 0201FFF0h when done. Kernels may clobber r0-r5 and r7;
 * the driver keeps its state in r6 and r8-r12.
 *
 * VRAM/palette/OAM are never touched (mGBA models PPU VRAM contention that
 * the native bus deliberately does not; display is forced blank anyway), and
 * no LDM/STM reads or writes the cartridge space (see docs/CPU_TIMING.md).
 */
.syntax unified
.cpu arm7tdmi
.section .text

.arm
.global _start
_start:
    mov r0, #0x04000000
    mov r1, #0x80
    strh r1, [r0]               /* DISPCNT: forced blank */
    mov r1, #0
    str r1, [r0, #0x208]        /* IME = 0 */
    ldr sp, =0x03007F00
    ldr r1, =blob_start
    ldr r2, =blob_end
    mov r3, #0x03000000
    mov r4, #0x02000000
copy:
    ldr r5, [r1], #4
    str r5, [r3], #4
    str r5, [r4], #4
    cmp r1, r2
    blo copy
    ldr r12, =0x04000100
    mov r1, #0
    strh r1, [r12]              /* TM0 reload 0 */
    mov r1, #0x80
    strh r1, [r12, #2]          /* TM0 enable, F/1 */
    ldr r11, =0x02020000
    ldr r8, =pass_table
pass:
    ldr r0, [r8], #4            /* WAITCNT */
    cmn r0, #1
    beq finish
    ldr r1, [r8], #4            /* internal memory control */
    mov r2, #0x04000000
    add r2, r2, #0x200
    strh r0, [r2, #4]           /* 04000204h WAITCNT */
    add r2, r2, #0x600
    str r1, [r2]                /* 04000800h */
    ldr r9, =region_table
region:
    ldr r6, [r9], #4
    cmp r6, #0
    beq pass
    ldr r10, =kernel_table
kernel:
    ldr r7, [r10], #4
    cmn r7, #1
    beq region
    add r7, r7, r6
    mov lr, pc
    bx r7
    str r0, [r11], #4
    b kernel
finish:
    ldr r0, =0x600D
    ldr r1, =0x0201FFF0
    str r0, [r1]
done:
    b done
.ltorg

.align 2
pass_table:
    .word 0x0000, 0x0D000020    /* power-on: ROM 4/2, SRAM 4, no prefetch */
    .word 0x4317, 0x0D000020    /* ROM 3/1, SRAM 8, prefetch */
    .word 0x0317, 0x0D000020    /* ROM 3/1, SRAM 8, no prefetch */
    .word 0x4018, 0x0D000020    /* ROM 2/1, SRAM 4, prefetch */
    .word 0x4000, 0x0E000020    /* ROM 4/2, prefetch, EWRAM 1 wait */
    .word 0xFFFFFFFF
region_table:
    .word blob_start, 0x03000000, 0x02000000, 0
kernel_table:
    .word a_alu - blob_start
    .word a_loop - blob_start
    .word a_ldr_iwram - blob_start
    .word a_ldr_ewram - blob_start
    .word a_ldr_rom - blob_start
    .word a_ldrh_io - blob_start
    .word a_ldrb_sram - blob_start
    .word a_str_iwram - blob_start
    .word a_strh_ewram - blob_start
    .word a_ldm_stm_iwram - blob_start
    .word a_mul_small - blob_start
    .word a_mul_large - blob_start
    .word a_umull_smlal - blob_start
    .word a_swp - blob_start
    .word a_bl - blob_start
    .word a_bx_thumb - blob_start
    .word a_cond_fail - blob_start
    .word a_alu_pc - blob_start
    .word a_ldr_iwram_mul - blob_start
    .word t_alu - blob_start + 1
    .word t_loop - blob_start + 1
    .word t_ldr_iwram - blob_start + 1
    .word t_ldr_ewram - blob_start + 1
    .word t_ldr_rom - blob_start + 1
    .word t_ldrh_io - blob_start + 1
    .word t_str_iwram - blob_start + 1
    .word t_push_pop - blob_start + 1
    .word t_mul - blob_start + 1
    .word t_bl - blob_start + 1
    .word t_bl_pop_pc - blob_start + 1
    .word t_bx_arm - blob_start + 1
    .word t_ldr_pcrel - blob_start + 1
    .word t_cond_branch - blob_start + 1
    .word 0xFFFFFFFF

/* ─────────────────────────── kernel blob ─────────────────────────── */
.align 4
.global blob_start
blob_start:

.arm
a_alu:
    ldrh r1, [r12]
    .rept 8
    add r3, r3, #1
    .endr
    ldrh r2, [r12]
    b a_out

a_loop:
    mov r3, #8
    ldrh r1, [r12]
1:  subs r3, r3, #1
    bne 1b
    ldrh r2, [r12]
    b a_out

a_ldr_iwram:
    mov r4, #0x03000000
    add r4, r4, #0x7000
    ldrh r1, [r12]
    .rept 4
    ldr r3, [r4]
    .endr
    ldrh r2, [r12]
    b a_out

a_ldr_ewram:
    mov r4, #0x02000000
    add r4, r4, #0x30000
    ldrh r1, [r12]
    .rept 4
    ldr r3, [r4]
    .endr
    ldrh r2, [r12]
    b a_out

a_ldr_rom:
    mov r4, #0x08000000
    ldrh r1, [r12]
    ldr r3, [r4]
    ldr r3, [r4, #4]
    ldrh r3, [r4, #8]
    ldrb r3, [r4, #9]
    ldrh r2, [r12]
    b a_out

a_ldrh_io:
    mov r4, #0x04000000
    ldrh r1, [r12]
    .rept 4
    ldrh r3, [r4, #4]
    .endr
    ldrh r2, [r12]
    b a_out

a_ldrb_sram:
    mov r4, #0x0E000000
    ldrh r1, [r12]
    .rept 4
    ldrb r3, [r4]
    .endr
    ldrh r2, [r12]
    b a_out

a_str_iwram:
    mov r4, #0x03000000
    add r4, r4, #0x7000
    ldrh r1, [r12]
    .rept 4
    str r3, [r4]
    .endr
    ldrh r2, [r12]
    b a_out

a_strh_ewram:
    mov r4, #0x02000000
    add r4, r4, #0x30000
    ldrh r1, [r12]
    .rept 4
    strh r3, [r4]
    .endr
    ldrh r2, [r12]
    b a_out

a_ldm_stm_iwram:
    mov r4, #0x03000000
    add r4, r4, #0x7000
    ldrh r1, [r12]
    ldmia r4, {r0, r3, r5, r7}
    stmia r4, {r0, r3, r5, r7}
    ldmia r4, {r3}
    stmia r4, {r0, r3}
    ldrh r2, [r12]
    b a_out

a_mul_small:
    mov r5, #3
    mov r7, #0x55
    ldrh r1, [r12]
    .rept 4
    mul r3, r5, r7              /* Rs=r7: one m-cycle */
    .endr
    ldrh r2, [r12]
    b a_out

a_mul_large:
    ldr r5, =0x12345678
    ldr r7, =0x76543210
    ldrh r1, [r12]
    .rept 2
    mul r3, r5, r7              /* Rs=r7: four m-cycles */
    mla r3, r5, r7, r3
    .endr
    ldrh r2, [r12]
    b a_out

a_umull_smlal:
    ldr r5, =0x12345678
    mov r7, #0x3400
    ldrh r1, [r12]
    umull r3, r0, r5, r7
    smlal r3, r0, r5, r7
    umlal r3, r0, r7, r5
    smull r3, r0, r7, r5
    ldrh r2, [r12]
    b a_out

a_swp:
    mov r4, #0x03000000
    add r4, r4, #0x7000
    ldrh r1, [r12]
    swp r3, r5, [r4]
    swpb r3, r5, [r4]
    ldrh r2, [r12]
    b a_out

a_bl:
    mov r5, lr
    ldrh r1, [r12]
    .rept 4
    bl a_leaf
    .endr
    ldrh r2, [r12]
    mov lr, r5
    b a_out
a_leaf:
    bx lr

a_bx_thumb:
    mov r5, lr
    ldr r4, a_bx_thumb_off
    add r4, r4, r6              /* r6: this copy's blob base (driver) */
    ldrh r1, [r12]
    .rept 2
    mov lr, pc
    bx r4
    .endr
    ldrh r2, [r12]
    mov lr, r5
    b a_out

a_cond_fail:
    cmp r3, r3                  /* Z=1 */
    ldrh r1, [r12]
    .rept 8
    addne r3, r3, #1
    .endr
    ldrh r2, [r12]
    b a_out

a_alu_pc:
    ldrh r1, [r12]
    .rept 3
    add pc, pc, #0              /* ALU PC write: skips the next word */
    mov r3, r3
    .endr
    ldrh r2, [r12]
    b a_out

a_ldr_iwram_mul:
    mov r4, #0x03000000
    add r4, r4, #0x7000
    ldr r5, =0x00FFFFFF
    ldrh r1, [r12]
    ldr r3, [r4]
    mul r3, r5, r5
    ldr r3, [r4]
    add r3, r3, #1
    str r3, [r4]
    mul r3, r5, r5
    ldrh r2, [r12]
    b a_out

a_out:
    sub r0, r2, r1
    mov r0, r0, lsl #16
    mov r0, r0, lsr #16
    bx lr
.ltorg

.thumb
.align 1
t_leaf:
    bx lr

.align 1
t_alu:
    mov r2, r12
    ldrh r1, [r2]
    .rept 8
    adds r3, #1
    .endr
    ldrh r0, [r2]
    b t_out

t_loop:
    mov r2, r12
    movs r3, #8
    ldrh r1, [r2]
1:  subs r3, #1
    bne 1b
    ldrh r0, [r2]
    b t_out

t_ldr_iwram:
    mov r2, r12
    ldr r4, =0x03007000
    ldrh r1, [r2]
    .rept 4
    ldr r3, [r4]
    .endr
    ldrh r0, [r2]
    b t_out

t_ldr_ewram:
    mov r2, r12
    ldr r4, =0x02030000
    ldrh r1, [r2]
    .rept 4
    ldr r3, [r4]
    .endr
    ldrh r0, [r2]
    b t_out

t_ldr_rom:
    mov r2, r12
    ldr r4, =0x08000000
    ldrh r1, [r2]
    ldr r3, [r4]
    ldrh r3, [r4, #2]
    ldr r3, [r4, #4]
    ldrb r3, [r4, #9]
    ldrh r0, [r2]
    b t_out

t_ldrh_io:
    mov r2, r12
    ldr r4, =0x04000004
    ldrh r1, [r2]
    .rept 4
    ldrh r3, [r4]
    .endr
    ldrh r0, [r2]
    b t_out

t_str_iwram:
    mov r2, r12
    ldr r4, =0x03007000
    ldrh r1, [r2]
    .rept 4
    str r3, [r4]
    .endr
    ldrh r0, [r2]
    b t_out

t_push_pop:
    mov r2, r12
    ldrh r1, [r2]
    .rept 2
    push {r3, r4, r5, r7}
    pop {r3, r4, r5, r7}
    .endr
    ldrh r0, [r2]
    b t_out

t_mul:
    mov r2, r12
    ldr r5, =0x00012345
    movs r3, #7
    ldrh r1, [r2]
    .rept 4
    muls r3, r5
    .endr
    ldrh r0, [r2]
    b t_out

t_bl:
    mov r2, r12
    mov r5, lr
    ldrh r1, [r2]
    .rept 4
    bl t_leaf
    .endr
    ldrh r0, [r2]
    mov lr, r5
    b t_out

t_bl_pop_pc:
    mov r2, r12
    mov r5, lr
    ldrh r1, [r2]
    .rept 2
    bl t_pop_leaf
    .endr
    ldrh r0, [r2]
    mov lr, r5
    b t_out
t_pop_leaf:
    push {lr}
    pop {pc}

t_bx_arm:
    mov r2, r12
    mov r5, lr
    ldr r4, =a_leaf - blob_start
    adds r4, r4, r6             /* r6: this copy's blob base (driver) */
    ldrh r1, [r2]
    .rept 2
    bl t_call_r4
    .endr
    ldrh r0, [r2]
    mov lr, r5
    b t_out
t_call_r4:
    bx r4                       /* ARM a_leaf returns with bx lr (THUMB lr) */

t_ldr_pcrel:
    mov r2, r12
    ldrh r1, [r2]
    .rept 4
    ldr r3, t_pool_word
    .endr
    ldrh r0, [r2]
    b t_out

t_cond_branch:
    mov r2, r12
    movs r3, #0
    ldrh r1, [r2]
    .rept 4
    cmp r3, #0
    beq 2f                      /* taken */
    adds r3, #1
2:  cmp r3, #1
    beq 3f                      /* not taken */
3:
    .endr
    ldrh r0, [r2]
    b t_out

t_out:
    subs r0, r0, r1
    lsls r0, r0, #16
    lsrs r0, r0, #16
    bx lr

.align 2
t_pool_word:
    .word 0x5A5A5A5A
.ltorg
a_bx_thumb_off:
    .word t_leaf - blob_start + 1

.align 4
.global blob_end
blob_end:
