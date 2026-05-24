// gba_recompile — read a GBA ROM, discover functions reachable from
// the entry point + seeded symbols, lower their ARM/THUMB bodies
// through the armv4t IR into C++ source, and write the result to a
// per-game `generated/` directory.
//
// This first cut produces an output file that compiles to a
// SCAFFOLD: function bodies are present as decoded-instruction
// comments + per-op TODO markers, and not-yet-lowered IrOp variants
// emit `runtime_unimplemented_op(...)` calls. The architecture and
// loop are real; the per-op lowering is filled in incrementally in
// follow-up sessions.
//
// CLI:
//   gba_recompile --rom <rom.gba>
//                 --entry 0x080000C0
//                 [--symbols <imported_symbols.tsv>]
//                 [--out <out_dir>]
//                 [--rom-base 0x08000000]
//                 [--max-functions 4096]
//
// Output:
//   <out_dir>/recompiled.cpp   (single file for first cut)
//   <out_dir>/recompiled.h     (forward decls + dispatch table)

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

#include "function_finder.h"
#include "arm_decode.h"
#include "thumb_decode.h"
#include "arm_ir.h"

using gbarecomp::CpuMode;
using gbarecomp::Function;
using gbarecomp::FunctionFinder;
using gbarecomp::FunctionSeed;

namespace {

struct Cli {
    std::string rom_path;
    std::string symbols_path;
    std::string out_dir = "generated";
    uint32_t entry = 0x080000C0u;
    uint32_t rom_base = 0x08000000u;
    std::size_t max_functions = 4096;
    bool ok = true;
};

void print_usage() {
    std::printf(
        "gba_recompile --rom <path> [--entry HEX] [--symbols TSV]\n"
        "              [--out DIR] [--rom-base HEX]\n"
        "              [--max-functions N]\n"
        "\n"
        "Discovers functions reachable from the entry + symbol seeds\n"
        "and writes generated C++ source to <out>/recompiled.{cpp,h}.\n");
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
        else if (a == "--entry")        parse_hex(next(), c.entry);
        else if (a == "--symbols")      c.symbols_path = next() ? argv[i] : "";
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
    if (c.rom_path.empty()) {
        std::fprintf(stderr, "missing --rom\n");
        c.ok = false;
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

        // First-cut lowering: only direct calls (BL) and direct
        // unconditional branches (B) get real C. Everything else is
        // a TODO marker so the file at least scaffolds the loop.
        if (ins.op == armv4t::IrOp::BL && !ins.is_indirect) {
            auto it = func_names_by_addr.find(ins.branch_target);
            if (it != func_names_by_addr.end()) {
                std::fprintf(f, "    %s();\n", it->second.c_str());
            } else {
                std::fprintf(f, "    runtime_dispatch(0x%08Xu);\n",
                             ins.branch_target);
            }
        } else if (ins.op == armv4t::IrOp::B && !ins.is_indirect &&
                   ins.cond == armv4t::Cond::AL) {
            auto it = func_names_by_addr.find(ins.branch_target);
            if (it != func_names_by_addr.end()) {
                std::fprintf(f, "    %s(); return;\n",
                             it->second.c_str());
            } else {
                std::fprintf(f, "    runtime_dispatch(0x%08Xu); return;\n",
                             ins.branch_target);
            }
        } else if (ins.is_return) {
            std::fprintf(f, "    return;  /* return-shaped */\n");
        } else {
            std::fprintf(f,
                "    runtime_unimplemented_op(\"%s\", 0x%08Xu);\n",
                armv4t::ir_op_name(ins.op), pc);
        }
        pc += step;
    }
}

void write_header(const std::string& dir,
                   const std::vector<Function>& funcs) {
    std::string path = dir + "/recompiled.h";
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return;
    }
    std::fprintf(f,
        "// AUTO-GENERATED by gba_recompile. DO NOT EDIT.\n"
        "// Forward declarations for the recompiled cart code.\n"
        "//\n"
        "// Total functions: %zu\n"
        "\n"
        "#pragma once\n\n"
        "extern \"C\" {\n",
        funcs.size());
    for (const auto& fn : funcs) {
        std::fprintf(f, "void %s(void);  /* 0x%08X %s */\n",
                     fn.name.c_str(), fn.addr,
                     fn.mode == CpuMode::Thumb ? "thumb" : "arm");
    }
    std::fprintf(f, "}  /* extern \"C\" */\n");
    std::fclose(f);
}

