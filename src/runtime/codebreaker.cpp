// codebreaker.cpp -- see codebreaker.h.
//
// Derived from the CodeBreaker cheat engine in the NovaPowers gbarecomp fork,
// reworked into a strict, data-only interpreter for mod packages.
#include "codebreaker.h"

namespace gbarecomp::codebreaker {
namespace {

constexpr std::uint64_t kEwramBegin = 0x02000000u;
constexpr std::uint64_t kEwramEnd = 0x02040000u;
constexpr std::uint64_t kIwramBegin = 0x03000000u;
constexpr std::uint64_t kIwramEnd = 0x03008000u;

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool is_blank(char c) { return c == ' ' || c == '\t'; }

// Exactly "XXXXXXXX YYYY": eight hex digits, optional blanks, four hex
// digits, nothing else but surrounding blanks.
bool parse_words(const std::string& text, std::uint32_t* op1,
                 std::uint16_t* op2) {
    std::size_t i = 0;
    while (i < text.size() && is_blank(text[i])) ++i;
    std::uint32_t first = 0;
    for (int digit = 0; digit < 8; ++digit, ++i) {
        if (i >= text.size() || hex_value(text[i]) < 0) return false;
        first = (first << 4) | static_cast<std::uint32_t>(hex_value(text[i]));
    }
    while (i < text.size() && is_blank(text[i])) ++i;
    std::uint32_t second = 0;
    for (int digit = 0; digit < 4; ++digit, ++i) {
        if (i >= text.size() || hex_value(text[i]) < 0) return false;
        second = (second << 4) | static_cast<std::uint32_t>(hex_value(text[i]));
    }
    while (i < text.size() && is_blank(text[i])) ++i;
    if (i != text.size()) return false;
    *op1 = first;
    *op2 = static_cast<std::uint16_t>(second);
    return true;
}

// [begin, end) lies entirely inside one work-RAM region.
bool in_ram(std::uint64_t begin, std::uint64_t end) {
    if (end <= begin) return false;
    return (begin >= kEwramBegin && end <= kEwramEnd) ||
           (begin >= kIwramBegin && end <= kIwramEnd);
}

bool halfword_ok(std::uint64_t address) {
    return (address & 1u) == 0 && in_ram(address, address + 2);
}

bool is_conditional(OpKind kind) {
    switch (kind) {
        case OpKind::IfEqual:
        case OpKind::IfNotEqual:
        case OpKind::IfGreater:
        case OpKind::IfLess:
        case OpKind::IfAnd:
        case OpKind::IfButtons:
            return true;
        default:
            return false;
    }
}

std::uint16_t byte_swap(std::uint32_t halfword) {
    return static_cast<std::uint16_t>(((halfword >> 8) & 0xFFu) |
                                      ((halfword & 0xFFu) << 8));
}

}  // namespace

bool parse(const std::vector<std::string>& lines, Program* out,
           std::string* error) {
    Program program;
    auto fail = [&](std::size_t index, const std::string& why) {
        if (error)
            *error = "CodeBreaker line " + std::to_string(index + 1) + ": " + why;
        if (out) out->ops.clear();
        return false;
    };
    if (lines.empty()) return fail(0, "no codes");

    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::uint32_t op1 = 0;
        std::uint16_t op2 = 0;
        if (!parse_words(lines[i], &op1, &op2))
            return fail(i, "expected \"XXXXXXXX YYYY\" hex words");
        const unsigned type = op1 >> 28;
        const std::uint32_t address = op1 & 0x0FFFFFFFu;
        Op op;
        op.address = address;
        op.operand = op2;
        switch (type) {
            case 0x0:  // game ID master code
            case 0x1:  // hook master code
                continue;
            case 0x2: op.kind = OpKind::Or16; break;
            case 0x3:
                op.kind = OpKind::Write8;
                op.operand = op2 & 0xFFu;
                if (!in_ram(address, address + 1u))
                    return fail(i, "address is outside EWRAM/IWRAM");
                program.ops.push_back(op);
                continue;
            case 0x4: {
                if (i + 1 >= lines.size())
                    return fail(i, "slide code is missing its continuation line");
                std::uint32_t c1 = 0;
                std::uint16_t c2 = 0;
                if (!parse_words(lines[i + 1], &c1, &c2))
                    return fail(i + 1, "expected \"XXXXXXXX YYYY\" hex words");
                op.kind = OpKind::Slide16;
                op.count = c1 & 0xFFFFu;
                op.operand_step = c1 >> 16;
                op.address_step = c2;
                if (op.count > 1 && (op.address_step & 1u))
                    return fail(i + 1, "slide address step must be even");
                if (!halfword_ok(address))
                    return fail(i, "address is outside EWRAM/IWRAM or misaligned");
                if (op.count > 1) {
                    const std::uint64_t last =
                        address + std::uint64_t{op.count - 1} * op.address_step;
                    if (!in_ram(address, last + 2u))
                        return fail(i, "slide leaves its EWRAM/IWRAM region");
                }
                ++i;
                program.ops.push_back(op);
                continue;
            }
            case 0x5: {
                op.kind = OpKind::FillList16;
                const std::uint32_t count = op2;
                if (count == 0) return fail(i, "fill list has no halfwords");
                if (!halfword_ok(address) ||
                    !in_ram(address, address + std::uint64_t{count} * 2u))
                    return fail(i, "fill list leaves its EWRAM/IWRAM region "
                                   "or is misaligned");
                const std::size_t head = i;
                while (op.values.size() < count) {
                    if (++i >= lines.size())
                        return fail(head, "fill list is missing data lines");
                    std::uint32_t d1 = 0;
                    std::uint16_t d2 = 0;
                    if (!parse_words(lines[i], &d1, &d2))
                        return fail(i, "expected \"XXXXXXXX YYYY\" hex words");
                    const std::uint32_t halves[3] = {d1 >> 16, d1 & 0xFFFFu, d2};
                    for (std::uint32_t half : halves)
                        if (op.values.size() < count)
                            op.values.push_back(byte_swap(half));
                }
                program.ops.push_back(op);
                continue;
            }
            case 0x6: op.kind = OpKind::And16; break;
            case 0x7: op.kind = OpKind::IfEqual; break;
            case 0x8: op.kind = OpKind::Write16; break;
            case 0x9:
                return fail(i, "encrypted CodeBreaker codes are not supported; "
                               "use the decrypted codes");
            case 0xA: op.kind = OpKind::IfNotEqual; break;
            case 0xB: op.kind = OpKind::IfGreater; break;
            case 0xC: op.kind = OpKind::IfLess; break;
            case 0xD:
                if (address != 0x20u)
                    return fail(i, "only the D0000020 button conditional is "
                                   "supported");
                op.kind = OpKind::IfButtons;
                op.address = kKeyInputAddress;
                program.ops.push_back(op);
                continue;
            case 0xE: op.kind = OpKind::Add16; break;
            case 0xF: op.kind = OpKind::IfAnd; break;
        }
        if (!halfword_ok(address))
            return fail(i, "address is outside EWRAM/IWRAM or misaligned");
        program.ops.push_back(op);
    }
    if (program.ops.empty())
        return fail(lines.size() - 1, "no code has an effect");
    if (is_conditional(program.ops.back().kind))
        return fail(lines.size() - 1, "conditional is not followed by a code");
    if (out) *out = std::move(program);
    return true;
}

