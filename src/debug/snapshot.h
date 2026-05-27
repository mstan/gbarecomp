// snapshot.h — save-state container + per-subsystem serialization.
//
// Per PRINCIPLES.md "Control Flow Semantics": save/load restores guest
// CPU + memory + devices at a well-defined generated-code resume
// boundary. It NEVER serializes host C-stack state.
//
// The resume boundary is the gap between two runtime step_once() calls:
// step_once() runs runtime_dispatch(g_cpu.R[15]), which returns with the
// host C stack fully unwound and g_cpu.R[15] pointing at the next guest
// PC. At that point g_cpu (plus the host-side call-return stack, device
// state, and memory) fully describes where execution resumes. Snapshots
// are taken ONLY at that boundary — TCP commands run between frame steps,
// and the host-window hotkey is sampled at the top of a loop iteration.
//
// Layering note: SnapshotWriter / SnapshotReader are header-only so the
// gba/ subsystem .cpp files can use them without depending on the debug
// static library (gbarecomp_gba must not link gbarecomp_debug). The
// orchestrator (save_state / load_state) lives in snapshot.cpp.

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace gba {
class GbaBus;
class GbaPpu;
}  // namespace gba

namespace gbarecomp::debug {

// Bump when any subsystem's serialized layout changes. load_state
// refuses a blob whose version != kSnapshotVersion (savestates are
// debug-loop artifacts; we don't migrate old formats).
constexpr uint32_t kSnapshotVersion = 1;

// ── SnapshotWriter ─────────────────────────────────────────────────
// Append-only little-endian byte sink. Header-only.
class SnapshotWriter {
public:
    void u8(uint8_t v)  { buf_.push_back(v); }
    void u16(uint16_t v) { u8(v & 0xFF); u8((v >> 8) & 0xFF); }
    void u32(uint32_t v) { u16(v & 0xFFFF); u16((v >> 16) & 0xFFFF); }
    void u64(uint64_t v) { u32(v & 0xFFFFFFFFu); u32((v >> 32) & 0xFFFFFFFFu); }
    void i8(int8_t v)   { u8(static_cast<uint8_t>(v)); }
    void boolean(bool v) { u8(v ? 1u : 0u); }

    void bytes(const void* p, std::size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        buf_.insert(buf_.end(), b, b + n);
    }

    const std::vector<uint8_t>& buffer() const { return buf_; }
    std::size_t size() const { return buf_.size(); }

private:
    std::vector<uint8_t> buf_;
};

// ── SnapshotReader ─────────────────────────────────────────────────
// Bounds-checked little-endian cursor. Any read past the end sets the
// failure flag and returns zero/false; callers check ok() at the end
// rather than every read. Header-only.
class SnapshotReader {
public:
    SnapshotReader(const uint8_t* data, std::size_t len)
        : cur_(data), end_(data + len) {}

    uint8_t u8() {
        if (cur_ + 1 > end_) { fail(); return 0; }
        return *cur_++;
    }
    uint16_t u16() { uint16_t lo = u8(); uint16_t hi = u8(); return lo | (hi << 8); }
    uint32_t u32() { uint32_t lo = u16(); uint32_t hi = u16(); return lo | (hi << 16); }
    uint64_t u64() {
        uint64_t lo = u32(); uint64_t hi = u32();
        return lo | (hi << 32);
    }
    int8_t i8() { return static_cast<int8_t>(u8()); }
    bool boolean() { return u8() != 0; }

    void bytes(void* p, std::size_t n) {
        if (cur_ + n > end_) { fail(); std::memset(p, 0, n); return; }
        std::memcpy(p, cur_, n);
        cur_ += n;
    }

    // Skip n bytes (used to tolerate trailing data in a section).
    void skip(std::size_t n) {
        if (cur_ + n > end_) { fail(); return; }
        cur_ += n;
    }

    bool ok() const { return ok_; }
    std::size_t remaining() const { return static_cast<std::size_t>(end_ - cur_); }

private:
    void fail() { ok_ = false; }
    const uint8_t* cur_;
    const uint8_t* end_;
    bool ok_ = true;
};

// ── Orchestrator ───────────────────────────────────────────────────
// Everything the orchestrator needs to capture / restore the full
// machine. The bus + ppu are the live objects (their pointers are NOT
// serialized — they stay wired across a restore). The counter pointers
// carry host-loop continuity (steps / cycles / frame index) so a
// restored session reports coherent numbers; they are advisory, not
// guest state. rom_sha1 is the 40-hex gate: load refuses a blob whose
// stored ROM hash differs.
struct SnapshotContext {
    gba::GbaBus* bus = nullptr;
    gba::GbaPpu* ppu = nullptr;
    const char*  rom_sha1 = nullptr;

    uint64_t* taken          = nullptr;
    uint64_t* cycles_elapsed = nullptr;
    uint64_t* vblank_count   = nullptr;
};

// Serialize the full machine state to `path`. MUST be called only at a
// clean dispatch boundary. Returns false (with *err set) on I/O error.
bool save_state(const char* path, const SnapshotContext& ctx, std::string* err);

// Restore from `path`. Returns false (with *err set) on I/O error,
// bad magic, version mismatch, ROM-hash mismatch, or truncation.
bool load_state(const char* path, const SnapshotContext& ctx, std::string* err);

}  // namespace gbarecomp::debug
