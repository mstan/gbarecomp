// tcp_debug_server.cpp — see tcp_debug_server.h.

#include "tcp_debug_server.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
   using socket_t = SOCKET;
#  define CLOSESOCK closesocket
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
   using socket_t = int;
#  define INVALID_SOCKET (-1)
#  define SOCKET_ERROR (-1)
#  define CLOSESOCK ::close
#endif

// Debug PC breakpoint global (defined in runtime_bus_bridge.cpp). The
// set_break_pc command writes it; runtime_should_yield() consults it.
extern "C" uint32_t g_runtime_break_pc;

#include "cpu_state.h"
#include "gba_audio.h"
#include "gba_bus.h"
#include "gba_m4a.h"
#include "gba_io.h"
#include "gba_ppu.h"
#include "runtime_arm.h"
#include "symbol_lookup.h"

namespace gbarecomp::debug {

namespace {

// ─────────────────────────────────────────────────────────────────────
// JSON micro-helpers (same shape as oracle/main.cpp)
// ─────────────────────────────────────────────────────────────────────

void json_emit_hex(std::string& out, const uint8_t* data, std::size_t n) {
    out.reserve(out.size() + n * 2 + 2);
    out.push_back('"');
    static const char* H = "0123456789abcdef";
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(H[data[i] >> 4]);
        out.push_back(H[data[i] & 0xF]);
    }
    out.push_back('"');
}

bool extract_uint(std::string_view req, std::string_view key, uint64_t& out) {
    auto pos = req.find(key);
    if (pos == std::string_view::npos) return false;
    pos = req.find(':', pos);
    if (pos == std::string_view::npos) return false;
    ++pos;
    while (pos < req.size() && (req[pos] == ' ' || req[pos] == '"')) ++pos;
    int base = 10;
    if (pos + 1 < req.size() && req[pos] == '0' &&
        (req[pos + 1] == 'x' || req[pos + 1] == 'X')) {
        base = 16;
        pos += 2;
    }
    uint64_t v = 0;
    bool any = false;
    while (pos < req.size()) {
        char c = req[pos];
        int digit;
        if (c >= '0' && c <= '9')                      digit = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f')   digit = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F')   digit = c - 'A' + 10;
        else break;
        v = v * base + static_cast<uint64_t>(digit);
        any = true;
        ++pos;
    }
    if (!any) return false;
    out = v;
    return true;
}

// Extract a JSON string value for `key` into `out`. Handles the common
// escapes a path can carry on the wire (\\ and \"); other escapes pass
// through verbatim. Returns false if the key/string is absent.
bool extract_string(std::string_view req, std::string_view key,
                    std::string& out) {
    auto pos = req.find(key);
    if (pos == std::string_view::npos) return false;
    pos = req.find(':', pos);
    if (pos == std::string_view::npos) return false;
    ++pos;
    while (pos < req.size() && req[pos] == ' ') ++pos;
    if (pos >= req.size() || req[pos] != '"') return false;
    ++pos;
    out.clear();
    while (pos < req.size()) {
        char c = req[pos++];
        if (c == '\\' && pos < req.size()) {
            char e = req[pos++];
            out.push_back(e);  // \\ -> \, \" -> ", others -> literal
            continue;
        }
        if (c == '"') return true;
        out.push_back(c);
    }
    return false;  // unterminated string
}

void emit_error(std::string& out, const char* msg) {
    out = "{\"ok\":false,\"error\":\"";
    out += msg;
    out += "\"}";
}

void emit_ok_int(std::string& out, const char* key, uint64_t v) {
    char buf[96];
    std::snprintf(buf, sizeof(buf),
                  "{\"ok\":true,\"%s\":%llu}", key,
                  static_cast<unsigned long long>(v));
    out = buf;
}

uint64_t frame_counter(const TcpDebugServer::Context& ctx) {
    if (ctx.sync_frames) return *ctx.sync_frames;
    return ctx.ppu ? ctx.ppu->frame_count() : 0u;
}

// ─────────────────────────────────────────────────────────────────────
// Region readers — pull bytes directly from the bus's backing arrays
// rather than going through bus.read8(), which would mask side effects
// like IF write-1-to-clear.
// ─────────────────────────────────────────────────────────────────────

void cmd_read_region(const uint8_t* base, std::size_t size,
                     std::string_view req, std::string& out,
                     uint32_t bus_base_addr) {
    uint64_t addr = 0, len = 0;
    if (!extract_uint(req, "\"addr\"", addr) ||
        !extract_uint(req, "\"len\"", len)) {
        emit_error(out, "missing addr/len");
        return;
    }
    uint64_t off = (addr >= bus_base_addr) ? addr - bus_base_addr : addr;
    if (off > size || len > size || off + len > size) {
        emit_error(out, "out of range");
        return;
    }
    out  = "{\"ok\":true,\"base\":";
    char ab[32];
    std::snprintf(ab, sizeof(ab), "%u",
                  static_cast<unsigned>(bus_base_addr + off));
    out += ab;
    out += ",\"len\":";
    char lb[32];
    std::snprintf(lb, sizeof(lb), "%llu",
                  static_cast<unsigned long long>(len));
    out += lb;
    out += ",\"data\":";
    json_emit_hex(out, base + off, static_cast<std::size_t>(len));
    out += "}";
}

void cmd_read_io_dynamic(gba::GbaBus& bus, std::string_view req,
                         std::string& out) {
    uint64_t addr = 0, len = 0;
    if (!extract_uint(req, "\"addr\"", addr) ||
        !extract_uint(req, "\"len\"", len)) {
        emit_error(out, "missing addr/len");
        return;
    }
    uint64_t off = (addr >= 0x04000000u) ? addr - 0x04000000u : addr;
    if (off > gba::GbaIo::kIoSize || len > gba::GbaIo::kIoSize ||
        off + len > gba::GbaIo::kIoSize) {
        emit_error(out, "out of range");
        return;
    }
    std::vector<uint8_t> bytes(static_cast<std::size_t>(len));
    for (uint64_t i = 0; i < len; ++i) {
        bytes[static_cast<std::size_t>(i)] =
            bus.io().read8(static_cast<uint32_t>(off + i));
    }
    out  = "{\"ok\":true,\"base\":";
    char ab[32];
    std::snprintf(ab, sizeof(ab), "%u",
                  static_cast<unsigned>(0x04000000u + off));
    out += ab;
    out += ",\"len\":";
    char lb[32];
    std::snprintf(lb, sizeof(lb), "%llu",
                  static_cast<unsigned long long>(len));
    out += lb;
    out += ",\"data\":";
    json_emit_hex(out, bytes.data(), bytes.size());
    out += "}";
}

