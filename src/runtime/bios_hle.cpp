// bios_hle.cpp — opt-in High-Level Emulation of GBA BIOS SWI calls.
//
// See bios_hle.h for the design (opt-in, LLE stays the oracle, unimplemented
// SWIs fall through to the recompiled BIOS). This file services a SWI by
// computing its effect directly on the recompiled register file (g_cpu) and
// guest memory (the bus_* bridge), then returning 1 so runtime_swi resumes the
// caller at LR without an SVC-mode entry or BIOS dispatch.
//
// SWI semantics are ported from mGBA's src/gba/bios.c (MPL-2.0, © Jeffrey Pfau,
// vendored under third_party/mgba) and GBATEK. See THIRD_PARTY_ATTRIBUTION.md.
//
// TIMING NOTE: the cycle cost charged per SWI is an approximation. The LLE
// (recompiled real BIOS) accrues the exact hardware cycle count as it executes;
// HLE cannot reproduce that bit-for-bit without re-deriving every stall. Where
// mGBA provides a closed-form stall (Div/Sqrt/ArcTan/LZ77) we port it; the rest
// charge a nominal cost. Cycle-exact HLE is a known limitation of HLE mode
// (opt-in); LLE remains the timing oracle.
//
// FLOAT NOTE: the affine SWIs (BgAffineSet/ObjAffineSet) and MidiKey2Freq use
// host float math (matching mGBA), which can differ from the real BIOS fixed
// point in the low bits — another reason LLE stays the oracle.

#include "bios_hle.h"

#include "runtime_arm.h"  // g_cpu, runtime_tick, bus_read_u*/bus_write_u*

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>  // div, abs

