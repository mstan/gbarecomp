// gba_recompile — read a GBA ROM or BIOS image, discover functions
// reachable from the entry point + seeded symbols, lower their
// ARM/THUMB bodies through the armv4t IR into C++ source, and write
// the result to a per-game `generated/` directory (or, for BIOS, to
// `gbarecomp/src/runtime/generated_bios/`).
//
// Two modes:
//   --rom  <rom.gba>      cartridge recompilation (rom_base=0x08000000)
//   --bios <bios.bin>     BIOS recompilation (rom_base=0x00000000),
//                         seeds reset/SWI/IRQ vectors at 0x00/0x08/0x18.
//
// CLI:
//   gba_recompile --rom <rom.gba>
//                 --entry 0x080000C0
//                 [--symbols <imported_symbols.tsv>]
//                 [--out <out_dir>]
//                 [--rom-base 0x08000000]
//                 [--max-functions 4096]
//
//   gba_recompile --bios <bios.bin>
//                 [--out <out_dir>]
//                 [--max-functions 256]
//
// Cart output (default --out = "generated"):
//   <out_dir>/recompiled.cpp        function bodies
//   <out_dir>/recompiled.h          forward declarations
//   <out_dir>/dispatch_table.cpp    kDispatchTable / kDispatchTableLen
//
// BIOS output (default --out = "src/runtime/generated_bios"):
//   <out_dir>/bios_recompiled.cpp   function bodies
//   <out_dir>/bios_recompiled.h     forward declarations
//   <out_dir>/bios_dispatch_table.cpp  kBiosDispatchTable[] /
//                                       kBiosDispatchTableLen

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "config.h"
#include "function_finder.h"
#include "arm_codegen.h"
#include "arm_decode.h"
#include "thumb_decode.h"
#include "arm_ir.h"

using gbarecomp::Config;
using gbarecomp::CpuMode;
using gbarecomp::Function;
using gbarecomp::FunctionFinder;
using gbarecomp::FunctionSeed;

namespace {

struct Cli {
    std::string rom_path;       // --rom (cart mode)
    std::string bios_path;      // --bios (BIOS mode); mutually exclusive
    std::string symbols_path;
    std::string config_path;    // --config <path> (optional, per-binary TOML)
    std::string out_dir;
    uint32_t entry = 0x080000C0u;
    uint32_t rom_base = 0x08000000u;
    std::size_t max_functions = 4096;
    bool ok = true;
    bool bios_mode = false;     // set when --bios is parsed
    bool emit_symbol_map = true; // --no-symbol-map opts out (debug aid)
};

void print_usage() {
    std::printf(
        "gba_recompile --rom <path> [--entry HEX] [--symbols TSV]\n"
        "              [--config TOML] [--out DIR] [--rom-base HEX]\n"
        "              [--max-functions N]\n"
        "\n"
        "  Cart-recompile mode. Discovers functions reachable from\n"
        "  --entry + --symbols seeds and writes <out>/recompiled.{cpp,h}\n"
        "  + dispatch_table.cpp.\n"
        "\n"
        "gba_recompile --bios <path>\n"
        "              [--config TOML] [--out DIR] [--max-functions N]\n"
        "\n"
        "  BIOS-recompile mode. rom_base=0x00000000, default --out is\n"
        "  src/runtime/generated_bios. Seeds reset (0x00 ARM),\n"
        "  SWI (0x08 ARM), IRQ (0x18 ARM). Output is\n"
        "  bios_recompiled.{cpp,h} + bios_dispatch_table.cpp.\n"
        "\n"
        "  --config <TOML>  Per-binary configuration (manual function\n"
        "                   seeds, data-range exclusions, jump tables,\n"
        "                   identity hash). See docs/TOML_SCHEMA.md.\n");
}

bool parse_hex(const char* s, uint32_t& out) {
    if (!s) return false;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    char* end = nullptr;
    unsigned long v = std::strtoul(s, &end, 16);
    if (end == s) return false;
    out = static_cast<uint32_t>(v);
    return true;
}

Cli parse_cli(int argc, char** argv) {
    Cli c;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* {
            return (i + 1 < argc) ? argv[++i] : nullptr;
        };
        if (a == "--rom")               c.rom_path     = next() ? argv[i] : "";
        else if (a == "--bios") {
            c.bios_path = next() ? argv[i] : "";
            c.bios_mode = true;
        }
        else if (a == "--entry")        parse_hex(next(), c.entry);
        else if (a == "--symbols")      c.symbols_path = next() ? argv[i] : "";
        else if (a == "--config")       c.config_path  = next() ? argv[i] : "";
        else if (a == "--out")          c.out_dir      = next() ? argv[i] : "";
        else if (a == "--rom-base")     parse_hex(next(), c.rom_base);
        else if (a == "--no-symbol-map") c.emit_symbol_map = false;
        else if (a == "--symbol-map")    c.emit_symbol_map = true;
        else if (a == "--max-functions") {
            const char* v = next();
            if (v) c.max_functions =
                static_cast<std::size_t>(std::strtoul(v, nullptr, 10));
        } else if (a == "--help" || a == "-h") {
            print_usage();
            c.ok = false;
            return c;
        } else {
            std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
            c.ok = false;
            return c;
        }
    }
    if (c.bios_mode) {
        if (!c.rom_path.empty()) {
            std::fprintf(stderr,
                "--bios and --rom are mutually exclusive\n");
            c.ok = false;
            return c;
        }
        if (c.bios_path.empty()) {
            std::fprintf(stderr, "--bios needs a path argument\n");
            c.ok = false;
            return c;
        }
        // BIOS mode defaults: rom_base=0, out_dir=src/runtime/generated_bios.
        c.rom_base = 0x00000000u;
        if (c.out_dir.empty()) c.out_dir = "src/runtime/generated_bios";
    } else {
        if (c.rom_path.empty()) {
            std::fprintf(stderr, "missing --rom (or --bios for BIOS mode)\n");
            c.ok = false;
        }
        if (c.out_dir.empty()) c.out_dir = "generated";
    }
    return c;
}