void cmd_audio_samples(gba::GbaBus& bus, std::string_view req,
                       std::string& out) {
    uint64_t max_samples = 4096;
    uint64_t parsed = 0;
    if (extract_uint(req, "\"max\"", parsed) ||
        extract_uint(req, "\"count\"", parsed) ||
        extract_uint(req, "\"samples\"", parsed)) {
        max_samples = parsed;
    }
    if (max_samples > 16384) max_samples = 16384;

    std::vector<int16_t> samples(static_cast<std::size_t>(max_samples));
    std::size_t n = bus.audio().drain_samples(samples.data(), samples.size());
    std::vector<uint8_t> bytes(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        uint16_t u = static_cast<uint16_t>(samples[i]);
        bytes[i * 2 + 0] = static_cast<uint8_t>(u & 0xFFu);
        bytes[i * 2 + 1] = static_cast<uint8_t>((u >> 8) & 0xFFu);
    }

    char hdr[160];
    std::snprintf(hdr, sizeof(hdr),
                  "{\"ok\":true,\"rate\":%u,\"count\":%llu,"
                  "\"samples_generated\":%llu,\"data\":",
                  static_cast<unsigned>(bus.audio().sample_rate()),
                  static_cast<unsigned long long>(n),
                  static_cast<unsigned long long>(
                      bus.audio().samples_generated()));
    out = hdr;
    json_emit_hex(out, bytes.data(), bytes.size());
    out += "}";
}

// Always-on, non-destructive capture-ring query. Unlike audio_samples (which
// drains the playback FIFO), this reads a [start,count] window of the capture
// ring backward from the live head without consuming anything — the probe
// queries history, it does not arm/drain (ring-buffer discipline). Params:
//   "count"  : samples to return (default 8192, cap 65536)
//   "start"  : absolute sample index (samples_generated numbering). If absent,
//              returns the most recent `count` samples (start = head - count).
// Emits the mixed mono stream + raw per-channel arrays as little-endian int16
// hex so the drift comparator can isolate PSG channels for the bit-check.
void cmd_audio_cap(gba::GbaBus& bus, std::string_view req, std::string& out) {
    const auto& audio = bus.audio();
    uint64_t count = 8192;
    uint64_t parsed = 0;
    if (extract_uint(req, "\"count\"", parsed) ||
        extract_uint(req, "\"max\"", parsed)) {
        count = parsed;
    }
    if (count > gba::GbaAudio::kCapRingSize) count = gba::GbaAudio::kCapRingSize;

    uint64_t head = audio.samples_generated();
    uint64_t oldest = audio.capture_oldest_index();
    uint64_t start;
    if (extract_uint(req, "\"start\"", parsed)) {
        start = parsed;
    } else {
        start = (head > count) ? head - count : oldest;
    }

    std::vector<gba::GbaAudio::CapSample> buf(static_cast<std::size_t>(count));
    uint64_t first = 0;
    std::size_t n = audio.query_capture(start, buf.size(), buf.data(), first);

    auto emit_i16 = [&](const char* key, auto pick) {
        std::vector<uint8_t> bytes(n * 2);
        for (std::size_t i = 0; i < n; ++i) {
            uint16_t u = static_cast<uint16_t>(pick(buf[i]));
            bytes[i * 2 + 0] = static_cast<uint8_t>(u & 0xFFu);
            bytes[i * 2 + 1] = static_cast<uint8_t>((u >> 8) & 0xFFu);
        }
        out += ",\"";
        out += key;
        out += "\":";
        json_emit_hex(out, bytes.data(), bytes.size());
    };

    char hdr[224];
    std::snprintf(hdr, sizeof(hdr),
                  "{\"ok\":true,\"rate\":%u,\"count\":%llu,\"first\":%llu,"
                  "\"head\":%llu,\"oldest\":%llu",
                  static_cast<unsigned>(audio.sample_rate()),
                  static_cast<unsigned long long>(n),
                  static_cast<unsigned long long>(first),
                  static_cast<unsigned long long>(head),
                  static_cast<unsigned long long>(oldest));
    out = hdr;
    emit_i16("mixed",    [](const gba::GbaAudio::CapSample& s){ return s.mixed; });
    emit_i16("ch1",      [](const gba::GbaAudio::CapSample& s){ return s.ch[0]; });
    emit_i16("ch2",      [](const gba::GbaAudio::CapSample& s){ return s.ch[1]; });
    emit_i16("ch3",      [](const gba::GbaAudio::CapSample& s){ return s.ch[2]; });
    emit_i16("ch4",      [](const gba::GbaAudio::CapSample& s){ return s.ch[3]; });
    emit_i16("direct_a", [](const gba::GbaAudio::CapSample& s){ return s.direct_a; });
    emit_i16("direct_b", [](const gba::GbaAudio::CapSample& s){ return s.direct_b; });
    out += "}";
}

void append_fifo_state(std::string& out, const char* name,
                       const gba::GbaAudio::FifoDebugState& fifo) {
    char hdr[160];
    std::snprintf(hdr, sizeof(hdr),
                  ",\"%s\":{\"write\":%u,\"read\":%u,\"count\":%u,"
                  "\"shift_word\":%u,\"bytes_remaining\":%u,\"samples\":[",
                  name,
                  static_cast<unsigned>(fifo.write),
                  static_cast<unsigned>(fifo.read),
                  static_cast<unsigned>(fifo.count),
                  static_cast<unsigned>(fifo.shift_word),
                  static_cast<unsigned>(fifo.bytes_remaining));
    out += hdr;
    for (uint32_t i = 0; i < gba::GbaAudio::kMaxSamplesPerEvent; ++i) {
        if (i) out += ",";
        char b[16];
        std::snprintf(b, sizeof(b), "%d", static_cast<int>(fifo.samples[i]));
        out += b;
    }
    out += "]}";
}

