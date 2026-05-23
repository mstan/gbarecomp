// gba_irq.h — STUB. IRQ controller.
//
// Owns IE / IF / IME and the BIOS-mediated IRQ vector. On hardware,
// when IME=1 and (IE & IF) != 0, the CPU takes an IRQ at the next
// instruction boundary: CPSR → SPSR_irq, mode → IRQ, T → 0, I → 1,
// LR_irq → return_address + 4, PC → 0x18 (BIOS IRQ vector). The
// real implementation interacts with the scheduler so devices can
// pend bits.
//
// Reference: GBATEK § "GBA Interrupt Control" and ARM ARM ARMv4T A2.6.

#pragma once

namespace gba {

class GbaIrq {
public:
    GbaIrq();
    ~GbaIrq();
};

}  // namespace gba