namespace gba {
namespace {

BiosHleMode g_mode = BiosHleMode::Off;

// ── register / memory shorthands ───────────────────────────────────────────
inline uint32_t  R(int i)               { return g_cpu.R[i]; }
inline void      setR(int i, uint32_t v){ g_cpu.R[i] = v; }
inline uint32_t  rd32(uint32_t a)       { return bus_read_u32(a); }
inline uint16_t  rd16(uint32_t a)       { return bus_read_u16(a); }
inline uint8_t   rd8 (uint32_t a)       { return bus_read_u8 (a); }
inline void      wr32(uint32_t a, uint32_t v){ bus_write_u32(a, v); }
inline void      wr16(uint32_t a, uint16_t v){ bus_write_u16(a, v); }
inline void      wr8 (uint32_t a, uint8_t  v){ bus_write_u8 (a, v); }

inline int clz32(uint32_t x) { return x ? __builtin_clz(x) : 32; }

constexpr float kPi = 3.14159265358979323846f;

// GBA SWI numbers (GBATEK).
enum : uint32_t {
    SWI_SOFT_RESET          = 0x00,
    SWI_REGISTER_RAM_RESET  = 0x01,
    SWI_HALT                = 0x02,
    SWI_STOP                = 0x03,
    SWI_INTR_WAIT           = 0x04,
    SWI_VBLANK_INTR_WAIT    = 0x05,
    SWI_DIV                 = 0x06,
    SWI_DIV_ARM             = 0x07,
    SWI_SQRT                = 0x08,
    SWI_ARCTAN              = 0x09,
    SWI_ARCTAN2             = 0x0A,
    SWI_CPU_SET             = 0x0B,
    SWI_CPU_FAST_SET        = 0x0C,
    SWI_GET_BIOS_CHECKSUM   = 0x0D,
    SWI_BG_AFFINE_SET       = 0x0E,
    SWI_OBJ_AFFINE_SET      = 0x0F,
    SWI_BIT_UNPACK          = 0x10,
    SWI_LZ77_UNCOMP_WRAM    = 0x11,
    SWI_LZ77_UNCOMP_VRAM    = 0x12,
    SWI_HUFFMAN_UNCOMP      = 0x13,
    SWI_RL_UNCOMP_WRAM      = 0x14,
    SWI_RL_UNCOMP_VRAM      = 0x15,
    SWI_DIFF_8BIT_UNFILTER_WRAM = 0x16,
    SWI_DIFF_8BIT_UNFILTER_VRAM = 0x17,
    SWI_DIFF_16BIT_UNFILTER = 0x18,
    SWI_SOUND_BIAS          = 0x19,
    SWI_MIDI_KEY_2_FREQ     = 0x1F,
};

constexpr uint32_t GBA_BIOS_CHECKSUM = 0xBAAE187Fu;
constexpr uint32_t SIZE_BIOS         = 0x4000u;
constexpr uint32_t REG_SOUNDBIAS     = 0x04000088u;

// ── arithmetic SWIs ─────────────────────────────────────────────────────────

int _mulWait(int32_t r) {
    if ((r & 0xFFFFFF00) == 0xFFFFFF00 || !(r & 0xFFFFFF00)) return 1;
    if ((r & 0xFFFF0000) == 0xFFFF0000 || !(r & 0xFFFF0000)) return 2;
    if ((r & 0xFF000000) == 0xFF000000 || !(r & 0xFF000000)) return 3;
    return 4;
}

// Div (num,denom) → r0=quot, r1=rem, r3=abs(quot). Returns the cycle stall.
uint32_t do_div(int32_t num, int32_t denom) {
    if (denom == 0) {
        setR(0, (num < 0) ? 0xFFFFFFFFu : 1u);
        setR(1, static_cast<uint32_t>(num));
        setR(3, 1u);
    } else if (denom == -1 && num == INT32_MIN) {
        setR(0, static_cast<uint32_t>(INT32_MIN));
        setR(1, 0u);
        setR(3, static_cast<uint32_t>(INT32_MIN));
    } else {
        div_t res = div(num, denom);
        setR(0, static_cast<uint32_t>(res.quot));
        setR(1, static_cast<uint32_t>(res.rem));
        setR(3, static_cast<uint32_t>(res.quot < 0 ? -res.quot : res.quot));
    }
    int loops = clz32(static_cast<uint32_t>(denom)) - clz32(static_cast<uint32_t>(num));
    if (loops < 1) loops = 1;
    return 4u + 13u * static_cast<uint32_t>(loops) + 7u;
}

int16_t _ArcTan(int32_t i, int32_t* r1, int32_t* r3, uint32_t* cycles) {
    uint32_t c = 37;
    c += _mulWait(i * i);
    int32_t a = -((i * i) >> 14);
    c += _mulWait(0xA9 * a);
    int32_t b = ((0xA9 * a) >> 14) + 0x390;
    c += _mulWait(b * a); b = ((b * a) >> 14) + 0x91C;
    c += _mulWait(b * a); b = ((b * a) >> 14) + 0xFB6;
    c += _mulWait(b * a); b = ((b * a) >> 14) + 0x16AA;
    c += _mulWait(b * a); b = ((b * a) >> 14) + 0x2081;
    c += _mulWait(b * a); b = ((b * a) >> 14) + 0x3651;
    c += _mulWait(b * a); b = ((b * a) >> 14) + 0xA2F9;
    if (r1) *r1 = a;
    if (r3) *r3 = b;
    *cycles = c;
    return static_cast<int16_t>((i * b) >> 16);
}

int16_t _ArcTan2(int32_t x, int32_t y, int32_t* r1, uint32_t* cycles) {
    if (!y) { *cycles = 11; return x >= 0 ? 0 : static_cast<int16_t>(0x8000); }
    if (!x) { *cycles = 11; return y >= 0 ? 0x4000 : static_cast<int16_t>(0xC000); }
    if (y >= 0) {
        if (x >= 0) {
            if (x >= y) return _ArcTan((y << 14) / x, r1, nullptr, cycles);
        } else if (-x >= y) {
            return static_cast<int16_t>(_ArcTan((y << 14) / x, r1, nullptr, cycles) + 0x8000);
        }
        return static_cast<int16_t>(0x4000 - _ArcTan((x << 14) / y, r1, nullptr, cycles));
    } else {
        if (x <= 0) {
            if (-x > -y) return static_cast<int16_t>(_ArcTan((y << 14) / x, r1, nullptr, cycles) + 0x8000);
        } else if (x >= -y) {
            return static_cast<int16_t>(_ArcTan((y << 14) / x, r1, nullptr, cycles) + 0x10000);
        }
        return static_cast<int16_t>(0xC000 - _ArcTan((x << 14) / y, r1, nullptr, cycles));
    }
}

int32_t _Sqrt(uint32_t x, uint32_t* cycles) {
    if (!x) { *cycles = 53; return 0; }
    int32_t c = 15;
    uint32_t upper = x, bound = 1, lower;
    while (bound < upper) { upper >>= 1; bound <<= 1; c += 6; }
    while (true) {
        c += 6;
        upper = x;
        uint32_t accum = 0;
        lower = bound;
        while (true) {
            c += 5;
            uint32_t oldLower = lower;
            if (lower <= upper >> 1) lower <<= 1;
            if (oldLower >= upper >> 1) break;
        }
        while (true) {
            c += 8;
            accum <<= 1;
            if (upper >= lower) { ++accum; upper -= lower; }
            if (lower == bound) break;
            lower >>= 1;
        }
        uint32_t oldBound = bound;
        bound += accum;
        bound >>= 1;
        if (bound >= oldBound) { bound = oldBound; break; }
    }
    *cycles = static_cast<uint32_t>(c);
    return static_cast<int32_t>(bound);
}

// ── memory-copy SWIs ────────────────────────────────────────────────────────

void do_cpu_set() {
    uint32_t src = R(0), dst = R(1), ctrl = R(2);
    uint32_t count = ctrl & 0x1FFFFFu;
    bool fixed = (ctrl >> 24) & 1u;
    bool w32   = (ctrl >> 26) & 1u;
    if (w32) {
        for (uint32_t i = 0; i < count; ++i) {
            wr32(dst, rd32(src));
            dst += 4; if (!fixed) src += 4;
        }
    } else {
        for (uint32_t i = 0; i < count; ++i) {
            wr16(dst, rd16(src));
            dst += 2; if (!fixed) src += 2;
        }
    }
}

void do_cpu_fast_set() {
    uint32_t src = R(0), dst = R(1), ctrl = R(2);
    uint32_t count = ctrl & 0x1FFFFFu;      // word count (games pass multiples of 8)
    bool fixed = (ctrl >> 24) & 1u;
    for (uint32_t i = 0; i < count; ++i) {
        wr32(dst, rd32(src));
        dst += 4; if (!fixed) src += 4;
    }
}

// ── affine SWIs (float, matches mGBA) ───────────────────────────────────────

void do_bg_affine_set() {
    uint32_t off = R(0), dest = R(1);
    int32_t i = static_cast<int32_t>(R(2));
    while (i-- > 0) {
        float ox = static_cast<int32_t>(rd32(off))      / 256.f;
        float oy = static_cast<int32_t>(rd32(off + 4))  / 256.f;
        float cx = static_cast<int16_t>(rd16(off + 8));
        float cy = static_cast<int16_t>(rd16(off + 10));
        float sx = static_cast<int16_t>(rd16(off + 12)) / 256.f;
        float sy = static_cast<int16_t>(rd16(off + 14)) / 256.f;
        float theta = (rd16(off + 16) >> 8) / 128.f * kPi;
        off += 20;
        float a, b, c, d;
        a = d = cosf(theta);
        b = c = sinf(theta);
        a *= sx; b *= -sx; c *= sy; d *= sy;
        float rx = ox - (a * cx + b * cy);
        float ry = oy - (c * cx + d * cy);
        wr16(dest,      static_cast<uint16_t>(static_cast<int32_t>(a * 256.f)));
        wr16(dest + 2,  static_cast<uint16_t>(static_cast<int32_t>(b * 256.f)));
        wr16(dest + 4,  static_cast<uint16_t>(static_cast<int32_t>(c * 256.f)));
        wr16(dest + 6,  static_cast<uint16_t>(static_cast<int32_t>(d * 256.f)));
        wr32(dest + 8,  static_cast<uint32_t>(static_cast<int32_t>(rx * 256.f)));
        wr32(dest + 12, static_cast<uint32_t>(static_cast<int32_t>(ry * 256.f)));
        dest += 16;
    }
}

void do_obj_affine_set() {
    uint32_t off = R(0), dest = R(1);
    int32_t i = static_cast<int32_t>(R(2));
    int32_t diff = static_cast<int32_t>(R(3));
    while (i-- > 0) {
        float sx = static_cast<int16_t>(rd16(off))     / 256.f;
        float sy = static_cast<int16_t>(rd16(off + 2)) / 256.f;
        float theta = (rd16(off + 4) >> 8) / 128.f * kPi;
        off += 8;
        float a, b, c, d;
        a = d = cosf(theta);
        b = c = sinf(theta);
        a *= sx; b *= -sx; c *= sy; d *= sy;
        wr16(dest,            static_cast<uint16_t>(static_cast<int32_t>(a * 256.f)));
        wr16(dest + diff,     static_cast<uint16_t>(static_cast<int32_t>(b * 256.f)));
        wr16(dest + diff * 2, static_cast<uint16_t>(static_cast<int32_t>(c * 256.f)));
        wr16(dest + diff * 3, static_cast<uint16_t>(static_cast<int32_t>(d * 256.f)));
        dest += diff * 4;
    }
}

// ── decompression SWIs ──────────────────────────────────────────────────────

uint32_t do_unLz77(int width) {
    uint32_t source = R(0), dest = R(1);
    uint32_t cycles = 20;
    int remaining = (rd32(source) & 0xFFFFFF00u) >> 8;
    int blockheader = 0;
    source += 4;
    int blocksRemaining = 0;
    uint32_t disp; int bytes, byte; int halfword = 0;
    while (remaining > 0) {
        cycles += 14;
        if (blocksRemaining) {
            cycles += 18;
            if (blockheader & 0x80) {
                int block = rd8(source + 1) | (rd8(source) << 8);
                source += 2;
                disp = dest - (block & 0x0FFF) - 1;
                bytes = (block >> 12) + 3;
                while (bytes--) {
                    cycles += 10;
                    if (remaining) --remaining;  // else improperly-compressed (overrun on HW)
                    if (width == 2) {
                        byte = static_cast<int16_t>(rd16(disp & ~1u));
                        byte >>= (disp & 1) * 8;
                        if (dest & 1) { halfword |= byte << 8; wr16(dest ^ 1, static_cast<uint16_t>(halfword)); }
                        else          { halfword = byte & 0xFF; }
                        cycles += 4;
                    } else {
                        byte = rd8(disp);
                        wr8(dest, static_cast<uint8_t>(byte));
                    }
                    ++disp; ++dest;
                }
            } else {
                byte = rd8(source); ++source;
                if (width == 2) {
                    if (dest & 1) { halfword |= byte << 8; wr16(dest ^ 1, static_cast<uint16_t>(halfword)); }
                    else          { halfword = byte; }
                } else {
                    wr8(dest, static_cast<uint8_t>(byte));
                }
                ++dest; --remaining;
            }
            blockheader <<= 1;
            --blocksRemaining;
        } else {
            blockheader = rd8(source); ++source;
            blocksRemaining = 8;
        }
    }
    setR(0, source); setR(1, dest); setR(3, 0);
    return cycles;
}

void do_unHuffman() {
    uint32_t source = R(0) & 0xFFFFFFFCu, dest = R(1);
    uint32_t header = rd32(source);
    int remaining = header >> 8;
    unsigned bits = header & 0xF;
    if (bits == 0) bits = 8;
    if ((32 % bits) || bits == 1) return;  // unimplemented unaligned Huffman
    int treesize = (rd8(source + 4) << 1) + 1;
    int block = 0;
    uint32_t treeBase = source + 5;
    source += 5 + treesize;
    uint32_t nPointer = treeBase;
    uint8_t node; int bitsSeen = 0;
    node = rd8(nPointer);
    while (remaining > 0) {
        uint32_t bitstream = rd32(source);
        source += 4;
        for (int bitsRemaining = 32; bitsRemaining > 0 && remaining > 0;
             --bitsRemaining, bitstream <<= 1) {
            uint32_t next = (nPointer & ~1u) + (node & 0x3F) * 2 + 2;  // Offset = bits[5:0]
            int readBits;
            if (bitstream & 0x80000000u) {
                if (node & 0x40) { readBits = rd8(next + 1); }        // RTerm = bit 6
                else { nPointer = next + 1; node = rd8(nPointer); continue; }
            } else {
                if (node & 0x80) { readBits = rd8(next); }            // LTerm = bit 7
                else { nPointer = next; node = rd8(nPointer); continue; }
            }
            block |= (readBits & ((1 << bits) - 1)) << bitsSeen;
            bitsSeen += bits;
            nPointer = treeBase; node = rd8(nPointer);
            if (bitsSeen == 32) {
                bitsSeen = 0; wr32(dest, block); dest += 4; remaining -= 4; block = 0;
            }
        }
    }
    setR(0, source); setR(1, dest);
}

void do_unRl(int width) {
    uint32_t source = R(0);
    int remaining = (rd32(source & 0xFFFFFFFCu) & 0xFFFFFF00u) >> 8;
    int padding = (4 - remaining) & 0x3;
    source += 4;
    uint32_t dest = R(1);
    int halfword = 0;
    while (remaining > 0) {
        int blockheader = rd8(source); ++source;
        if (blockheader & 0x80) {
            blockheader &= 0x7F; blockheader += 3;
            int block = rd8(source); ++source;
            while (blockheader-- && remaining) {
                --remaining;
                if (width == 2) {
                    if (dest & 1) { halfword |= block << 8; wr16(dest ^ 1, static_cast<uint16_t>(halfword)); }
                    else          { halfword = block; }
                } else { wr8(dest, static_cast<uint8_t>(block)); }
                ++dest;
            }
        } else {
            blockheader++;
            while (blockheader-- && remaining) {
                --remaining;
                int byte = rd8(source); ++source;
                if (width == 2) {
                    if (dest & 1) { halfword |= byte << 8; wr16(dest ^ 1, static_cast<uint16_t>(halfword)); }
                    else          { halfword = byte; }
                } else { wr8(dest, static_cast<uint8_t>(byte)); }
                ++dest;
            }
        }
    }
    if (width == 2) {
        if (dest & 1) { --padding; ++dest; }
        for (; padding > 0; padding -= 2, dest += 2) wr16(dest, 0);
    } else {
        while (padding--) { wr8(dest, 0); ++dest; }
    }
    setR(0, source); setR(1, dest);
}

void do_unFilter(int inwidth, int outwidth) {
    uint32_t source = R(0) & 0xFFFFFFFCu, dest = R(1);
    uint32_t header = rd32(source);
    int remaining = header >> 8;
    uint16_t halfword = 0, old = 0;
    source += 4;
    while (remaining > 0) {
        uint16_t nw = (inwidth == 1) ? rd8(source) : rd16(source);
        nw += old;
        if (outwidth > inwidth) {
            halfword >>= 8; halfword |= (nw << 8);
            if (source & 1) { wr16(dest, halfword); dest += outwidth; remaining -= outwidth; }
        } else if (outwidth == 1) {
            wr8(dest, static_cast<uint8_t>(nw)); dest += outwidth; remaining -= outwidth;
        } else {
            wr16(dest, nw); dest += outwidth; remaining -= outwidth;
        }
        old = nw;
        source += inwidth;
    }
    setR(0, source); setR(1, dest);
}

void do_unBitPack() {
    uint32_t source = R(0), dest = R(1), info = R(2);
    unsigned sourceLen   = rd16(info);
    unsigned sourceWidth = rd8(info + 2);
    unsigned destWidth   = rd8(info + 3);
    switch (sourceWidth) { case 1: case 2: case 4: case 8: break; default: return; }
    switch (destWidth)   { case 1: case 2: case 4: case 8: case 16: case 32: break; default: return; }
    uint32_t bias = rd32(info + 4);
    uint8_t in = 0; uint32_t out = 0;
    int bitsRemaining = 0, bitsEaten = 0;
    while (sourceLen > 0 || bitsRemaining) {
        if (!bitsRemaining) { in = rd8(source); bitsRemaining = 8; ++source; --sourceLen; }
        unsigned scaled = in & ((1u << sourceWidth) - 1);
        in >>= sourceWidth;
        if (scaled || (bias & 0x80000000u)) scaled += bias & 0x7FFFFFFFu;
        bitsRemaining -= sourceWidth;
        out |= scaled << bitsEaten;
        bitsEaten += destWidth;
        if (bitsEaten == 32) { wr32(dest, out); bitsEaten = 0; out = 0; dest += 4; }
    }
    setR(0, source); setR(1, dest);
}

void do_midi_key_2_freq() {
    uint32_t key = rd32(R(0) + 4);
    setR(0, static_cast<uint32_t>(
        key / exp2f((180.f - R(1) - R(2) / 256.f) / 12.f)));
}

// ── dispatcher ──────────────────────────────────────────────────────────────
// Returns 1 (serviced in HLE) or 0 (fall through to the recompiled LLE BIOS).
// The default of any un-cased SWI is 0, so LLE handles everything HLE omits
// (SoftReset/RegisterRamReset/Halt/Stop/IntrWait/VBlankIntrWait, the sound
// driver SWIs, MultiBoot, …) — those need the halt/IRQ/reset machinery the
// recompiled BIOS already implements exactly, and mGBA's own HLE likewise
// delegates them.
int dispatch(uint32_t swi) {
    uint32_t cost = 45;  // nominal SWI overhead for the non-stall cases
    switch (swi) {
    case SWI_DIV:              cost = do_div(static_cast<int32_t>(R(0)), static_cast<int32_t>(R(1))); break;
    case SWI_DIV_ARM:          cost = do_div(static_cast<int32_t>(R(1)), static_cast<int32_t>(R(0))); break;
    case SWI_SQRT: {
        uint32_t c; setR(0, static_cast<uint32_t>(_Sqrt(R(0), &c))); cost = c; break;
    }
    case SWI_ARCTAN: {
        uint32_t c; int32_t r1, r3;
        int16_t v = _ArcTan(static_cast<int32_t>(R(0)), &r1, &r3, &c);
        setR(0, static_cast<uint32_t>(static_cast<int32_t>(v)));
        setR(1, static_cast<uint32_t>(r1)); setR(3, static_cast<uint32_t>(r3)); cost = c; break;
    }
    case SWI_ARCTAN2: {
        uint32_t c; int32_t r1 = static_cast<int32_t>(R(1));
        int16_t v = _ArcTan2(static_cast<int32_t>(R(0)), static_cast<int32_t>(R(1)), &r1, &c);
        setR(0, static_cast<uint16_t>(v)); setR(1, static_cast<uint32_t>(r1));
        setR(3, 0x170u); cost = c; break;
    }
    case SWI_GET_BIOS_CHECKSUM:
        setR(0, GBA_BIOS_CHECKSUM); setR(1, 1u); setR(3, SIZE_BIOS); break;
    case SWI_CPU_SET:          do_cpu_set(); cost = 10 + (R(2) & 0x1FFFFF) * 2; break;
    case SWI_CPU_FAST_SET:     do_cpu_fast_set(); cost = 10 + (R(2) & 0x1FFFFF) * 2; break;
    case SWI_BG_AFFINE_SET:    do_bg_affine_set(); cost = 10 + R(2) * 60; break;
    case SWI_OBJ_AFFINE_SET:   do_obj_affine_set(); cost = 10 + R(2) * 40; break;
    case SWI_BIT_UNPACK:       do_unBitPack(); break;
    case SWI_LZ77_UNCOMP_WRAM: cost = do_unLz77(1); break;
    case SWI_LZ77_UNCOMP_VRAM: cost = do_unLz77(2); break;
    case SWI_HUFFMAN_UNCOMP:   do_unHuffman(); break;
    case SWI_RL_UNCOMP_WRAM:   do_unRl(1); break;
    case SWI_RL_UNCOMP_VRAM:   do_unRl(2); break;
    case SWI_DIFF_8BIT_UNFILTER_WRAM: do_unFilter(1, 1); break;
    case SWI_DIFF_8BIT_UNFILTER_VRAM: do_unFilter(1, 2); break;
    case SWI_DIFF_16BIT_UNFILTER:     do_unFilter(2, 2); break;
    case SWI_SOUND_BIAS:       wr16(REG_SOUNDBIAS, static_cast<uint16_t>(R(0) ? 0x200 : 0)); break;
    case SWI_MIDI_KEY_2_FREQ:  do_midi_key_2_freq(); break;
    default:
        return 0;  // fall through to the recompiled LLE BIOS
    }
    runtime_tick(cost);
    return 1;
}

}  // namespace

void bios_hle_set_mode(BiosHleMode mode) {
    g_mode = mode;
    g_bios_hle_hook = (mode == BiosHleMode::On) ? &dispatch : nullptr;
}

void bios_hle_boot_skip(uint32_t cart_entry) {
    // Synthesize the state the real BIOS leaves when it hands off to the cart
    // (GBATEK "GBA Reset"; the same state NanoBoyAdvance / mGBA HLE reproduce),
    // then jump straight to the cart entry — the boot logo/chime never runs.
    // reset_recomp_cpu() already seeded the canonical banked SPs; we flip to
    // System mode, point the active SP at the System/User bank, clear the
    // low registers, and zero the user IRQ-handler pointer the recompiled BIOS
    // dispatcher (vector 0x18) reads. R13/R14/R15 are set explicitly below.
    for (int i = 0; i < 13; ++i) g_cpu.R[i] = 0;
    g_cpu.banked_sp[ARM_BANK_SUPERVISOR] = 0x03007FE0u;
    g_cpu.banked_sp[ARM_BANK_IRQ]        = 0x03007FA0u;
    g_cpu.banked_sp[ARM_BANK_USER]       = 0x03007F00u;
    g_cpu.banked_lr[ARM_BANK_SUPERVISOR] = 0;
    g_cpu.banked_lr[ARM_BANK_IRQ]        = 0;
    g_cpu.R[13] = 0x03007F00u;   // active SP = System/User bank
    g_cpu.R[14] = 0;
    g_cpu.cpsr  = 0x0000001Fu;   // System mode, ARM state, IRQ/FIQ enabled
    g_cpu.R[15] = cart_entry;
    // User IRQ handler pointer the BIOS IRQ dispatcher reads; the game installs
    // its own during init. Zero = none yet. (IME defaults to 0, so no IRQ can
    // vector before the game programs IE/IME anyway.)
    bus_write_u32(0x03007FFCu, 0);
}

BiosHleMode bios_hle_mode() { return g_mode; }

const char* bios_hle_mode_name(BiosHleMode mode) {
    switch (mode) {
    case BiosHleMode::Off: return "LLE (recompiled BIOS)";
    case BiosHleMode::On:  return "HLE (with LLE fallback)";
    }
    return "unknown";
}

}  // namespace gba