void cmd_audio_state(gba::GbaBus& bus, std::string& out) {
    const auto& audio = bus.audio();
    char hdr[240];
    std::snprintf(hdr, sizeof(hdr),
                  "{\"ok\":true,\"rate\":%u,\"samples_generated\":%llu,"
                  "\"soundbias\":%u,\"cycles_per_sample\":%u,"
                  "\"samples_per_event\":%u,\"cycle_accumulator\":%u,"
                  "\"cycles_until_event\":%u",
                  static_cast<unsigned>(audio.sample_rate()),
                  static_cast<unsigned long long>(audio.samples_generated()),
                  static_cast<unsigned>(audio.debug_soundbias()),
                  static_cast<unsigned>(
                      gba::GbaAudio::kSystemHz / audio.sample_rate()),
                  static_cast<unsigned>(audio.debug_samples_per_event()),
                  static_cast<unsigned>(audio.debug_cycle_accumulator()),
                  static_cast<unsigned>(audio.debug_cycles_until_event()));
    out = hdr;
    append_fifo_state(out, "fifo_a", audio.debug_fifo_state(0));
    append_fifo_state(out, "fifo_b", audio.debug_fifo_state(1));
    out += "}";
}

void cmd_audio_trace(gba::GbaBus& bus, std::string_view req,
                     std::string& out) {
    uint64_t max_entries = 128;
    uint64_t parsed = 0;
    if (extract_uint(req, "\"max\"", parsed) ||
        extract_uint(req, "\"count\"", parsed)) {
        max_entries = parsed;
    }
    uint32_t available = bus.audio().debug_trace_count();
    if (max_entries > available) max_entries = available;
    if (max_entries > gba::GbaAudio::kFifoTraceSize) {
        max_entries = gba::GbaAudio::kFifoTraceSize;
    }
    uint32_t start = available - static_cast<uint32_t>(max_entries);
    out = "{\"ok\":true,\"entries\":[";
    for (uint32_t i = 0; i < max_entries; ++i) {
        auto tr = bus.audio().debug_trace_entry(start + i);
        if (i) out += ",";
        char item[240];
        std::snprintf(item, sizeof(item),
            "{\"base\":%llu,\"fifo\":%u,\"until\":%u,\"start\":%u,"
            "\"slots\":%u,\"count\":%u,\"remaining\":%u,\"sample\":%d}",
            static_cast<unsigned long long>(tr.sample_base),
            static_cast<unsigned>(tr.fifo_id),
            static_cast<unsigned>(tr.until_cycles),
            static_cast<unsigned>(tr.start_slot),
            static_cast<unsigned>(tr.slots),
            static_cast<unsigned>(tr.count),
            static_cast<unsigned>(tr.bytes_remaining),
            static_cast<int>(tr.sample));
        out += item;
    }
    out += "]}";
}

void cmd_runtime_trace(const TcpDebugServer::Context& ctx,
                       std::string_view req, std::string& out) {
    if (!ctx.runtime_trace_copy) {
        emit_error(out, "runtime trace unavailable");
        return;
    }

    uint64_t max_entries = 128;
    uint64_t parsed = 0;
    if (extract_uint(req, "\"max\"", parsed) ||
        extract_uint(req, "\"count\"", parsed)) {
        max_entries = parsed;
    }
    if (max_entries > 4096) max_entries = 4096;  // full g_trace ring depth

    std::vector<RuntimeTraceEntry> entries(
        static_cast<std::size_t>(max_entries));
    uint32_t n = ctx.runtime_trace_copy(
        entries.data(), static_cast<uint32_t>(entries.size()));

    char hdr[80];
    std::snprintf(hdr, sizeof(hdr), "{\"ok\":true,\"count\":%u,\"entries\":[",
                  static_cast<unsigned>(n));
    out = hdr;
    for (uint32_t i = 0; i < n; ++i) {
        const RuntimeTraceEntry& e = entries[i];
        if (i) out += ",";
        char item[560];
        std::snprintf(
            item, sizeof(item),
            "{\"seq\":%u,\"cycles\":%llu,\"kind\":%u,\"pc\":%u,\"cpsr\":%u,"
            "\"addr\":%u,\"value\":%u,\"aux\":%u,"
            "\"r0\":%u,\"r1\":%u,\"r2\":%u,\"r3\":%u,"
            "\"r4\":%u,\"r5\":%u,\"r12\":%u,\"r13\":%u,\"r14\":%u}",
            static_cast<unsigned>(e.seq),
            static_cast<unsigned long long>(e.cycles),
            static_cast<unsigned>(e.kind),
            static_cast<unsigned>(e.pc),
            static_cast<unsigned>(e.cpsr),
            static_cast<unsigned>(e.addr),
            static_cast<unsigned>(e.value),
            static_cast<unsigned>(e.aux),
            static_cast<unsigned>(e.r0),
            static_cast<unsigned>(e.r1),
            static_cast<unsigned>(e.r2),
            static_cast<unsigned>(e.r3),
            static_cast<unsigned>(e.r4),
            static_cast<unsigned>(e.r5),
            static_cast<unsigned>(e.r12),
            static_cast<unsigned>(e.r13),
            static_cast<unsigned>(e.r14));
        out += item;
    }
    out += "]}";
}

