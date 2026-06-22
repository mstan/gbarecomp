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
#include "arm_sljit.h"     // armv4t::sljit_supports (heal-coverage measurement)
#include "arm_decode.h"    // armv4t::ArmDecoder
#include "thumb_decode.h"  // armv4t::ThumbDecoder
#include "arm_ir.h"        // armv4t::Instr, ir_op_name
#include "emit_function.h"
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
// Emitter — the per-function lowering now lives in
// src/recompile/emit_function.{h,cpp} (gbarecomp::emit_function_body),
// shared with the Stage-2 runtime self-healing recompiler. write_body()
// below calls it via the FILE* wrapper.
// ─────────────────────────────────────────────────────────────────────

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

// ── C++ identifier sanitization ──────────────────────────────────────
//
// Each guest function is emitted as `void <name>(void)`, and that same
// <name> is referenced from the header decl, the dispatch table, and
// inter-function call sites in the bodies. The name comes from the decomp
// seed where known (readable codegen) — but a seed name is just a symbol-
// table string and can be a C++ keyword, the std namespace ("std" is a real
// libc routine in pokefirered at 0x081E8108), or a duplicate of another
// local symbol. Any of those breaks the C++ compile. We KEEP the readable
// name wherever it is already a unique, valid, non-reserved identifier and
// only disambiguate the offenders by appending the (unique) guest address,
// so the generated diff stays minimal and the debug symbol map still
// matches the emitted code one-for-one.
bool is_cxx_reserved_name(const std::string& s) {
    static const std::unordered_set<std::string> kReserved = {
        // C++ keywords and alternative tokens
        "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand",
        "bitor", "bool", "break", "case", "catch", "char", "char8_t",
        "char16_t", "char32_t", "class", "compl", "concept", "const",
        "consteval", "constexpr", "constinit", "const_cast", "continue",
        "co_await", "co_return", "co_yield", "decltype", "default", "delete",
        "do", "double", "dynamic_cast", "else", "enum", "explicit", "export",
        "extern", "false", "float", "for", "friend", "goto", "if", "inline",
        "int", "long", "mutable", "namespace", "new", "noexcept", "not",
        "not_eq", "nullptr", "operator", "or", "or_eq", "private", "protected",
        "public", "register", "reinterpret_cast", "requires", "return",
        "short", "signed", "sizeof", "static", "static_assert", "static_cast",
        "struct", "switch", "template", "this", "thread_local", "throw",
        "true", "try", "typedef", "typeid", "typename", "union", "unsigned",
        "using", "virtual", "void", "volatile", "wchar_t", "while", "xor",
        "xor_eq",
        // The standard namespace + macros the generated body pulls into scope.
        "std", "NULL",
    };
    return kReserved.count(s) != 0;
}

bool is_valid_c_identifier(const std::string& s) {
    if (s.empty()) return false;
    auto is_ident_start = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
    };
    auto is_ident_cont = [&](char c) {
        return is_ident_start(c) || (c >= '0' && c <= '9');
    };
    if (!is_ident_start(s[0])) return false;
    for (char c : s) {
        if (!is_ident_cont(c)) return false;
    }
    return true;
}

