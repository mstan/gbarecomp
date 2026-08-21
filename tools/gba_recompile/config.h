// config.h — TOML config loader for gba_recompile.
//
// One TOML file per binary, loaded via --config <path>. See
// gbarecomp/docs/TOML_SCHEMA.md for the format.
//
// The config supplements (never replaces) the function-finder's
// automated walk. Manual entries deduplicate against discovered
// ones; data ranges hard-exclude bytes; jump tables auto-expand
// into per-target extra_func equivalents.
//
// Power-user contract: entries are not validated for correctness.
// Only structural contradictions abort.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "function_finder.h"

namespace gbarecomp {

enum class JumpTableFormat : uint8_t {
    Abs32 = 0,
    Abs16 = 1,
    PcrelArm = 2,
    PcrelThumb = 3,
};

enum class JumpTableEntriesMode : uint8_t {
    Arm = 0,
    Thumb = 1,
    Auto = 2,    // bit 0 of entry encodes mode (interworking)
};

struct ConfigProgram {
    std::string name;
    std::string id;
    uint32_t    load_address = 0;
    uint32_t    size = 0;
    uint32_t    entry_pc = 0;
    // Speculative PC-relative literal harvesting is useful for early
    // exploration, but can misclassify data as code. Targets that drive a
    // strict static corpus can disable it and add only observed callbacks.
    bool        speculative_literal_harvest = true;
    // Optional half-open executable ROM range for AOT pointer-table
    // discovery. Zero/empty disables it. Reachable table bases are validated
    // with compiler-shaped prologues before address-taken leaves are seeded.
    uint32_t    aot_scan_start = 0;
    uint32_t    aot_scan_end = 0;
    // Emit native resume entries for every instruction boundary so IRQ/SWI
    // returns never require interpreter or self-heal fallback.
    bool        static_resume_all = false;
    // Number of deterministic C++ translation units used for emitted guest
    // bodies. Zero selects the adaptive cartridge default. Cartridge output
    // is never monolithic; BIOS output remains one small translation unit.
    uint32_t    codegen_shards = 0;
};

struct ConfigIdentity {
    std::string sha1;       // required
    std::string md5;        // optional, empty if not declared
};

struct ConfigExtraFunc {
    uint32_t    addr = 0;
    // Optional immutable ROM backing for a relocated RAM entry. Discovery
    // propagates this source/runtime bias through its direct CFG edges.
    uint32_t    source_addr = 0;
    CpuMode     mode = CpuMode::Arm;
    std::string name;       // optional; finder generates one if empty
    std::string note;       // documentation only
    // resume = true marks an explicit interior/resume seed: if addr falls
    // inside another function it is rolled up as a mid-function alias entry
    // (alternate dispatch target) instead of fragmenting the host. Used for
    // IRQ/SWI-return resume PCs (e.g. a WaitForVBlank busy-spin interior).
    bool        resume = false;
};

// An explicitly reviewed interruptible host span. Every aligned instruction
// after start and before end becomes an interior resume alias.
struct ConfigResumeRange {
    uint32_t    start = 0;
    uint32_t    end = 0;
    CpuMode     mode = CpuMode::Arm;
    std::string note;
};

// Exact THUMB data-processing instruction whose immediate operand may be
// replaced by a game-owned runtime enhancement callback. Merely declaring a
// callback in the runner is insufficient: generated code contains the
// chokepoint only at these reviewed PCs.
struct ConfigThumbAluImmediateOverride {
    uint32_t    addr = 0;
    std::string note;
};

struct ConfigAluImmediateOverride {
    uint32_t    addr = 0;
    std::string note;
};

// Reviewed function entry eligible for a trusted native replacement callback.
// This does not seed discovery: codegen audits it against actual emitted roots.
struct ConfigModFunctionHook {
    uint32_t    addr = 0;
    CpuMode     mode = CpuMode::Arm;
    std::string note;
};

struct ConfigDataRange {
    uint32_t    start = 0;
    uint32_t    end = 0;    // [start, end)
    std::string note;
};

struct ConfigCodeCopy {
    uint32_t    runtime_start = 0;
    uint32_t    source_start = 0;
    uint32_t    size = 0;
    std::string name;
    std::string note;
};

struct ConfigJumpTable {
    uint32_t            addr = 0;
    uint32_t            stride = 0;
    uint32_t            count = 0;
    JumpTableFormat     format = JumpTableFormat::Abs32;
    JumpTableEntriesMode entries_mode = JumpTableEntriesMode::Arm;
    std::string         name;
    std::string         note;
};

struct ConfigExcludeFunc {
    uint32_t    addr = 0;
    std::string reason;     // required
};

struct Config {
    std::string             source_path;  // path the config was loaded from
    ConfigProgram           program;
    ConfigIdentity          identity;
    std::vector<ConfigExtraFunc>    extra_funcs;
    std::vector<ConfigResumeRange>  resume_ranges;
    std::vector<ConfigThumbAluImmediateOverride>
        thumb_alu_immediate_overrides;
    std::vector<ConfigAluImmediateOverride> alu_immediate_overrides;
    std::vector<ConfigModFunctionHook> mod_function_hooks;
    std::vector<ConfigDataRange>    data_ranges;
    std::vector<ConfigCodeCopy>     code_copies;
    std::vector<ConfigJumpTable>    jump_tables;
    std::vector<ConfigExcludeFunc>  exclude_funcs;
};

// Load a TOML config from `path`. On success returns true and
// populates `out`. On parse/structural error returns false and
// writes a human-readable diagnostic to stderr.
//
// Identity hash verification is NOT performed here — call
// verify_identity() against the actual binary bytes after loading.
bool load_config(const std::string& path, Config& out);

// Merge a supplemental TOML overlay on top of an already-loaded base
// config. An overlay carries only the tables that describe bytes
// ([[extra_func]], [[data_range]], [[code_copy]], [[jump_table]], ...);
// declaring [program] is an error, and [identity] — when present — must
// match the base's sha1, which is what binds a generated overlay to the
// exact binary it was derived from.
//
// The base wins every conflict: an [[extra_func]] the base already
// declares at the same (addr, mode) keeps its reviewed name and note, and
// an overlay entry the base excludes or marks as data is dropped with a
// warning. This is the same precedence the function finder applies
// downstream, where the first seed for an (addr, mode) supplies the name.
//
// Used for machine-generated symbol overlays (see
// tools/symbol_import/import_decomp_symbols.py and
// docs/SYMBOL_OVERLAY.md) so importing symbols never means replacing, or
// writing to, a game's hand-authored game.toml.
bool load_config_overlay(const std::string& path, Config& base);

// Verify the config's identity hashes against the binary bytes.
// Returns true on match; false and prints a diagnostic on
// mismatch. If `identity.sha1` is empty the check fails (sha1
// is required by the schema).
bool verify_identity(const Config& cfg,
                      const uint8_t* binary, std::size_t binary_len);

// Print a short summary of the loaded config to stdout. Called
// after load + verify so the operator sees what's in effect.
void print_config_summary(const Config& cfg);

}  // namespace gbarecomp