// ── Cycle-anchor sampler (Axis 2) ──────────────────────────────────────
// {"cmd":"cyc_anchor","pc":P,"hits":H} -> {ok,pc,armed,fp_count,count,cyc:[...]}.
// Filters the always-on insn-fingerprint ring by guest PC and returns the
// cumulative g_runtime_cycles stamp of each execution. Consecutive-hit Δ is the
// offset-cancelled cycle ruler peered against the NBA oracle's cyc_anchor.
// armed=0 means GBARECOMP_INSN_TRACE is off (ring empty) — the recomp build with
// the ring armed is where this returns live data; the bios_smoke interpreter
// mirrors into its own local ring and leaves this (recomp) ring empty.
void cmd_cyc_anchor(std::string_view req, std::string& out) {
    uint64_t pc = 0;
    if (!extract_uint(req, "\"pc\"", pc)) { emit_error(out, "missing pc"); return; }
    uint64_t hits = 256, parsed = 0;
    if (extract_uint(req, "\"hits\"", parsed) ||
        extract_uint(req, "\"count\"", parsed) ||
        extract_uint(req, "\"max\"", parsed)) {
        hits = parsed;
    }
    if (hits > 65536) hits = 65536;
    std::vector<unsigned long long> cyc(static_cast<std::size_t>(hits));
    uint32_t n = runtime_fp_query_pc(static_cast<uint32_t>(pc),
                                     static_cast<uint32_t>(hits), cyc.data());
    char hdr[160];
    std::snprintf(hdr, sizeof(hdr),
                  "{\"ok\":true,\"pc\":%llu,\"armed\":%u,\"fp_count\":%u,"
                  "\"count\":%u,\"cyc\":[",
                  static_cast<unsigned long long>(pc),
                  g_runtime_insn_trace ? 1u : 0u,
                  runtime_fp_count(), n);
    out = hdr;
    for (uint32_t i = 0; i < n; ++i) {
        if (i) out += ',';
        char b[24];
        std::snprintf(b, sizeof(b), "%llu", cyc[i]);
        out += b;
    }
    out += "]}";
}

// ── IRQ raise/take ring query (Axis 3) ─────────────────────────────────
// {"cmd":"irq_cap","count":C} -> {ok,total,count,entries:[{cycle,src,ret,cpsr,
// from_halt}...]}. Dumps the most recent N IRQ vectorings (TAKE-time) from the
// always-on IRQ-vector ring. `src` is the active IE&IF source mask at the
// vector; `from_halt` distinguishes the wake-from-HALT path. NOTE: raise-time
// (the IF-set instant) is not separately recorded — take-time only (burndown
// Axis-3 gap). Populated by runtime_irq in the recompiled runtime.
void cmd_irq_cap(std::string_view req, std::string& out) {
    uint64_t count = 256, parsed = 0;
    if (extract_uint(req, "\"count\"", parsed) ||
        extract_uint(req, "\"max\"", parsed)) {
        count = parsed;
    }
    if (count > 65536) count = 65536;
    std::vector<RuntimeIrqLogEntry> buf(static_cast<std::size_t>(count));
    uint32_t n = runtime_irq_log_copy_recent(buf.data(),
                                             static_cast<uint32_t>(count));
    char hdr[96];
    std::snprintf(hdr, sizeof(hdr),
                  "{\"ok\":true,\"total\":%u,\"count\":%u,\"entries\":[",
                  runtime_irq_log_count(), n);
    out = hdr;
    for (uint32_t i = 0; i < n; ++i) {
        if (i) out += ',';
        char item[200];
        std::snprintf(item, sizeof(item),
                      "{\"cycle\":%llu,\"src\":%u,\"ret\":%u,\"cpsr\":%u,"
                      "\"from_halt\":%u}",
                      buf[i].cycles, buf[i].src, buf[i].ret, buf[i].cpsr,
                      buf[i].from_halt);
        out += item;
    }
    out += "]}";
}

// ── MMIO write-trace ring query (Axis 4) ───────────────────────────────
// {"cmd":"mmio_cap","count":C,"start":S?} -> {ok,total,oldest,first,count,
// entries:[{cycle,addr,value,size,pc}...]}. Non-destructive window query of the
// always-on IO write-trace ring (gba_io.cpp). Default returns the most recent
// `count` writes; `start` requests an absolute index window.
void cmd_mmio_cap(std::string_view req, std::string& out) {
    uint64_t count = 4096, parsed = 0;
    if (extract_uint(req, "\"count\"", parsed) ||
        extract_uint(req, "\"max\"", parsed)) {
        count = parsed;
    }
    if (count > 65536) count = 65536;
    uint64_t total  = gba::gba_mmio_cap_total();
    uint64_t oldest = gba::gba_mmio_cap_oldest();
    uint64_t start;
    if (extract_uint(req, "\"start\"", parsed)) {
        start = parsed;
    } else {
        start = (total > count) ? total - count : oldest;
    }
    std::vector<gba::MmioCapEntry> buf(static_cast<std::size_t>(count));
    uint64_t first = 0;
    std::size_t n = gba::gba_mmio_cap_query(start, buf.size(), buf.data(), first);
    char hdr[176];
    std::snprintf(hdr, sizeof(hdr),
                  "{\"ok\":true,\"total\":%llu,\"oldest\":%llu,\"first\":%llu,"
                  "\"count\":%llu,\"entries\":[",
                  static_cast<unsigned long long>(total),
                  static_cast<unsigned long long>(oldest),
                  static_cast<unsigned long long>(first),
                  static_cast<unsigned long long>(n));
    out = hdr;
    for (std::size_t i = 0; i < n; ++i) {
        if (i) out += ',';
        char item[224];
        std::snprintf(item, sizeof(item),
                      "{\"cycle\":%llu,\"addr\":%llu,\"value\":%llu,\"size\":%u,"
                      "\"pc\":%llu}",
                      static_cast<unsigned long long>(buf[i].cycle),
                      static_cast<unsigned long long>(buf[i].addr),
                      static_cast<unsigned long long>(buf[i].value),
                      buf[i].size,
                      static_cast<unsigned long long>(buf[i].pc));
        out += item;
    }
    out += "]}";
}