// Rewrite funcs[].name so every name is a valid, unique C++ identifier that
// cannot collide with a host symbol. Deterministic: processes in vector
// order, uses the guest address (unique per function) as the disambiguating
// suffix. Mutates in place.
//
// Cart functions are emitted `extern "C"` into the SAME global symbol
// namespace as the host program — libc, the GCC builtins, and the gbarecomp
// runtime API. pokefirered's AGB libc contributes real symbols named
// `memcpy`, `memset`, `_exit`, `strcpy`, `abort`, `fflush`, … and a guest
// function emitted under one of those names would HIJACK the host's symbol
// at link: every host `memcpy`/`memset` call — including the C-runtime
// startup that runs *before* our constructors — would jump into guest code
// operating on an uninitialised g_cpu, which is exactly the pre-main hang
// FireRed showed. (Minish Cap never tripped this: its symbol set contains no
// libc names.) A reserved `gf_` prefix the host never uses makes the
// collision impossible by construction, and subsumes C++ keyword / `std`
// namespace collisions for free. BIOS functions are NOT prefixed — they
// dispatch through a separate table and their seed names (reset/swi/irq)
// don't collide — but still get keyword/duplicate hardening.
void sanitize_function_identifiers(std::vector<Function>& funcs,
                                   bool prefix_guest) {
    std::unordered_set<std::string> used;
    used.reserve(funcs.size() * 2);
    int renamed = 0;
    for (auto& fn : funcs) {
        std::string ident = fn.name;
        // Coerce to syntactically valid identifier chars. Seed names are
        // normally already valid; this is defensive against odd symbol-table
        // strings.
        if (!is_valid_c_identifier(ident)) {
            std::string fixed;
            fixed.reserve(ident.size() + 1);
            for (char c : ident) {
                bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '_';
                fixed.push_back(ok ? c : '_');
            }
            if (fixed.empty() ||
                !((fixed[0] >= 'A' && fixed[0] <= 'Z') ||
                  (fixed[0] >= 'a' && fixed[0] <= 'z') || fixed[0] == '_')) {
                fixed.insert(fixed.begin(), '_');
            }
            ident = fixed;
        }
        if (prefix_guest) {
            // Host-collision-proof by construction (also neutralises keywords
            // and the std namespace).
            ident = "gf_" + ident;
        } else if (is_cxx_reserved_name(ident)) {
            // BIOS path: keep the name but disambiguate a reserved word.
            char suffix[16];
            std::snprintf(suffix, sizeof(suffix), "_%08X", fn.addr);
            ident += suffix;
        }
        // Guarantee uniqueness against any earlier function (duplicate local
        // symbols, or a prefixed name that coincides with another) by
        // appending the unique guest address.
        if (used.count(ident)) {
            char suffix[16];
            std::snprintf(suffix, sizeof(suffix), "_%08X", fn.addr);
            ident += suffix;
            // Residual-collision guard (e.g. ARM + THUMB at one address).
            while (used.count(ident)) ident += "_";
        }
        if (ident != fn.name) ++renamed;
        used.insert(ident);
        fn.name = std::move(ident);
    }
    if (renamed) {
        std::printf("  sanitized %d function name(s) -> collision-proof C++ "
                    "identifiers%s\n", renamed,
                    prefix_guest ? " (gf_ prefix)" : " (reserved/duplicate)");
    }
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

    // Control-flow into a data_range. Two very different cases:
    //
    //  * AUTHORITATIVE range (readelf [[data_range]] or a manual
    //    [[jump_table]] hint): the operator asserted these bytes are data,
    //    so flow into them is a genuine contradiction — HARD ERROR, and the
    //    human must resolve it (shrink the range / exclude the caller).
    //
    //  * AUTO jump_table (the finder's own speculative abs32-table guess):
    //    real code can never branch *into* a true code-pointer table, so a
    //    collision here is proof the guess mis-modeled some packed/offset
    //    THUMB switch (e.g. LeafGreen rev1 @ 0x0801A8E8 — 48 case handlers
    //    that each branch to a shared address inside the table bytes, which
    //    readelf confirms ARE data). The data_range itself is correct; only
    //    the decoded branches are artifacts. Downgrade to a WARNING: keep
    //    the bytes as data and let the residual indirect branches resolve
    //    through the runtime's honest self-heal path. Never a build failure.
    const auto& cols = finder.collisions();
    std::size_t fatal = 0, auto_jt = 0;
    for (const auto& c : cols) {
        if (c.range_note == "auto jump_table") ++auto_jt; else ++fatal;
    }
    if (auto_jt > 0) {
        std::fprintf(stderr,
            "WARNING: %zu control-flow entries into an auto-detected "
            "jump_table (mis-modeled switch; bytes kept as data, residual "
            "branches self-heal at runtime). Sample:\n", auto_jt);
        std::size_t shown = 0;
        for (const auto& c : cols) {
            if (c.range_note != "auto jump_table") continue;
            if (shown++ >= 3) break;
            std::fprintf(stderr,
                "  [0x%08X,0x%08X) auto jump_table <- 0x%08X via %s",
                c.range_start, c.range_end, c.flow_target_addr,
                c.flow_origin_kind.c_str());
            if (c.flow_origin_addr != 0)
                std::fprintf(stderr, " in fn 0x%08X", c.flow_origin_addr);
            std::fprintf(stderr, "\n");
        }
    }
    if (fatal > 0) {
        std::fprintf(stderr,
            "ERROR: %zu control-flow entries into [[data_range]]\n", fatal);
        for (const auto& c : cols) {
            if (c.range_note == "auto jump_table") continue;
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
        std::printf("  literal_pool_seeds:    %zu kept / %zu PC-rel "
                    "literals (speculative code-pointer harvest)\n",
                    stats.literal_pool_seeds_kept,
                    stats.literal_pool_words_seen);
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

    // Optional: measure sljit heal-coverage over the discovered corpus, then
    // exit (don't emit). GBARECOMP_SLJIT_COVERAGE=1. Reports how many functions
    // emit_function_sljit would heal vs decline + the dominant declined ops +
    // a few demo candidates (fully supported, with a load/store).
    if (std::getenv("GBARECOMP_SLJIT_COVERAGE")) {
        auto rd32 = [&](uint32_t a) -> uint32_t {
            const uint32_t off = a - effective_rom_base;
            if (a < effective_rom_base || off + 4u > rom.size()) return 0u;
            const uint8_t* p = rom.data() + off;
            return p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
                   (uint32_t(p[3]) << 24);
        };
        auto rd16 = [&](uint32_t a) -> uint16_t {
            const uint32_t off = a - effective_rom_base;
            if (a < effective_rom_base || off + 2u > rom.size()) return 0u;
            const uint8_t* p = rom.data() + off;
            return static_cast<uint16_t>(p[0] | (uint16_t(p[1]) << 8));
        };
        std::size_t supported = 0, leaf_supported = 0;
        std::unordered_map<std::string, int> decline;
        std::vector<const Function*> demo;
        for (const auto& f : funcs) {
            const bool thumb = (f.mode == CpuMode::Thumb);
            bool ok = (f.end_addr > f.addr);
            bool has_call = false, has_mem = false;
            std::string first_bad;
            for (uint32_t a = f.addr; a < f.end_addr; a += thumb ? 2u : 4u) {
                armv4t::Instr in = thumb ? armv4t::ThumbDecoder::decode(rd16(a), a)
                                         : armv4t::ArmDecoder::decode(rd32(a), a);
                if (in.op == armv4t::IrOp::BL || in.op == armv4t::IrOp::BL_suffix)
                    has_call = true;
                switch (in.op) {
                    case armv4t::IrOp::LDR: case armv4t::IrOp::STR:
                    case armv4t::IrOp::LDRB: case armv4t::IrOp::STRB:
                    case armv4t::IrOp::LDRH: case armv4t::IrOp::STRH:
                        has_mem = true; break;
                    default: break;
                }
                if (ok) {
                    const char* why = armv4t::sljit_decline_reason(in);
                    if (why) {
                        ok = false;
                        first_bad = std::string(armv4t::ir_op_name(in.op)) +
                                    " [" + why + "]";
                    }
                }
            }
            if (ok) {
                ++supported;
                if (!has_call) ++leaf_supported;
                if (demo.size() < 12 && has_mem && !has_call) demo.push_back(&f);
            } else if (!first_bad.empty()) {
                decline[first_bad]++;
            }
        }
        const std::size_t total = funcs.size();
        std::printf("\n[sljit heal-coverage over %zu discovered functions]\n", total);
        std::printf("  fully supported (would heal): %zu (%.1f%%)\n",
                    supported, total ? 100.0 * double(supported) / double(total) : 0.0);
        std::printf("  ... leaf (no BL call):        %zu\n", leaf_supported);
        std::printf("  top first-unsupported-op classes:\n");
        std::vector<std::pair<std::string, int>> v(decline.begin(), decline.end());
        std::sort(v.begin(), v.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        for (std::size_t i = 0; i < v.size() && i < 20; ++i)
            std::printf("    %-28s %d\n", v[i].first.c_str(), v[i].second);
        std::printf("  demo candidates (fully supported, leaf, has load/store):\n");
        for (const Function* f : demo)
            std::printf("    0x%08X %-5s len=0x%X\n", f->addr,
                        f->mode == CpuMode::Thumb ? "thumb" : "arm",
                        f->end_addr - f->addr);
        std::fflush(stdout);
        return 0;
    }

    // Make sure the output dir exists.
#ifdef _WIN32
    std::string mkdir_cmd = "cmd /c mkdir \"" + cli.out_dir + "\" 2>nul";
#else
    std::string mkdir_cmd = "mkdir -p \"" + cli.out_dir + "\"";
#endif
    std::system(mkdir_cmd.c_str());

    // Sanitize function names to valid, unique, non-reserved C++ identifiers
    // before emission. Work on a copy so the finder's canonical vector (and
    // anything that consults it by reference) is untouched. The SAME sanitized
    // vector feeds every emitter, so the header decl, body def, dispatch table,
    // inter-function calls, and debug symbol map all agree on each name.
    std::vector<Function> emit_funcs = funcs;
    sanitize_function_identifiers(emit_funcs, /*prefix_guest=*/!cli.bios_mode);

    OutputNames names = names_for_mode(cli.bios_mode);
    if (cli.emit_symbol_map) write_symbol_map(cli.out_dir, emit_funcs, cli.bios_mode);
    write_header(cli.out_dir, emit_funcs, names);
    write_body(cli.out_dir, emit_funcs, rom.data(), rom.size(), effective_rom_base,
               names);
    write_dispatch_table(cli.out_dir, emit_funcs, names);
    std::printf("==> wrote %s/{%s, %s, %s}\n",
                cli.out_dir.c_str(),
                names.header, names.body, names.dispatch);

    return 0;
}