std::vector<uint8_t> read_file(const std::string& path,
                                std::string* err) {
    std::vector<uint8_t> out;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { *err = "open failed: " + path; return out; }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) { std::fclose(f); *err = "empty or unseekable"; return out; }
    out.resize(static_cast<std::size_t>(sz));
    std::size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    if (got != out.size()) {
        *err = "short read";
        out.clear();
    }
    return out;
}

// Parse the importer's TSV:
//   # comments...
//   0x080000C0\tarm\tname_or_blank
std::vector<FunctionSeed> load_symbols(const std::string& path) {
    std::vector<FunctionSeed> out;
    if (path.empty()) return out;
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "warn: symbols file not readable: %s\n",
                     path.c_str());
        return out;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::stringstream ss(line);
        std::string addr_s, mode_s, name_s;
        if (!std::getline(ss, addr_s, '\t')) continue;
        if (!std::getline(ss, mode_s, '\t')) continue;
        std::getline(ss, name_s, '\t');
        uint32_t addr = 0;
        if (addr_s.size() > 2 && addr_s[0] == '0' &&
            (addr_s[1] == 'x' || addr_s[1] == 'X')) {
            addr = static_cast<uint32_t>(
                std::strtoul(addr_s.c_str() + 2, nullptr, 16));
        } else {
            continue;
        }
        CpuMode m = (mode_s == "arm") ? CpuMode::Arm : CpuMode::Thumb;
        out.push_back(FunctionSeed{addr, m, name_s});
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────
// Emitter
// ─────────────────────────────────────────────────────────────────────