void run(const Program& program, const Memory& memory) {
    bool skip_next = false;
    for (const Op& op : program.ops) {
        if (skip_next) {
            skip_next = false;
            continue;
        }
        const std::uint32_t a = op.address;
        const std::uint32_t v = op.operand;
        switch (op.kind) {
            case OpKind::Write8:
                memory.write8(memory.ctx, a, static_cast<std::uint8_t>(v));
                break;
            case OpKind::Write16:
                memory.write16(memory.ctx, a, static_cast<std::uint16_t>(v));
                break;
            case OpKind::Or16:
                memory.write16(memory.ctx, a, static_cast<std::uint16_t>(
                    memory.read16(memory.ctx, a) | v));
                break;
            case OpKind::And16:
                memory.write16(memory.ctx, a, static_cast<std::uint16_t>(
                    memory.read16(memory.ctx, a) & v));
                break;
            case OpKind::Add16:
                memory.write16(memory.ctx, a, static_cast<std::uint16_t>(
                    memory.read16(memory.ctx, a) + v));
                break;
            case OpKind::Slide16: {
                std::uint32_t address = a;
                std::uint32_t value = v;
                for (std::uint32_t k = 0; k < op.count; ++k) {
                    memory.write16(memory.ctx, address,
                                   static_cast<std::uint16_t>(value));
                    address += op.address_step;
                    value += op.operand_step;
                }
                break;
            }
            case OpKind::FillList16:
                for (std::size_t k = 0; k < op.values.size(); ++k)
                    memory.write16(memory.ctx,
                                   a + static_cast<std::uint32_t>(k * 2u),
                                   op.values[k]);
                break;
            case OpKind::IfEqual:
                skip_next = memory.read16(memory.ctx, a) != v;
                break;
            case OpKind::IfNotEqual:
                skip_next = memory.read16(memory.ctx, a) == v;
                break;
            case OpKind::IfGreater:
                skip_next = !(memory.read16(memory.ctx, a) > v);
                break;
            case OpKind::IfLess:
                skip_next = !(memory.read16(memory.ctx, a) < v);
                break;
            case OpKind::IfAnd:
                skip_next = (memory.read16(memory.ctx, a) & v) == 0;
                break;
            case OpKind::IfButtons:
                // KEYINPUT is active-low: every selected button must read 0.
                skip_next = (memory.read16(memory.ctx, a) & v) != 0;
                break;
        }
    }
}

}  // namespace gbarecomp::codebreaker