void write_dispatch_table(const std::string& dir,
                           const std::vector<Function>& funcs) {
    std::string path = dir + "/dispatch_table.cpp";
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f,
        "// AUTO-GENERATED by gba_recompile. DO NOT EDIT.\n"
        "// Dispatch table: maps recompiled function addresses to\n"
        "// the corresponding generated C function pointer. The\n"
        "// runtime calls runtime_dispatch(addr) which binary-searches\n"
        "// this table.\n\n"
        "#include <cstdint>\n"
        "#include \"recompiled.h\"\n\n"
        "struct DispatchEntry { uint32_t addr; void (*fn)(void); };\n"
        "extern \"C\" const DispatchEntry kDispatchTable[] = {\n");
    for (const auto& fn : funcs) {
        std::fprintf(f, "    {0x%08Xu, %s},\n", fn.addr, fn.name.c_str());
    }
    std::fprintf(f,
        "};\n"
        "extern \"C\" const unsigned kDispatchTableLen = %zu;\n",
        funcs.size());
    std::fclose(f);
}

void write_body(const std::string& dir,
                const std::vector<Function>& funcs,
                const uint8_t* rom, std::size_t rom_size,
                uint32_t rom_base) {
    std::unordered_map<uint32_t, std::string> name_by_addr;
    for (const auto& fn : funcs) name_by_addr[fn.addr] = fn.name;

    std::string path = dir + "/recompiled.cpp";
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return;
    }
    std::fprintf(f,
        "// AUTO-GENERATED by gba_recompile. DO NOT EDIT.\n"
        "//\n"
        "// First-cut scaffold output. Function bodies are decoded-\n"
        "// instruction comments + per-op TODOs; only direct calls\n"
        "// (BL) and direct unconditional branches (B) get real C.\n"
        "// Everything else lowers to runtime_unimplemented_op so\n"
        "// the runner can route to interpreter fallback (or abort)\n"
        "// until the per-op codegen lowering catches up.\n\n"
        "#include \"runtime_arm.h\"\n"
        "#include \"recompiled.h\"\n\n");
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
        return cli.rom_path.empty() ? 2 : 0;
    }

    std::string err;
    auto rom = read_file(cli.rom_path, &err);
    if (rom.empty()) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }

    auto seeds = load_symbols(cli.symbols_path);
    std::printf("==> loaded %zu seed symbols from %s\n",
                seeds.size(),
                cli.symbols_path.empty() ? "(none)" : cli.symbols_path.c_str());

    FunctionFinder finder(rom.data(), rom.size(), cli.rom_base);
    finder.add_seed(FunctionSeed{cli.entry, CpuMode::Arm, "start_vector"});
    for (const auto& s : seeds) finder.add_seed(s);
    finder.run(cli.max_functions);

    const auto& funcs = finder.functions();
    const auto& stats = finder.stats();
    std::printf("==> discovered %zu functions "
                "(arm=%zu thumb=%zu indirect=%zu undefined=%zu "
                "branch_targets=%zu)\n",
                stats.functions_total, stats.functions_arm,
                stats.functions_thumb, stats.indirect_transfer_count,
                stats.undefined_instr_count,
                stats.branch_targets_discovered);

    // Make sure the output dir exists.
#ifdef _WIN32
    std::string mkdir_cmd = "cmd /c mkdir \"" + cli.out_dir + "\" 2>nul";
#else
    std::string mkdir_cmd = "mkdir -p \"" + cli.out_dir + "\"";
#endif
    std::system(mkdir_cmd.c_str());

    write_header(cli.out_dir, funcs);
    write_body(cli.out_dir, funcs, rom.data(), rom.size(), cli.rom_base);
    write_dispatch_table(cli.out_dir, funcs);
    std::printf("==> wrote %s/recompiled.{h,cpp} + dispatch_table.cpp\n",
                cli.out_dir.c_str());

    return 0;
}