void emit_function_body(std::FILE* f, const Function& fn,
                        const uint8_t* rom, std::size_t rom_size,
                        uint32_t rom_base,
                        const std::unordered_map<uint64_t, std::string>&
                            func_names_by_key) {
    const uint32_t step = (fn.mode == CpuMode::Thumb) ? 2u : 4u;
    armv4t::CodegenCtx ctx;
    ctx.names_by_key = &func_names_by_key;
    ctx.current_function_addr = fn.addr;
    ctx.current_function_end_addr = fn.end_addr;
    ctx.current_function_thumb = (fn.mode == CpuMode::Thumb);
    const uint32_t fn_source_addr = fn.source_addr ? fn.source_addr : fn.addr;

    auto source_offset_for = [&](uint32_t guest_pc, uint32_t len,
                                 std::size_t* out) -> bool {
        int64_t delta = static_cast<int64_t>(guest_pc) -
            static_cast<int64_t>(fn.addr);
        int64_t source_pc = static_cast<int64_t>(fn_source_addr) + delta;
        if (source_pc < static_cast<int64_t>(rom_base)) return false;
        uint64_t off64 = static_cast<uint64_t>(
            source_pc - static_cast<int64_t>(rom_base));
        if (off64 + len > rom_size) return false;
        *out = static_cast<std::size_t>(off64);
        return true;
    };

    auto plain_reg_source = [](const armv4t::Instr& ins, uint8_t* rm) {
        if (ins.op != armv4t::IrOp::MOV ||
            ins.op2.kind != armv4t::Op2::Kind::Shifted ||
            ins.op2.shifted.by_register ||
            ins.op2.shifted.type != armv4t::ShiftType::LSL ||
            ins.op2.shifted.imm_or_rs != 0) {
            return false;
        }
        *rm = ins.op2.shifted.rm;
        return true;
    };

    auto invalidate_written_aliases = [](const armv4t::Instr& ins,
                                         bool alias[16]) {
        auto clear = [&](uint8_t reg) {
            if (reg < 16) alias[reg] = false;
        };

        switch (ins.op) {
            case armv4t::IrOp::AND: case armv4t::IrOp::EOR:
            case armv4t::IrOp::SUB: case armv4t::IrOp::RSB:
            case armv4t::IrOp::ADD: case armv4t::IrOp::ADC:
            case armv4t::IrOp::SBC: case armv4t::IrOp::RSC:
            case armv4t::IrOp::ORR: case armv4t::IrOp::MOV:
            case armv4t::IrOp::BIC: case armv4t::IrOp::MVN:
            case armv4t::IrOp::LDR: case armv4t::IrOp::LDRB:
            case armv4t::IrOp::LDRH: case armv4t::IrOp::LDRSB:
            case armv4t::IrOp::LDRSH: case armv4t::IrOp::SWP:
            case armv4t::IrOp::SWPB: case armv4t::IrOp::MRS:
                clear(ins.rd);
                break;
            case armv4t::IrOp::MUL: case armv4t::IrOp::MLA:
                clear(ins.rd);
                break;
            case armv4t::IrOp::UMULL: case armv4t::IrOp::UMLAL:
            case armv4t::IrOp::SMULL: case armv4t::IrOp::SMLAL:
                clear(ins.rd);
                clear(ins.rn);
                break;
            case armv4t::IrOp::LDM:
                if (ins.block.load) {
                    for (uint8_t reg = 0; reg < 16; ++reg) {
                        if (ins.block.reg_list & (1u << reg)) {
                            clear(reg);
                        }
                    }
                }
                break;
            case armv4t::IrOp::BL:
            case armv4t::IrOp::BL_prefix:
            case armv4t::IrOp::BL_suffix:
                clear(14);
                break;
            default:
                break;
        }
    };

    std::unordered_set<uint32_t> backward_targets;
    std::unordered_set<uint32_t> bx_c_return_pcs;
    bool lr_alias[16] = {};
    lr_alias[14] = true;
    armv4t::Instr prev_scan_ins{};
    bool have_prev_scan_ins = false;

    // Function boundaries can split compact THUMB epilogues:
    //
    //   pop {r3}
    //   bx  r3
    //
    // If the finder discovered the BX as a separate entry, the scan below
    // starts at the BX and would otherwise miss the stack-pop return idiom.
    // Seed the previous instruction from ROM so the first decoded instruction
    // in this function can still be classified correctly.
    if (fn_source_addr >= rom_base && (fn_source_addr - rom_base) >= step) {
        const uint32_t prev_pc = fn.addr - step;
        std::size_t prev_off = 0;
        if (source_offset_for(prev_pc, step, &prev_off)) {
            if (fn.mode == CpuMode::Thumb) {
                uint16_t hw = static_cast<uint16_t>(
                    rom[prev_off] | (rom[prev_off + 1] << 8));
                prev_scan_ins = armv4t::ThumbDecoder::decode(hw, prev_pc);
            } else {
                uint32_t w = static_cast<uint32_t>(rom[prev_off])
                    | (static_cast<uint32_t>(rom[prev_off + 1]) << 8)
                    | (static_cast<uint32_t>(rom[prev_off + 2]) << 16)
                    | (static_cast<uint32_t>(rom[prev_off + 3]) << 24);
                prev_scan_ins = armv4t::ArmDecoder::decode(w, prev_pc);
            }
            have_prev_scan_ins = true;
        }
    }

    for (uint32_t scan_pc = fn.addr; scan_pc < fn.end_addr; scan_pc += step) {
        std::size_t scan_off = 0;
        if (!source_offset_for(scan_pc, step, &scan_off)) break;
        armv4t::Instr scan_ins;
        if (fn.mode == CpuMode::Thumb) {
            uint16_t hw = static_cast<uint16_t>(
                rom[scan_off] | (rom[scan_off + 1] << 8));
            scan_ins = armv4t::ThumbDecoder::decode(hw, scan_pc);
        } else {
            uint32_t w = static_cast<uint32_t>(rom[scan_off])
                | (static_cast<uint32_t>(rom[scan_off + 1]) << 8)
                | (static_cast<uint32_t>(rom[scan_off + 2]) << 16)
                | (static_cast<uint32_t>(rom[scan_off + 3]) << 24);
            scan_ins = armv4t::ArmDecoder::decode(w, scan_pc);
        }
        if (scan_ins.op == armv4t::IrOp::B &&
            scan_ins.branch_target >= fn.addr &&
            scan_ins.branch_target < fn.end_addr &&
            scan_ins.branch_target < scan_pc) {
            backward_targets.insert(scan_ins.branch_target);
        }
        if (scan_ins.op == armv4t::IrOp::BX &&
            have_prev_scan_ins &&
            prev_scan_ins.op == armv4t::IrOp::LDM &&
            prev_scan_ins.block.load &&
            prev_scan_ins.block.writeback &&
            prev_scan_ins.block.rn == 13 &&
            scan_ins.rm < 16 &&
            (prev_scan_ins.block.reg_list &
                static_cast<uint16_t>(1u << scan_ins.rm)) != 0) {
            bx_c_return_pcs.insert(scan_pc);
        }
        if (scan_ins.op == armv4t::IrOp::BX &&
            scan_ins.rm < 16 &&
            lr_alias[scan_ins.rm]) {
            bx_c_return_pcs.insert(scan_pc);
        }

        bool sets_lr_alias = false;
        uint8_t alias_dst = 0;
        uint8_t alias_src = 0;
        if (plain_reg_source(scan_ins, &alias_src) &&
            scan_ins.rd < 16 &&
            scan_ins.cond == armv4t::Cond::AL &&
            alias_src < 16 &&
            lr_alias[alias_src]) {
            sets_lr_alias = true;
            alias_dst = scan_ins.rd;
        }
        invalidate_written_aliases(scan_ins, lr_alias);
        if (sets_lr_alias) {
            lr_alias[alias_dst] = true;
        }

        prev_scan_ins = scan_ins;
        have_prev_scan_ins = true;
    }

    uint32_t pc = fn.addr;
    while (pc < fn.end_addr) {
        std::size_t off = 0;
        if (!source_offset_for(pc, step, &off)) break;
        armv4t::Instr ins;
        if (fn.mode == CpuMode::Thumb) {
            uint16_t hw = static_cast<uint16_t>(
                rom[off] | (rom[off + 1] << 8));
            ins = armv4t::ThumbDecoder::decode(hw, pc);
        } else {
            uint32_t w = static_cast<uint32_t>(rom[off])
                | (static_cast<uint32_t>(rom[off + 1]) << 8)
                | (static_cast<uint32_t>(rom[off + 2]) << 16)
                | (static_cast<uint32_t>(rom[off + 3]) << 24);
            ins = armv4t::ArmDecoder::decode(w, pc);
        }

        if (backward_targets.count(pc) != 0) {
            std::fprintf(f, "L_%08X:\n", pc);
        }

        std::string line = armv4t::format_ir(ins);
        std::fprintf(f, "    /* %08X  %s */\n", pc, line.c_str());

        bool ni = false;
        ctx.force_bx_c_return = bx_c_return_pcs.count(pc) != 0;
        std::string emitted = armv4t::ArmCodegen::emit_instr(ins, ctx, &ni);
        std::fputs(emitted.c_str(), f);
        // emit_instr already handles all return-shaped flows
        // (branches set PC + dispatch + return; SWI sets PC +
        // returns; LDM with PC in list returns; LDR PC dispatches).
        // For non-PC-writing instructions execution falls through
        // to the next emitted block.
        (void)ni;  // tracked for diagnostics; abort path is in the emit.
        pc += step;
    }
    // Fall-through tail-call dispatch.
    //
    // The function body may end for two reasons:
    //   1. A natural terminator (return, unconditional B, indirect
    //      branch, undefined). emit_instr() already emitted PC update
    //      + `return;`, so this trailing dispatch is dead code.
    //   2. The decode loop hit `fn.end_addr` (clipped to the next
    //      function's start by the finder). Execution is supposed
    //      to FALL THROUGH into the next function — typically when
    //      two callers enter a shared suffix at different points
    //      (e.g., the BIOS's reset_vector and SWI 0x26's handler
    //      share code starting at 0x8C).
    // In case 2, without this dispatch the runtime would return to
    // the exec loop with PC unchanged and re-dispatch the same
    // function forever. The trailing dispatch hands control to the
    // adjacent function via the dispatch table.
    std::fprintf(f,
        "    /* fall-through to 0x%08X */\n"
        "    g_cpu.R[15] = 0x%08Xu;\n"
        "    runtime_dispatch(0x%08Xu);\n"
        "    return;\n",
        fn.end_addr, fn.end_addr, fn.end_addr);
}

