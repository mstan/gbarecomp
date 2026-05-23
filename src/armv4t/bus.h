// bus.h — Portable memory-bus interface used by the IR interpreter
// and by codegen-emitted helpers.
//
// This lives in the armv4t/ layer so the interpreter can stay
// platform-portable (no gba/ dependency). The concrete GBA bus
// (`gba::MemoryBus`) inherits from this — see `src/gba/gba_memory.h`.
//
// The interface deliberately exposes only the read/write primitives.
// Unmapped-access logging, region classification, waitstate timing,
// IO-side-effects — all of that lives in concrete implementations.

#pragma once

#include <cstdint>

namespace armv4t {

struct Bus {
    virtual ~Bus() = default;

    virtual uint8_t  read8 (uint32_t addr) = 0;
    virtual uint16_t read16(uint32_t addr) = 0;
    virtual uint32_t read32(uint32_t addr) = 0;

    virtual void write8 (uint32_t addr, uint8_t  v) = 0;
    virtual void write16(uint32_t addr, uint16_t v) = 0;
    virtual void write32(uint32_t addr, uint32_t v) = 0;
};

}  // namespace armv4t
