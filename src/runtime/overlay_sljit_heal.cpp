// overlay_sljit_heal.cpp — see overlay_sljit_heal.h.

#include "overlay_sljit_heal.h"

#include <vector>

#include "arm_decode.h"       // armv4t::ArmDecoder
#include "arm_ir.h"           // armv4t::Instr
#include "arm_sljit.h"        // armv4t::emit_function_sljit, sljit_supports
#include "function_finder.h"  // gbarecomp::FunctionFinder
#include "thumb_decode.h"     // armv4t::ThumbDecoder

namespace gbarecomp {

namespace {

uint32_t read_u32(const uint8_t* b, std::size_t size, uint32_t base, uint32_t a) {
    const uint32_t off = a - base;
    if (a < base || off + 4u > size) return 0u;
    return static_cast<uint32_t>(b[off]) |
           (static_cast<uint32_t>(b[off + 1]) << 8) |
           (static_cast<uint32_t>(b[off + 2]) << 16) |
           (static_cast<uint32_t>(b[off + 3]) << 24);
}
uint16_t read_u16(const uint8_t* b, std::size_t size, uint32_t base, uint32_t a) {
    const uint32_t off = a - base;
    if (a < base || off + 2u > size) return 0u;
    return static_cast<uint16_t>(static_cast<uint16_t>(b[off]) |
                                 (static_cast<uint16_t>(b[off + 1]) << 8));
}

// Discover the function rooted at (pc, thumb) against the live code image and
// decode its extent into `prog` (single mode across [addr, end_addr) — inter-
// working exits via BX/dispatch). Returns false if undiscoverable / empty.
bool decode_function(uint32_t pc, bool thumb, const uint8_t* bytes,
                     std::size_t size, uint32_t base,
                     std::vector<armv4t::Instr>& prog, uint32_t* out_end) {
    FunctionFinder ff(bytes, size, base);
    FunctionSeed seed;
    seed.addr = pc;
    seed.mode = thumb ? CpuMode::Thumb : CpuMode::Arm;
    ff.add_seed(seed);
    ff.run(/*max_functions=*/256);

    const Function* target = nullptr;
    for (const auto& fn : ff.functions()) {
        if (fn.addr == pc && (fn.mode == CpuMode::Thumb) == thumb) {
            target = &fn;
            break;
        }
    }
    if (!target || target->end_addr <= target->addr) return false;

    for (uint32_t a = target->addr; a < target->end_addr;) {
        if (thumb) {
            prog.push_back(armv4t::ThumbDecoder::decode(read_u16(bytes, size, base, a), a));
            a += 2u;
        } else {
            prog.push_back(armv4t::ArmDecoder::decode(read_u32(bytes, size, base, a), a));
            a += 4u;
        }
    }
    *out_end = target->end_addr;
    return !prog.empty();
}

}  // namespace

bool overlay_sljit_produce(uint32_t pc, bool thumb,
                           const uint8_t* bytes, std::size_t size, uint32_t base,
                           void (**out_fn)(void), void** out_code,
                           uint32_t* out_end, bool* out_leaf) {
    std::vector<armv4t::Instr> prog;
    uint32_t end = 0;
    if (!decode_function(pc, thumb, bytes, size, base, prog, &end)) return false;

    armv4t::SljitFn fn = armv4t::emit_function_sljit(
        prog.data(), static_cast<unsigned>(prog.size()));
    if (!fn.fn) return false;  // emitter declined (unsupported op in the extent)

    *out_fn = fn.fn;
    *out_code = fn.code;
    *out_end = end;
    if (out_leaf) {
        // Leaf = no outgoing call (BL/BLX). Such a function returns via bx lr /
        // pop {pc} / mov pc,lr and never re-enters dispatch from its body, so the
        // P6 gate can shadow-validate it in isolation.
        bool leaf = true;
        for (const auto& in : prog)
            if (in.is_call) { leaf = false; break; }
        *out_leaf = leaf;
    }
    return true;
}

bool overlay_sljit_function_supported(uint32_t pc, bool thumb,
                                      const uint8_t* bytes, std::size_t size,
                                      uint32_t base) {
    std::vector<armv4t::Instr> prog;
    uint32_t end = 0;
    if (!decode_function(pc, thumb, bytes, size, base, prog, &end)) return false;
    for (const auto& in : prog)
        if (!armv4t::sljit_supports(in)) return false;
    return true;
}

}  // namespace gbarecomp