// Output naming: cart mode uses recompiled.{cpp,h} + dispatch_table.cpp
// with kDispatchTable. BIOS mode uses bios_recompiled.{cpp,h} +
// bios_dispatch_table.cpp with kBiosDispatchTable. The runtime
// consults kBiosDispatchTable first for PC < 0x4000 and falls
// through to kDispatchTable otherwise.
struct OutputNames {
    const char* header;
    const char* body;
    const char* dispatch;
    const char* table_symbol;
    const char* table_len_symbol;
    const char* description;
};

OutputNames names_for_mode(bool bios_mode) {
    if (bios_mode) {
        return {
            "bios_recompiled.h",
            "bios_recompiled.cpp",
            "bios_dispatch_table.cpp",
            "kBiosDispatchTable",
            "kBiosDispatchTableLen",
            "recompiled GBA BIOS",
        };
    }
    return {
        "recompiled.h",
        "recompiled.cpp",
        "dispatch_table.cpp",
        "kDispatchTable",
        "kDispatchTableLen",
        "recompiled cart code",
    };
}

void write_header(const std::string& dir,
                   const std::vector<Function>& funcs,
                   const OutputNames& names) {
    std::string path = dir + "/" + names.header;
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return;
    }
    std::fprintf(f,
        "// AUTO-GENERATED by gba_recompile. DO NOT EDIT.\n"
        "// Forward declarations for the %s.\n"
        "//\n"
        "// Total functions: %zu\n"
        "\n"
        "#pragma once\n\n"
        "extern \"C\" {\n",
        names.description, funcs.size());
    for (const auto& fn : funcs) {
        std::fprintf(f, "void %s(void);  /* 0x%08X %s */\n",
                     fn.name.c_str(), fn.addr,
                     fn.mode == CpuMode::Thumb ? "thumb" : "arm");
    }
    std::fprintf(f, "}  /* extern \"C\" */\n");
    std::fclose(f);
}