// ── Determinism hook (Axis 7) ──────────────────────────────────────────
// {"cmd":"state_hash"} -> {ok,cycles,iwram,ewram,vram,pal,oam,hash}. A cheap
// read-only FNV-1a-64 over IWRAM+EWRAM+VRAM+PAL+OAM plus g_runtime_cycles, so a
// run-twice determinism probe can compare end-state in one call. Per-region
// hashes localize a divergence.
void cmd_state_hash(const TcpDebugServer::Context& ctx, std::string& out) {
    if (!ctx.bus) { emit_error(out, "bus unavailable"); return; }
    auto fnv = [](const uint8_t* p, std::size_t n) -> uint64_t {
        uint64_t h = 1469598103934665603ull;
        for (std::size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
        return h;
    };
    uint64_t iw = fnv(ctx.bus->iwram_ptr(), 32 * 1024);
    uint64_t ew = fnv(ctx.bus->ewram_ptr(), 256 * 1024);
    uint64_t vr = fnv(ctx.bus->vram_ptr(), 96 * 1024);
    uint64_t pa = fnv(ctx.bus->pal_ptr(), 1024);
    uint64_t oa = fnv(ctx.bus->oam_ptr(), 1024);
    unsigned long long cyc = g_runtime_cycles;
    uint64_t all = 1469598103934665603ull;
    uint64_t parts[6] = { iw, ew, vr, pa, oa, static_cast<uint64_t>(cyc) };
    for (int k = 0; k < 6; ++k) {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(&parts[k]);
        for (int j = 0; j < 8; ++j) { all ^= b[j]; all *= 1099511628211ull; }
    }
    char buf[400];
    std::snprintf(buf, sizeof(buf),
                  "{\"ok\":true,\"cycles\":%llu,\"iwram\":\"%016llx\","
                  "\"ewram\":\"%016llx\",\"vram\":\"%016llx\",\"pal\":\"%016llx\","
                  "\"oam\":\"%016llx\",\"hash\":\"%016llx\"}",
                  cyc, static_cast<unsigned long long>(iw),
                  static_cast<unsigned long long>(ew),
                  static_cast<unsigned long long>(vr),
                  static_cast<unsigned long long>(pa),
                  static_cast<unsigned long long>(oa),
                  static_cast<unsigned long long>(all));
    out = buf;
}

void dispatch(const TcpDebugServer::Context& ctx, std::string_view req,
              std::string& out, bool& want_quit, bool& step_failed) {
    out.clear();

    auto contains = [&](const char* tok) {
        return req.find(tok) != std::string_view::npos;
    };

    if (contains("\"ping\"") || req == "ping") {
        out = "{\"ok\":true,\"who\":\"gbarecomp_native\"}";
        return;
    }
    if (contains("\"frame\"") && !contains("\"emu_")) {
        emit_ok_int(out, "frame", frame_counter(ctx));
        return;
    }
    if (contains("\"step_inst\"")) {
        if (!ctx.step_inst) {
            emit_error(out, "step_inst callback not wired");
            return;
        }
        bool ok = ctx.step_inst();
        if (!ok) step_failed = true;
        // Return PC + CPSR + R0..R14 so the lockstep harness can
        // catch register-level divergence at its true origin (not
        // wait for the cascade into a branch decision).
        uint32_t pc = ctx.recomp_cpu ? ctx.recomp_cpu->R[15]
                                      : (ctx.cpu ? ctx.cpu->R[15] : 0u);
        uint64_t f  = frame_counter(ctx);
        std::string body;
        body.reserve(512);
        char hdr[160];
        std::snprintf(hdr, sizeof(hdr),
                      "{\"ok\":%s,\"pc\":%u,\"frame\":%llu,"
                      "\"cycles\":%u,\"cycles_elapsed\":%llu",
                      ok ? "true" : "false",
                      static_cast<unsigned>(pc),
                      static_cast<unsigned long long>(f),
                      static_cast<unsigned>(ctx.last_step_cycles ?
                          *ctx.last_step_cycles : 0u),
                      static_cast<unsigned long long>(ctx.cycles_elapsed ?
                          *ctx.cycles_elapsed : 0u));
        body = hdr;
        if (ctx.recomp_cpu) {
            for (int i = 0; i < 15; ++i) {
                char f[48];
                std::snprintf(f, sizeof(f), ",\"r%d\":%u",
                              i, static_cast<unsigned>(ctx.recomp_cpu->R[i]));
                body += f;
            }
            char cpsr_field[48];
            std::snprintf(cpsr_field, sizeof(cpsr_field), ",\"cpsr\":%u",
                          static_cast<unsigned>(ctx.recomp_cpu->cpsr));
            body += cpsr_field;
        } else if (ctx.cpu) {
            for (int i = 0; i < 15; ++i) {
                char f[48];
                std::snprintf(f, sizeof(f), ",\"r%d\":%u",
                              i, static_cast<unsigned>(ctx.cpu->R[i]));
                body += f;
            }
            uint32_t cpsr = 0;
            if (ctx.cpu->cpsr.n) cpsr |= 1u << 31;
            if (ctx.cpu->cpsr.z) cpsr |= 1u << 30;
            if (ctx.cpu->cpsr.c) cpsr |= 1u << 29;
            if (ctx.cpu->cpsr.v) cpsr |= 1u << 28;
            if (ctx.cpu->cpsr.i) cpsr |= 1u << 7;
            if (ctx.cpu->cpsr.f) cpsr |= 1u << 6;
            if (ctx.cpu->cpsr.t) cpsr |= 1u << 5;
            cpsr |= ctx.cpu->cpsr.mode & 0x1Fu;
            char cpsr_field[48];
            std::snprintf(cpsr_field, sizeof(cpsr_field), ",\"cpsr\":%u",
                          static_cast<unsigned>(cpsr));
            body += cpsr_field;
        }
        body += "}";
        out = body;
        return;
    }
    if (contains("\"step\"") || contains("\"step_to_vblank\"")) {
        if (!ctx.step) {
            emit_error(out, "step callback not wired");
            return;
        }
        bool ok = ctx.step();
        if (!ok) step_failed = true;
        uint64_t f = frame_counter(ctx);
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "{\"ok\":%s,\"frame\":%llu}",
                      ok ? "true" : "false",
                      static_cast<unsigned long long>(f));
        out = buf;
        return;
    }
    // Free-run control. `continue`/`pause` are NON-BLOCKING: they set the game
    // thread's run-state and return immediately, so the server (this thread)
    // stays free to answer observation commands while the game free-runs — even
    // while it is wedged in a busy-spin freeze (observe-the-hung-core). Place
    // these BEFORE the generic readers so the short tokens match first.
    if (contains("\"continue\"") || contains("\"resume\"")) {
        if (ctx.resume) ctx.resume();
        out = "{\"ok\":true,\"run\":\"running\"}";
        return;
    }
    if (contains("\"pause\"")) {
        if (ctx.pause) ctx.pause();
        out = ctx.run_status ? ctx.run_status()
                             : std::string("{\"ok\":true,\"run\":\"paused\"}");
        return;
    }
    if (contains("\"run_status\"")) {
        out = ctx.run_status ? ctx.run_status()
                             : std::string("{\"ok\":false,\"error\":\"no run_status\"}");
        return;
    }
    if (contains("\"read_oam\"")) {
        if (!ctx.bus) { emit_error(out, "bus unavailable"); return; }
        cmd_read_region(ctx.bus->oam_ptr(), 1024, req, out, 0x07000000u);
        return;
    }
    if (contains("\"read_pal\"")) {
        if (!ctx.bus) { emit_error(out, "bus unavailable"); return; }
        cmd_read_region(ctx.bus->pal_ptr(), 1024, req, out, 0x05000000u);
        return;
    }
    if (contains("\"read_vram\"")) {
        if (!ctx.bus) { emit_error(out, "bus unavailable"); return; }
        cmd_read_region(ctx.bus->vram_ptr(), 96 * 1024, req, out, 0x06000000u);
        return;
    }
    if (contains("\"read_iwram\"")) {
        if (!ctx.bus) { emit_error(out, "bus unavailable"); return; }
        cmd_read_region(ctx.bus->iwram_ptr(), 32 * 1024, req, out, 0x03000000u);
        return;
    }
    if (contains("\"read_ewram\"")) {
        if (!ctx.bus) { emit_error(out, "bus unavailable"); return; }
        cmd_read_region(ctx.bus->ewram_ptr(), 256 * 1024, req, out, 0x02000000u);
        return;
    }
    if (contains("\"read_io\"")) {
        if (!ctx.bus) { emit_error(out, "bus unavailable"); return; }
        cmd_read_io_dynamic(*ctx.bus, req, out);
        return;
    }
    if (contains("\"m4a_dump\"")) {
        // Observability (MC-HP-002): read the live MP2K SoundInfo +
        // 12-channel array out of guest RAM and flag any voice whose
        // wave/data pointer left a sane region. Pure read; no exec path.
        if (!ctx.bus) { emit_error(out, "bus unavailable"); return; }
        gba::mp2k_dump_live(*ctx.bus, out);
        return;
    }
    if (contains("\"m4a_detect\"")) {
        // Scan the loaded ROM image for the SDK MP2K driver (SoundMain
        // signature → SoundMainRAM hook address). One-shot; static.
        if (!ctx.bus) { emit_error(out, "bus unavailable"); return; }
        auto sigs = gba::mp2k_detect(ctx.bus->rom_ptr(), ctx.bus->rom_size());
        out = "{\"ok\":true,\"sigs\":[";
        for (std::size_t i = 0; i < sigs.size(); ++i) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "%s{\"sound_main_off\":%llu,\"sound_main_ram\":%u}",
                          i == 0 ? "" : ",",
                          static_cast<unsigned long long>(sigs[i].sound_main_off),
                          static_cast<unsigned>(sigs[i].sound_main_ram));
            out += buf;
        }
        out += "]}";
        return;
    }
    if (contains("\"set_keyinput\"")) {
        if (!ctx.bus) { emit_error(out, "bus unavailable"); return; }
        uint64_t value = 0x03FFu;
        if (!extract_uint(req, "\"value\"", value)) {
            emit_error(out, "missing value");
            return;
        }
        ctx.bus->io().set_keyinput(static_cast<uint16_t>(value & 0x03FFu));
        out = "{\"ok\":true}";
        return;
    }
    if (contains("\"set_break_pc\"")) {
        // Debug PC breakpoint (MC-HP-002): runtime_should_yield() unwinds
        // the current runtime_dispatch when the guest PC reaches this
        // value, so the spin inside a single dispatch can be inspected.
        uint64_t value = 0;
        if (!extract_uint(req, "\"value\"", value)) {
            emit_error(out, "missing value");
            return;
        }
        g_runtime_break_pc = static_cast<uint32_t>(value);
        out = "{\"ok\":true}";
        return;
    }
    if (contains("\"symbol\"")) {
        // Resolve a guest PC to the nearest recompiled function name +
        // offset, e.g. {"name":"UpdateAnimationVariableFrames","offset":16}.
        // Backed by the generated address->name map (symbol_lookup.h).
        uint64_t addr = 0;
        if (!extract_uint(req, "\"addr\"", addr)) {
            emit_error(out, "missing addr");
            return;
        }
        uint32_t off = 0;
        const char* name = gba_symbol_lookup(static_cast<uint32_t>(addr), &off);
        char buf[256];
        if (name) {
            std::snprintf(buf, sizeof(buf),
                          "{\"ok\":true,\"addr\":%llu,\"name\":\"%s\","
                          "\"offset\":%u}",
                          static_cast<unsigned long long>(addr), name, off);
        } else {
            std::snprintf(buf, sizeof(buf),
                          "{\"ok\":true,\"addr\":%llu,\"name\":null,"
                          "\"offset\":0}",
                          static_cast<unsigned long long>(addr));
        }
        out = buf;
        return;
    }
    if (contains("\"ppu_state\"")) {
        if (!ctx.bus || !ctx.ppu) { emit_error(out, "ppu unavailable"); return; }
        auto io16 = [&](uint32_t off) -> uint16_t {
            return ctx.bus->io().read16(off);
        };
        auto io32 = [&](uint32_t off) -> uint32_t {
            uint32_t lo = io16(off);
            uint32_t hi = io16(off + 2);
            return lo | (hi << 16);
        };
        auto append_kv = [](std::string& s, const char* key, uint64_t v) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), ",\"%s\":%llu",
                          key, static_cast<unsigned long long>(v));
            s += buf;
        };
        std::string body = "{\"ok\":true";
        append_kv(body, "frame", ctx.ppu->frame_count());
        append_kv(body, "dispcnt",  io16(0x00));
        append_kv(body, "dispstat", io16(0x04));
        append_kv(body, "vcount",   io16(0x06));
        for (int bg = 0; bg < 4; ++bg) {
            char k[24];
            std::snprintf(k, sizeof(k), "bg%dcnt", bg);
            append_kv(body, k, io16(0x08 + bg * 2));
            std::snprintf(k, sizeof(k), "bg%dhofs", bg);
            append_kv(body, k, io16(0x10 + bg * 4));
            std::snprintf(k, sizeof(k), "bg%dvofs", bg);
            append_kv(body, k, io16(0x12 + bg * 4));
        }
        append_kv(body, "bg2pa", io16(0x20)); append_kv(body, "bg2pb", io16(0x22));
        append_kv(body, "bg2pc", io16(0x24)); append_kv(body, "bg2pd", io16(0x26));
        append_kv(body, "bg2x",  io32(0x28)); append_kv(body, "bg2y",  io32(0x2C));
        append_kv(body, "bg3pa", io16(0x30)); append_kv(body, "bg3pb", io16(0x32));
        append_kv(body, "bg3pc", io16(0x34)); append_kv(body, "bg3pd", io16(0x36));
        append_kv(body, "bg3x",  io32(0x38)); append_kv(body, "bg3y",  io32(0x3C));
        append_kv(body, "win0h", io16(0x40)); append_kv(body, "win1h", io16(0x42));
        append_kv(body, "win0v", io16(0x44)); append_kv(body, "win1v", io16(0x46));
        append_kv(body, "winin", io16(0x48)); append_kv(body, "winout",io16(0x4A));
        append_kv(body, "mosaic",  io16(0x4C));
        append_kv(body, "bldcnt",  io16(0x50));
        append_kv(body, "bldalpha",io16(0x52));
        append_kv(body, "bldy",    io16(0x54));
        body += "}";
        out = body;
        return;
    }
    if (contains("\"screenshot\"")) {
        if (!ctx.bus || !ctx.ppu) { emit_error(out, "ppu unavailable"); return; }
        const uint8_t* rgb = nullptr;
        std::vector<uint8_t> live;
        if (ctx.ppu->has_latched_framebuffer()) {
            rgb = ctx.ppu->latched_framebuffer();
        } else {
            live.assign(ctx.ppu->render_bytes(), 0);
            ctx.ppu->render(live.data(),
                            ctx.bus->io().read16(0x000),
                            ctx.bus->io().raw(),
                            ctx.bus->vram_ptr(),
                            ctx.bus->oam_ptr(),
                            ctx.bus->pal_ptr());
            rgb = live.data();
        }
        // Dimensions track the active view (240x160 faithful, wider when
        // view-area expansion is on) so oracle tooling sees the real frame.
        out = "{\"ok\":true,\"w\":" + std::to_string(ctx.ppu->render_width()) +
              ",\"h\":" + std::to_string(ctx.ppu->render_height()) + ",\"data\":";
        json_emit_hex(out, rgb, ctx.ppu->render_bytes());
        out += "}";
        return;
    }
    if (contains("\"audio_samples\"")) {
        if (!ctx.bus) { emit_error(out, "bus unavailable"); return; }
        cmd_audio_samples(*ctx.bus, req, out);
        return;
    }
    if (contains("\"audio_cap\"")) {
        if (!ctx.bus) { emit_error(out, "bus unavailable"); return; }
        cmd_audio_cap(*ctx.bus, req, out);
        return;
    }
    if (contains("\"audio_state\"")) {
        if (!ctx.bus) { emit_error(out, "bus unavailable"); return; }
        cmd_audio_state(*ctx.bus, out);
        return;
    }
    if (contains("\"audio_trace\"")) {
        if (!ctx.bus) { emit_error(out, "bus unavailable"); return; }
        cmd_audio_trace(*ctx.bus, req, out);
        return;
    }
    if (contains("\"runtime_trace\"")) {
        cmd_runtime_trace(ctx, req, out);
        return;
    }
    if (contains("\"cyc_anchor\"")) {
        cmd_cyc_anchor(req, out);
        return;
    }
    if (contains("\"irq_cap\"")) {
        cmd_irq_cap(req, out);
        return;
    }
    if (contains("\"mmio_cap\"")) {
        cmd_mmio_cap(req, out);
        return;
    }
    if (contains("\"state_hash\"")) {
        cmd_state_hash(ctx, out);
        return;
    }
    if (contains("\"registers\"")) {
        if (!ctx.cpu && !ctx.recomp_cpu) {
            emit_error(out, "cpu unavailable");
            return;
        }
        std::string body = "{\"ok\":true";
        uint32_t cpsr = 0;
        if (ctx.recomp_cpu) {
            for (int i = 0; i < 16; ++i) {
                char field[64];
                std::snprintf(field, sizeof(field), ",\"r%d\":%u",
                              i, static_cast<unsigned>(ctx.recomp_cpu->R[i]));
                body += field;
            }
            cpsr = ctx.recomp_cpu->cpsr;
        } else {
            for (int i = 0; i < 16; ++i) {
                char field[64];
                std::snprintf(field, sizeof(field), ",\"r%d\":%u",
                              i, static_cast<unsigned>(ctx.cpu->R[i]));
                body += field;
            }
            // Pack CPSR the same way enter_irq does.
            if (ctx.cpu->cpsr.n) cpsr |= 1u << 31;
            if (ctx.cpu->cpsr.z) cpsr |= 1u << 30;
            if (ctx.cpu->cpsr.c) cpsr |= 1u << 29;
            if (ctx.cpu->cpsr.v) cpsr |= 1u << 28;
            if (ctx.cpu->cpsr.i) cpsr |= 1u << 7;
            if (ctx.cpu->cpsr.f) cpsr |= 1u << 6;
            if (ctx.cpu->cpsr.t) cpsr |= 1u << 5;
            cpsr |= ctx.cpu->cpsr.mode & 0x1Fu;
        }
        char tail[64];
        std::snprintf(tail, sizeof(tail), ",\"cpsr\":%u}",
                      static_cast<unsigned>(cpsr));
        body += tail;
        out = body;
        return;
    }
    if (contains("\"counters\"")) {
        auto val = [&](uint64_t* p) -> uint64_t { return p ? *p : 0u; };
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"steps\":%llu,\"irq_entries\":%llu,"
            "\"swi_entries\":%llu,\"halt_steps\":%llu,"
            "\"vblank_irqs_raised\":%llu,\"cycles_elapsed\":%llu,"
            "\"last_step_cycles\":%u,\"sync_frames\":%llu}",
            static_cast<unsigned long long>(val(ctx.steps)),
            static_cast<unsigned long long>(val(ctx.irq_entries)),
            static_cast<unsigned long long>(val(ctx.swi_entries)),
            static_cast<unsigned long long>(val(ctx.halt_steps)),
            static_cast<unsigned long long>(val(ctx.vblank_irqs_raised)),
            static_cast<unsigned long long>(val(ctx.cycles_elapsed)),
            static_cast<unsigned>(ctx.last_step_cycles ?
                *ctx.last_step_cycles : 0u),
            static_cast<unsigned long long>(val(ctx.sync_frames)));
        out = buf;
        return;
    }
    if (contains("\"misses\"")) {
        // Live self-heal coverage: the always-on miss/heal bookkeeping queried
        // for the window of interest (no arm-then-run). Reports every PC the
        // interpreter bridged this session, which have healed to native, and
        // the native-call counts, so a TCP-driven repro can confirm a PC flips
        // bridge -> native (interp count freezes, native_calls climbs).
        if (!ctx.misses_query) {
            emit_error(out, "misses_query callback not wired");
            return;
        }
        out = ctx.misses_query();
        return;
    }
    if (contains("\"savestate_save\"")) {
        if (!ctx.savestate_save) {
            emit_error(out, "savestate_save callback not wired");
            return;
        }
        std::string path;
        if (!extract_string(req, "\"path\"", path) || path.empty()) {
            emit_error(out, "missing path");
            return;
        }
        std::string serr;
        if (!ctx.savestate_save(path, serr)) {
            emit_error(out, serr.empty() ? "savestate_save failed" : serr.c_str());
            return;
        }
        out = "{\"ok\":true,\"saved\":\"";
        for (char c : path) { if (c == '"' || c == '\\') out.push_back('\\'); out.push_back(c); }
        out += "\"}";
        return;
    }
    if (contains("\"fp_save\"")) {
        if (!ctx.fp_save) {
            emit_error(out, "fp_save callback not wired (insn fingerprinting "
                            "unavailable)");
            return;
        }
        std::string path;
        if (!extract_string(req, "\"path\"", path) || path.empty()) {
            emit_error(out, "missing path");
            return;
        }
        uint32_t n = ctx.fp_save(path);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "{\"ok\":true,\"count\":%u}",
                      static_cast<unsigned>(n));
        out = buf;
        return;
    }
    if (contains("\"savestate_load\"")) {
        if (!ctx.savestate_load) {
            emit_error(out, "savestate_load callback not wired");
            return;
        }
        std::string path;
        if (!extract_string(req, "\"path\"", path) || path.empty()) {
            emit_error(out, "missing path");
            return;
        }
        std::string serr;
        if (!ctx.savestate_load(path, serr)) {
            emit_error(out, serr.empty() ? "savestate_load failed" : serr.c_str());
            return;
        }
        // Report the resume PC + frame so the client can confirm the
        // restore landed where it expects.
        uint32_t pc = ctx.recomp_cpu ? ctx.recomp_cpu->R[15]
                                     : (ctx.cpu ? ctx.cpu->R[15] : 0u);
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "{\"ok\":true,\"pc\":%u,\"frame\":%llu}",
                      static_cast<unsigned>(pc),
                      static_cast<unsigned long long>(frame_counter(ctx)));
        out = buf;
        return;
    }
    if (contains("\"quit\"")) {
        out = "{\"ok\":true,\"bye\":true}";
        want_quit = true;
        return;
    }

    emit_error(out, "unknown command");
}

}  // namespace

