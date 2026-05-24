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
                        const std::unordered_map<uint32_t, std::string>&
                            func_names_by_addr) {
    const uint32_t step = (fn.mode == CpuMode::Thumb) ? 2u : 4u;
    armv4t::CodegenCtx ctx;
    ctx.names_by_addr = &func_names_by_addr;
    uint32_t pc = fn.addr;
    while (pc < fn.end_addr) {
        if (pc < rom_base) break;
        std::size_t off = pc - rom_base;
        if (off + step > rom_size) break;
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

        std::string line = armv4t::format_ir(ins);
        std::fprintf(f, "    /* %08X  %s */\n", pc, line.c_str());

        bool ni = false;
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
        "// this table.\n\n"
        "#include <cstdint>\n"
        "#include \"%s\"\n\n"
        "struct DispatchEntry { uint32_t addr; void (*fn)(void); };\n"
        "extern \"C\" const DispatchEntry %s[] = {\n",
        names.header, names.table_symbol);
    for (const auto& fn : funcs) {
        std::fprintf(f, "    {0x%08Xu, %s},\n", fn.addr, fn.name.c_str());
    }
    std::fprintf(f,
        "};\n"
        "extern \"C\" const unsigned %s = %zu;\n",
        names.table_len_symbol, funcs.size());
    std::fclose(f);
}

void write_body(const std::string& dir,
                const std::vector<Function>& funcs,
                const uint8_t* rom, std::size_t rom_size,
                uint32_t rom_base,
                const OutputNames& names) {
    std::unordered_map<uint32_t, std::string> name_by_addr;
    for (const auto& fn : funcs) name_by_addr[fn.addr] = fn.name;

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
        "// runtime_dispatch (see src/armv4t/runtime_arm.h). The\n"
        "// interpreter is NOT consulted at runtime — every IrOp that\n"
        "// the codegen can't yet lower aborts via\n"
        "// runtime_unimplemented_op. See PRINCIPLES.md \"Interpreter\n"
        "// is informative, never load-bearing (SHOWSTOPPER)\".\n\n"
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
        emit_function_body(f, fn, rom, rom_size, rom_base, name_by_addr);
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
    // The config's extra_funcs / data_ranges / jump_tables are
    // consumed by the function-finder in a later integration step;
    // for now we just load + verify + summarize so authors get
    // immediate feedback on whether their TOML is well-formed.
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

    FunctionFinder finder(rom.data(), rom.size(), cli.rom_base);

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
                    if (entry_pos + 2u > cli.rom_base + rom.size() ||
                        entry_pos < cli.rom_base) continue;
                    std::size_t off = entry_pos - cli.rom_base;
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
        finder.add_seed(FunctionSeed{cli.entry, CpuMode::Arm,
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
        std::printf("  data_ranges_honored:   %zu\n",
                    cfg.data_ranges.size() + cfg.jump_tables.size());
        std::printf("  excluded:              %zu\n",
                    stats.excluded_count);
        std::printf("  TOTAL emitted:         %zu\n",
                    stats.functions_total);
    }

    // Make sure the output dir exists.
#ifdef _WIN32
    std::string mkdir_cmd = "cmd /c mkdir \"" + cli.out_dir + "\" 2>nul";
#else
    std::string mkdir_cmd = "mkdir -p \"" + cli.out_dir + "\"";
#endif
    std::system(mkdir_cmd.c_str());

    OutputNames names = names_for_mode(cli.bios_mode);
    write_header(cli.out_dir, funcs, names);
    write_body(cli.out_dir, funcs, rom.data(), rom.size(), cli.rom_base,
               names);
    write_dispatch_table(cli.out_dir, funcs, names);
    std::printf("==> wrote %s/{%s, %s, %s}\n",
                cli.out_dir.c_str(),
                names.header, names.body, names.dispatch);

    return 0;
}