void write_dispatch_table(const std::string& dir,
                           const std::vector<Function>& funcs,
                           const OutputNames& names) {
    std::string path = dir + "/" + names.dispatch;
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f,
        "// AUTO-GENERATED by gba_recompile. DO NOT EDIT.\n"
        "// Dispatch table: maps recompiled function addresses to\n"
        "// the corresponding generated C function pointer. The\n"
        "// runtime calls runtime_dispatch(addr) which binary-searches\n"
        "// this table by address and current CPSR.T mode.\n\n"
        "#include <cstdint>\n"
        "#include \"%s\"\n\n"
        "struct DispatchEntry { uint32_t addr; uint8_t thumb; void (*fn)(void); };\n"
        "extern \"C\" const DispatchEntry %s[] = {\n",
        names.header, names.table_symbol);
    for (const auto& fn : funcs) {
        std::fprintf(f, "    {0x%08Xu, %uu, %s},\n",
                     fn.addr,
                     fn.mode == CpuMode::Thumb ? 1u : 0u,
                     fn.name.c_str());
    }
    std::fprintf(f,
        "};\n"
        "extern \"C\" const unsigned %s = %zu;\n",
        names.table_len_symbol, funcs.size());
    std::fclose(f);
}

// Emit a sorted address->name map for the runtime debugger. This is what
// turns raw `pc=0x08004260` in trace dumps / dispatch-miss reports / TCP
// into `pc=0x08004260 <UpdateAnimationVariableFrames+0x10>`. Names come
// from the decomp seeds where known, else the generated tfunc_/afunc_.
// The runtime links these via WEAK externs (src/armv4t/symbol_lookup.cpp),
// so a binary without a generated map degrades gracefully to no names.
// Cart mode emits symbol_map.cpp/kGbaSymbolMap; BIOS mode emits
// bios_symbol_map.cpp/kGbaBiosSymbolMap.
void write_symbol_map(const std::string& dir,
                      const std::vector<Function>& funcs,
                      bool bios_mode) {
    const char* file = bios_mode ? "bios_symbol_map.cpp" : "symbol_map.cpp";
    const char* tab  = bios_mode ? "kGbaBiosSymbolMap" : "kGbaSymbolMap";
    const char* cnt  = bios_mode ? "kGbaBiosSymbolMapCount" : "kGbaSymbolMapCount";
    std::string path = dir + "/" + file;
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return;
    }
    // Sort by address (ascending) so the runtime can binary-search for the
    // nearest function entry <= pc. Functions may collide at one address in
    // different modes; either name is fine for a PC->name annotation.
    std::vector<const Function*> sorted;
    sorted.reserve(funcs.size());
    for (const auto& fn : funcs) sorted.push_back(&fn);
    std::sort(sorted.begin(), sorted.end(),
              [](const Function* a, const Function* b) {
                  return a->addr < b->addr;
              });
    const char* reg = bios_mode ? "gba_symbol_register_bios"
                                 : "gba_symbol_register_cart";
    std::fprintf(f,
        "// AUTO-GENERATED by gba_recompile. DO NOT EDIT.\n"
        "// Address -> function-name map for the runtime debugger\n"
        "// (trace dumps, dispatch-miss reports, TCP `symbol`). Sorted by\n"
        "// address; the runtime binary-searches for the nearest entry <= pc.\n"
        "// A static initializer registers this table with the resolver in\n"
        "// src/armv4t/symbol_lookup.cpp (registration, not weak externs,\n"
        "// because MinGW PE-COFF weak symbols are unreliable from archives).\n\n"
        "#include <cstdint>\n\n"
        "struct GbaSymbol { uint32_t addr; const char* name; };\n"
        "extern \"C\" void %s(const GbaSymbol* tab, unsigned count);\n\n"
        "static const GbaSymbol %s[] = {\n",
        reg, tab);
    for (const Function* fn : sorted) {
        std::fprintf(f, "    {0x%08Xu, \"%s\"},\n", fn->addr, fn->name.c_str());
    }
    std::fprintf(f,
        "};\n"
        "static const unsigned %s = %zu;\n\n"
        "namespace {\n"
        "struct GbaSymbolMapInstaller {\n"
        "    GbaSymbolMapInstaller() { %s(%s, %s); }\n"
        "};\n"
        "static GbaSymbolMapInstaller g_gba_symbol_map_installer;\n"
        "}  // namespace\n",
        cnt, sorted.size(), reg, tab, cnt);
    std::fclose(f);
}