TcpDebugServer::TcpDebugServer()  = default;
TcpDebugServer::~TcpDebugServer() = default;

bool TcpDebugServer::run(int port, const Context& ctx) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::fprintf(stderr, "tcp_debug_server: WSAStartup failed\n");
        return false;
    }
#endif

    socket_t srv = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) {
        std::fprintf(stderr, "tcp_debug_server: socket() failed\n");
        return false;
    }
    int yes = 1;
    ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(srv, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        std::fprintf(stderr, "tcp_debug_server: bind 127.0.0.1:%d failed\n", port);
        CLOSESOCK(srv);
        return false;
    }
    if (::listen(srv, 1) != 0) {
        std::fprintf(stderr, "tcp_debug_server: listen failed\n");
        CLOSESOCK(srv);
        return false;
    }

    std::printf("native: tcp_debug_server listening on 127.0.0.1:%d\n", port);
    std::fflush(stdout);

    bool quit_server = false;
    while (!quit_server) {
        sockaddr_in cli{};
        socklen_t clen = sizeof(cli);
        socket_t c = ::accept(srv, reinterpret_cast<sockaddr*>(&cli), &clen);
        if (c == INVALID_SOCKET) continue;

        std::string inbuf;
        std::string resp;
        char rbuf[4096];
        bool client_done = false;
        bool step_failed = false;
        while (!client_done) {
            int n = ::recv(c, rbuf, sizeof(rbuf), 0);
            if (n <= 0) break;
            inbuf.append(rbuf, rbuf + n);
            std::size_t nl;
            while ((nl = inbuf.find('\n')) != std::string::npos) {
                std::string_view line(inbuf.data(), nl);
                if (!line.empty() && line.back() == '\r')
                    line.remove_suffix(1);
                bool want_quit = false;
                dispatch(ctx, line, resp, want_quit, step_failed);
                resp.push_back('\n');
                if (::send(c, resp.data(),
                           static_cast<int>(resp.size()), 0) < 0) {
                    client_done = true;
                    break;
                }
                if (want_quit) {
                    client_done = true;
                    quit_server = true;
                    break;
                }
                if (step_failed) {
                    // Don't auto-quit; let the client decide.
                    step_failed = false;
                }
                inbuf.erase(0, nl + 1);
            }
            if (inbuf.size() > 8192) {
                std::string err = "{\"ok\":false,\"error\":\"line too long\"}\n";
                ::send(c, err.data(), static_cast<int>(err.size()), 0);
                break;
            }
        }
        CLOSESOCK(c);
    }

    CLOSESOCK(srv);
#ifdef _WIN32
    WSACleanup();
#endif
    return true;
}

}  // namespace gbarecomp::debug
