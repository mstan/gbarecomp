// codebreaker.h -- strict CodeBreaker Advance code interpreter.
//
// The engine side of declarative mod cheats (docs/MOD_PACKAGES.md "Cheat
// codes"): a package's [[cheat]] record lists CodeBreaker Advance lines, the
// mod runtime parses them once when the manifest is loaded (any error rejects
// the manifest before boot), and the runtime runs the committed programs once
// per guest frame at frame start. There is no menu, preset table or game hook
// here; enablement is purely the mod catalog's feature selection.
//
// Semantics follow mGBA's CodeBreaker implementation (src/gba/cheats/
// codebreaker.c + src/core/cheats.c), which is the oracle used here:
//
//   0aaaaaaa xxxx  game ID (master code)             accepted, no effect
//   1aaaaaaa xxxx  hook address (master code)        accepted, no effect
//   2aaaaaaa yyyy  [a] = [a] | yyyy                  16-bit
//   3aaaaaaa 00yy  [a] = yy                          8-bit
//   4aaaaaaa yyyy  slide: next line iiiicccc ssss    cccc writes of yyyy at a,
//                                                    a += ssss bytes, y += iiii
//   5aaaaaaa cccc  fill list: cccc halfwords follow, three per line, each
//                  written byte-swapped at a, a+2, ...
//   6aaaaaaa yyyy  [a] = [a] & yyyy                  16-bit
//   7aaaaaaa yyyy  if [a] == yyyy run the next code
//   8aaaaaaa yyyy  [a] = yyyy                        16-bit
//   9...           encryption seed                   REJECTED (use decrypted)
//   Aaaaaaaa yyyy  if [a] != yyyy run the next code
//   Baaaaaaa yyyy  if [a] >  yyyy run the next code  (unsigned 16-bit)
//   Caaaaaaa yyyy  if [a] <  yyyy run the next code  (unsigned 16-bit)
//   D0000020 yyyy  if every button in yyyy is held (KEYINPUT active-low)
//   Eaaaaaaa yyyy  [a] = [a] + yyyy                  16-bit
//   Faaaaaaa yyyy  if ([a] & yyyy) != 0 run the next code
//
// "The next code" is the next complete code, including all continuation
// lines of a slide or fill list.
//
// Unlike a desktop emulator the parser is strict, because a package is a
// trust boundary: every address a code reads or writes must lie inside EWRAM
// (0x02000000-0x0203FFFF) or IWRAM (0x03000000-0x03007FFF), 16-bit accesses
// must be halfword aligned, multi-line codes must be complete, and a
// conditional must be followed by a code. Cheats therefore never touch IO,
// VRAM, ROM, save hardware or generated code.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gbarecomp::codebreaker {

enum class OpKind : std::uint8_t {
    Write8,
    Write16,
    Or16,
    And16,
    Add16,
    Slide16,
    FillList16,
    IfEqual,
    IfNotEqual,
    IfGreater,
    IfLess,
    IfAnd,
    IfButtons,
};

struct Op {
    OpKind kind = OpKind::Write16;
    std::uint32_t address = 0;
    std::uint32_t operand = 0;
    // Slide16 only.
    std::uint32_t count = 1;
    std::uint32_t address_step = 0;
    std::uint32_t operand_step = 0;
    // FillList16 only: halfwords already byte-swapped into write order.
    std::vector<std::uint16_t> values;
};

struct Program {
    std::vector<Op> ops;
};

// Parse one code per element ("XXXXXXXX YYYY", hex, case-insensitive,
// optional whitespace between the two words). Returns false and a message
// naming the 1-based line on the first error; `out` is left empty then.
bool parse(const std::vector<std::string>& lines, Program* out,
           std::string* error);

// Guest memory used to run a program. The runtime binds GbaBus accessors;
// tests bind plain arrays. Every pointer must be non-null.
struct Memory {
    void* ctx = nullptr;
    std::uint16_t (*read16)(void* ctx, std::uint32_t address) = nullptr;
    void (*write8)(void* ctx, std::uint32_t address, std::uint8_t value) = nullptr;
    void (*write16)(void* ctx, std::uint32_t address, std::uint16_t value) = nullptr;
};

// KEYINPUT, read by D0000020 conditionals.
constexpr std::uint32_t kKeyInputAddress = 0x04000130u;

// Run every op once (one guest frame).
void run(const Program& program, const Memory& memory);

}  // namespace gbarecomp::codebreaker