void write_body(const std::string& dir,
                const std::vector<Function>& funcs,
                const uint8_t* rom, std::size_t rom_size,
                uint32_t rom_base,
                const OutputNames& names) {
    std::unordered_map<uint64_t, std::string> name_by_key;
    for (const auto& fn : funcs) {
        uint64_t key = (static_cast<uint64_t>(fn.addr) << 1u) |
            (fn.mode == CpuMode::Thumb ? 1u : 0u);
        name_by_key[key] = fn.name;
    }

    std::string path = dir + "/" + names.body;
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return;
    }
    std::fprintf(f,
        "// AUTO-GENERATED by gba_recompile. DO NOT EDIT.\n"
        "//\n"
        "// Each guest function is lowered to a void C function whose\n"
        "// body operates on g_cpu, bus_*, arm_shift_*, arm_set_*, and\n"
        "// runtime_dispatch (see src/armv4t/runtime_arm.h). A dispatch\n"
        "// MISS self-heals (interpreter bridge + on-the-fly recompile +\n"
        "// loud log); an IrOp the codegen can't yet lower is a codegen\n"
        "// gap (NOT a miss) and aborts via runtime_unimplemented_op.\n"
        "// See PRINCIPLES.md \"Honest self-healing\" + \"Coverage\n"
        "// honesty is load-bearing\".\n\n"
        "#include \"runtime_arm.h\"\n"
        "#include \"%s\"\n\n",
        names.header);
    for (const auto& fn : funcs) {
        std::fprintf(f,
            "/* 0x%08X  mode=%s  end=0x%08X  branches=%zu%s */\n"
            "void %s(void) {\n",
            fn.addr,
            fn.mode == CpuMode::Thumb ? "thumb" : "arm",
            fn.end_addr,
            fn.direct_branch_targets.size(),
            fn.has_indirect_transfer ? "  indirect" : "",
            fn.name.c_str());
        emit_function_body(f, fn, rom, rom_size, rom_base, name_by_key);
        std::fprintf(f, "}\n\n");
    }
    std::fclose(f);
}

}  // namespace

