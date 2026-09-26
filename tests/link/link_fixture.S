/* Original minimal multi-cart link fixture. No BIOS or cartridge assets.
 * Harness supplies r0 = outgoing word, r1 = baud (0..3), r8 = startup delay.
 * r8=0 exercises immediate start; 4096 permits startup mode propagation. */
.syntax unified
.cpu arm7tdmi
.arm
.section .text
.global _start
_start:
    cmp r9, #0
    bne normal
    mov r4, #0x04000000
    add r4, r4, #0x100
    mov r2, #0
    strh r2, [r4, #0x34]       /* RCNT: serial */
    strh r0, [r4, #0x2a]       /* data before ready */
    orr r2, r1, #0x6000        /* MULTI + serial IRQ */
    strh r2, [r4, #0x28]
ready:
    ldrh r3, [r4, #0x28]
    tst r3, #8
    beq ready
settle:
    cmp r8, #0
    beq settled
    sub r8, r8, #1
    b settle
settled:
    tst r3, #4                /* hardware slave bit */
    bne complete
    orr r2, r2, #0x80
    strh r2, [r4, #0x28]
complete:
    add r5, r4, #0x100
    ldrh r3, [r5, #2]         /* IF; IME stays disabled */
    tst r3, #0x80
    beq complete
    mov r6, #0x02000000
    ldr r7, [r4, #0x20]
    str r7, [r6]
    ldr r7, [r4, #0x24]
    str r7, [r6, #4]
    ldrh r7, [r4, #0x28]
    str r7, [r6, #8]
    str r3, [r6, #12]
    mov r7, #0x42
    str r7, [r6, #16]
done:
    b done

/* Normal serial over a physical multiplayer cable: data flows forward.
 * r1: bit 0 selects fast clock, bit 1 selects 32-bit; r9 selects this path. */
normal:
    mov r4, #0x04000000
    add r4, r4, #0x100
    mov r2, #0
    strh r2, [r4, #0x34]
    and r2, r1, #1
    mov r2, r2, lsl #1
    orr r2, r2, #0x4000
    tst r1, #2
    orrne r2, r2, #0x1000
    strh r2, [r4, #0x28]
    str r0, [r4, #0x20]
    strh r0, [r4, #0x2a]
    tst r0, #0x2000
    orrne r2, r2, #0x80
    orreq r2, r2, #1
    strh r2, [r4, #0x28]
normal_settle:
    subs r8, r8, #1
    bne normal_settle
    tst r0, #0x2000
    orreq r2, r2, #0x80
    strheq r2, [r4, #0x28]
normal_wait:
    add r5, r4, #0x100
    ldrh r3, [r5, #2]
    tst r3, #0x80
    beq normal_wait
    mov r6, #0x02000000
    tst r1, #2
    ldrne r7, [r4, #0x20]
    ldrbeq r7, [r4, #0x2a]
    str r7, [r6]
    mov r7, #0
    str r7, [r6, #4]
    ldrh r7, [r4, #0x28]
    bic r7, r7, #4           /* pin SI is outside this oracle comparison */
    str r7, [r6, #8]
    str r3, [r6, #12]
    mov r7, #0x42
    str r7, [r6, #16]
    b done
