// overlay_emit.cpp — see overlay_emit.h.

#include "overlay_emit.h"

#include <cstdio>
#include <unordered_map>

#include "overlay_abi.h"      // GBA_OVERLAY_ABI_VERSION
#include "function_finder.h"  // FunctionFinder, FunctionSeed, Function, CpuMode
#include "emit_function.h"    // emit_function_body_str

namespace gbarecomp {

std::string emit_overlay_c(uint32_t pc, bool thumb,
                           const uint8_t* bytes, std::size_t size,
                           uint32_t base, uint32_t* out_end) {
    // Discover the function rooted at `pc` against the live code image. The
    // walk sets end_addr to the function's own CFG extent (function_finder
    // line ~1475), clamped down only by a discovered neighbour. Capped low:
    // we only need THIS entry — its callees self-heal independently on first
    // hit (single-candidate, one function per DLL).
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
    if (!target) return std::string();
    if (out_end) *out_end = target->end_addr;

    // Empty names map → every B/BL lowers to runtime_dispatch(target); no
    // cross-DLL direct calls (all inter-function flow routes back through the
    // host dispatcher).
    std::unordered_map<uint64_t, std::string> empty_names;
    std::string body = emit_function_body_str(*target, bytes, size, base,
                                              empty_names);

    char hdr[160];
    std::string out;
    out += "// AUTO-GENERATED Stage-2 self-heal overlay. Do not edit.\n";
    std::snprintf(hdr, sizeof(hdr),
                  "// function 0x%08X mode=%s end=0x%08X\n",
                  pc, thumb ? "thumb" : "arm", target->end_addr);
    out += hdr;
    out += "#include \"overlay_runtime_arm.h\"\n\n";
    // The overlay body is plain C (g_cpu.R[..], C-style casts, C99 mixed decls)
    // and the ABI/shim headers are __cplusplus-guarded, so this file compiles
    // under BOTH g++ (the dev/release producer, C++) and tcc (the bundled,
    // toolchain-free producer, C). OVL_EXPORT gives the three entry points C
    // linkage under C++ and DLL-exports them so the loader's GetProcAddress
    // resolves them: gcc would otherwise need -Wl,--export-all-symbols and tcc
    // has no such flag, so the export attribute is on the symbols themselves.
    // Keep this dual-toolchain-clean: no C++-only syntax below.
    out += "#ifdef _WIN32\n"
           "#define OVL_DLLEXPORT __declspec(dllexport)\n"
           "#else\n"
           "#define OVL_DLLEXPORT __attribute__((visibility(\"default\")))\n"
           "#endif\n"
           "#ifdef __cplusplus\n"
           "#define OVL_EXPORT extern \"C\" OVL_DLLEXPORT\n"
           "#else\n"
           "#define OVL_EXPORT OVL_DLLEXPORT\n"
           "#endif\n\n";
    // g_ovl is a plain (non-extern-C) global, matching the shim's extern decl;
    // overlay_init() stores the host callbacks into it. It is TU-local in
    // practice (only the same file's shim thunks read it), so its linkage name
    // is immaterial across C/C++.
    out += "const GbaOverlayCallbacks* g_ovl = 0;\n";
    std::snprintf(hdr, sizeof(hdr),
                  "OVL_EXPORT uint32_t overlay_abi(void) { return %uu; }\n",
                  static_cast<unsigned>(GBA_OVERLAY_ABI_VERSION));
    out += hdr;
    out += "OVL_EXPORT void overlay_init(const GbaOverlayCallbacks* cb) { g_ovl = cb; }\n\n";
    std::snprintf(hdr, sizeof(hdr), "OVL_EXPORT void func_%08X(void) {\n", pc);
    out += hdr;
    out += body;
    out += "}\n";
    return out;
}

}  // namespace gbarecomp