int main(int argc, char** argv) {
    Cli cli = parse_cli(argc, argv);
    if (!cli.ok) {
        print_usage();
        return (cli.rom_path.empty() && cli.bios_path.empty()) ? 2 : 0;
    }

    std::string err;
    const std::string& input_path = cli.bios_mode ? cli.bios_path
                                                  : cli.rom_path;
    auto rom = read_file(input_path, &err);
    if (rom.empty()) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }

    // ── Optional TOML config ───────────────────────────────────────
    // Per-binary configuration: manual function seeds, data-range
    // exclusions, jump tables, identity hash. See
    // docs/TOML_SCHEMA.md. When present, the binary's SHA-1 is
    // verified against [identity].sha1 before any discovery runs.
    //
    // When present, [program].load_address and [program].entry_pc
    // become the discovery base/entry. CLI --rom-base/--entry remain
    // the no-config path and quick-spike overrides.
    Config cfg;
    bool have_cfg = false;
    if (!cli.config_path.empty()) {
        if (!gbarecomp::load_config(cli.config_path, cfg)) {
            return 1;
        }
        if (!gbarecomp::verify_identity(cfg, rom.data(), rom.size())) {
            return 1;
        }
        gbarecomp::print_config_summary(cfg);
        have_cfg = true;
    }

    const uint32_t effective_rom_base =
        have_cfg ? cfg.program.load_address : cli.rom_base;
    const uint32_t effective_entry =
        have_cfg ? cfg.program.entry_pc : cli.entry;

    FunctionFinder finder(rom.data(), rom.size(), effective_rom_base);

    // ── Apply TOML config to the finder ──────────────────────────
    // Order: data_ranges + excludes first (so seed validation can
    // run against them), then jump_table expansion (which adds
    // both auto-data-ranges for the table bytes AND extra seeds),
    // then extra_func seeds.
    std::size_t jump_table_expanded_seeds = 0;
    if (have_cfg) {
        for (const auto& dr : cfg.data_ranges) {
            finder.add_data_range(dr.start, dr.end, dr.note);
        }
        for (const auto& cc : cfg.code_copies) {
            finder.add_code_copy(cc.runtime_start, cc.source_start,
                                 cc.size, cc.note);
        }
        for (const auto& ex : cfg.exclude_funcs) {
            finder.add_exclude(ex.addr, ex.reason);
        }
        for (const auto& jt : cfg.jump_tables) {
            // Auto-exclude the table's bytes from code decoding.
            const uint32_t table_bytes = jt.count * jt.stride;
            const uint32_t table_end = jt.addr + table_bytes;
            std::string note = "jump_table bytes";
            if (!jt.name.empty()) {
                note += " (" + jt.name + ")";
            }
            finder.add_data_range(jt.addr, table_end, note);

            // Decode each entry, expand to a seed.
            for (uint32_t i = 0; i < jt.count; ++i) {
                const uint32_t entry_pos = jt.addr + i * jt.stride;
                uint32_t target_addr = 0;
                gbarecomp::CpuMode target_mode = gbarecomp::CpuMode::Arm;
                if (jt.format == gbarecomp::JumpTableFormat::Abs32 ||
                    jt.format == gbarecomp::JumpTableFormat::PcrelArm ||
                    jt.format == gbarecomp::JumpTableFormat::PcrelThumb) {
                    const uint32_t raw = finder.read_u32_public(entry_pos);
                    switch (jt.format) {
                    case gbarecomp::JumpTableFormat::Abs32:
                        if (jt.entries_mode ==
                            gbarecomp::JumpTableEntriesMode::Auto) {
                            target_addr = raw & ~uint32_t{1};
                            target_mode = (raw & 1u)
                                ? gbarecomp::CpuMode::Thumb
                                : gbarecomp::CpuMode::Arm;
                        } else {
                            target_addr = raw;
                            target_mode = (jt.entries_mode ==
                                gbarecomp::JumpTableEntriesMode::Thumb)
                                ? gbarecomp::CpuMode::Thumb
                                : gbarecomp::CpuMode::Arm;
                        }
                        break;
                    case gbarecomp::JumpTableFormat::PcrelArm:
                        target_addr = entry_pos +
                            static_cast<int32_t>(raw);
                        target_mode = gbarecomp::CpuMode::Arm;
                        break;
                    case gbarecomp::JumpTableFormat::PcrelThumb:
                        target_addr = entry_pos +
                            static_cast<int32_t>(raw);
                        target_mode = gbarecomp::CpuMode::Thumb;
                        break;
                    default: break;
                    }
                } else if (jt.format == gbarecomp::JumpTableFormat::Abs16) {
                    if (entry_pos + 2u > effective_rom_base + rom.size() ||
                        entry_pos < effective_rom_base) continue;
                    std::size_t off = entry_pos - effective_rom_base;
                    uint16_t v = static_cast<uint16_t>(rom[off])
                        | (static_cast<uint16_t>(rom[off + 1]) << 8);
                    target_addr = v;
                    target_mode = (jt.entries_mode ==
                        gbarecomp::JumpTableEntriesMode::Thumb)
                        ? gbarecomp::CpuMode::Thumb
                        : gbarecomp::CpuMode::Arm;
                }
                // Skip null/empty entries (some tables pad with zeros).
                if (target_addr == 0) continue;

                char buf[96];
                if (!jt.name.empty()) {
                    std::snprintf(buf, sizeof(buf), "%s_%02u",
                                  jt.name.c_str(), i);
                } else {
                    std::snprintf(buf, sizeof(buf),
                                  "jumptab_%08X_%02u", jt.addr, i);
                }
                finder.add_seed(FunctionSeed{
                    target_addr, target_mode, std::string(buf)});
                ++jump_table_expanded_seeds;
            }
        }
        for (const auto& ef : cfg.extra_funcs) {
            finder.add_seed(FunctionSeed{ef.addr, ef.mode, ef.name});
        }
    }

    if (cli.bios_mode) {
        // GBA BIOS exception vector layout (ARM ARM A2.6):
        //   0x00 reset       — power-on entry
        //   0x04 undef       — undefined-instruction trap (skipped)
        //   0x08 SWI         — software interrupt
        //   0x0C prefetch    — prefetch abort (skipped)
        //   0x10 data        — data abort (skipped)
        //   0x14 reserved    — (skipped)
        //   0x18 IRQ         — IRQ entry
        //   0x1C FIQ         — FIQ entry (skipped — GBA doesn't use FIQ)
        // All vectors are ARM-state. Each is a single B instruction
        // whose target the function-finder follows, so we don't need
        // to seed the actual handler bodies.
        finder.add_seed(FunctionSeed{0x00000000u, CpuMode::Arm,
                                      "bios_reset_vector"});
        finder.add_seed(FunctionSeed{0x00000008u, CpuMode::Arm,
                                      "bios_swi_vector"});
        finder.add_seed(FunctionSeed{0x00000018u, CpuMode::Arm,
                                      "bios_irq_vector"});
        std::printf("==> BIOS mode: rom_base=0x00000000 size=%zu seeds=3\n",
                    rom.size());
    } else {
        auto seeds = load_symbols(cli.symbols_path);
        std::printf("==> loaded %zu seed symbols from %s\n",
                    seeds.size(),
                    cli.symbols_path.empty() ? "(none)"
                                              : cli.symbols_path.c_str());
        finder.add_seed(FunctionSeed{effective_entry, CpuMode::Arm,
                                      "start_vector"});
        for (const auto& s : seeds) finder.add_seed(s);
    }
    finder.run(cli.max_functions);

    // Data-range collisions are a hard error per docs/TOML_SCHEMA.md.
    // Report every collision before exiting so the operator can
    // resolve them in one pass.
    const auto& cols = finder.collisions();
    if (!cols.empty()) {
        std::fprintf(stderr,
            "ERROR: %zu control-flow entries into [[data_range]]\n",
            cols.size());
        for (const auto& c : cols) {
            std::fprintf(stderr,
                "  data_range [0x%08X, 0x%08X)%s%s\n"
                "    entered at 0x%08X via %s",
                c.range_start, c.range_end,
                c.range_note.empty() ? "" : "  ",
                c.range_note.c_str(),
                c.flow_target_addr, c.flow_origin_kind.c_str());
            if (!c.flow_origin_name.empty()) {
                std::fprintf(stderr, " \"%s\"", c.flow_origin_name.c_str());
            }
            if (c.flow_origin_addr != 0) {
                std::fprintf(stderr, " in function at 0x%08X",
                             c.flow_origin_addr);
            }
            std::fprintf(stderr, "\n");
        }
        std::fprintf(stderr,
            "Resolve by either:\n"
            "  - shrinking/removing the [[data_range]] covering the "
            "target, or\n"
            "  - marking the offending caller with "
            "[[exclude_func]] if it's a false-positive function.\n");
        return 1;
    }

    const auto& funcs = finder.functions();
    const auto& stats = finder.stats();
    std::printf("==> discovered %zu functions "
                "(arm=%zu thumb=%zu indirect=%zu undefined=%zu "
                "branch_targets=%zu)\n",
                stats.functions_total, stats.functions_arm,
                stats.functions_thumb, stats.indirect_transfer_count,
                stats.undefined_instr_count,
                stats.branch_targets_discovered);

    if (have_cfg) {
        std::printf("[gba_recompile discovery summary]\n");
        std::printf("  discovered_by_walk:    %zu  finder-only\n",
                    stats.discovered_by_walk_only);
        std::printf("  redundant_manual:      %zu  in both — "
                    "TOML carries documentation value\n",
                    stats.redundant_manual);
        std::printf("  manual_seeds_only:     %zu  "
                    "would be lost without TOML\n",
                    stats.manual_seeds_only);
        std::printf("  jump_table_expanded:   %zu  "
                    "seeds from [[jump_table]] decode\n",
                    jump_table_expanded_seeds);
        std::printf("  auto_jump_tables:      %zu  (%zu targets) "
                    "auto-detected, no hint\n",
                    stats.auto_jump_tables,
                    stats.auto_jump_table_targets);
        std::printf("  jt_confirm_events:     %zu  (%zu distinct: emitted %zu, "
                    "overlap %zu, rejected %zu unsized + %zu bound-mismatch)\n",
                    stats.jt_confirmations,
                    stats.auto_jump_tables + stats.jt_overlap_suppressed +
                        stats.jt_rejected_unsized +
                        stats.jt_rejected_bound_mismatch,
                    stats.auto_jump_tables,
                    stats.jt_overlap_suppressed,
                    stats.jt_rejected_unsized,
                    stats.jt_rejected_bound_mismatch);
        std::printf("  data_ranges_honored:   %zu\n",
                    cfg.data_ranges.size() + cfg.jump_tables.size());
        std::printf("  code_copies:           %zu\n",
                    cfg.code_copies.size());
        std::printf("  excluded:              %zu\n",
                    stats.excluded_count);
        std::printf("  TOTAL emitted:         %zu\n",
                    stats.functions_total);
        // Per-table dump so the auto-detected bases can be diffed
        // against the manual ground-truth set (MC-HP-000 validation).
        for (const auto& jt : finder.auto_jump_tables()) {
            std::printf("  auto_jt 0x%08X count=%u stride=%u site=0x%08X "
                        "%s %s\n",
                        jt.base, jt.count, jt.stride, jt.site_pc,
                        jt.interworking ? "BX" : "MOVpc",
                        jt.bounded ? "bounded" : "walked");
        }
    }
    // Flush the discovery summary now so it's observable before the
    // (much slower) codegen pass — lets a measurement run read the
    // numbers without waiting for full generation.
    std::fflush(stdout);

    // Make sure the output dir exists.
#ifdef _WIN32
    std::string mkdir_cmd = "cmd /c mkdir \"" + cli.out_dir + "\" 2>nul";
#else
    std::string mkdir_cmd = "mkdir -p \"" + cli.out_dir + "\"";
#endif
    std::system(mkdir_cmd.c_str());

    OutputNames names = names_for_mode(cli.bios_mode);
    if (cli.emit_symbol_map) write_symbol_map(cli.out_dir, funcs, cli.bios_mode);
    write_header(cli.out_dir, funcs, names);
    write_body(cli.out_dir, funcs, rom.data(), rom.size(), effective_rom_base,
               names);
    write_dispatch_table(cli.out_dir, funcs, names);
    std::printf("==> wrote %s/{%s, %s, %s}\n",
                cli.out_dir.c_str(),
                names.header, names.body, names.dispatch);

    return 0;
}
