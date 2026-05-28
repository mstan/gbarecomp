// oracle/main.cpp — mGBA-backed reference emulator.
//
// Embeds libmgba and exposes the `emu_*` / `read_emu_*` TCP command
// set from ../TCP.md. Native gbarecomp speaks the matching `read_*`
// commands; a diff harness sits on top, syncs via VBlank IRQ count
// (NEVER raw frame index), and reports the first divergence.
//
// This binary is the ONLY place libmgba may be linked. Per
// PRINCIPLES.md, the native build has zero copyleft emulator deps.

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
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

extern "C" {
#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/core/interface.h>
#include <mgba/core/log.h>
#include <mgba/core/timing.h>
#include <mgba/internal/gba/dma.h>
#include <mgba/internal/gba/gba.h>
#include <mgba-util/vfs.h>
}

namespace {

// ─────────────────────────────────────────────────────────────────────
// JSON micro-helpers
// ─────────────────────────────────────────────────────────────────────

// We hand-write JSON output because pulling in nlohmann/json or
// similar for a tool that emits ~10 unique response shapes is
// overkill. Requests are parsed with simple key-substring scans —
// good enough for trusted, localhost-only traffic.

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
        if (c >= '0' && c <= '9')      digit = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else break;
        v = v * base + static_cast<uint64_t>(digit);
        any = true;
        ++pos;
    }
    if (!any) return false;
    out = v;
    return true;
}

// Extract the hex string value of `key` (e.g. "data") and decode it
// into `out` bytes. Returns false if the key is absent or malformed.
bool extract_hex_bytes(std::string_view req, std::string_view key,
                       std::vector<uint8_t>& out) {
    auto pos = req.find(key);
    if (pos == std::string_view::npos) return false;
    pos = req.find(':', pos);
    if (pos == std::string_view::npos) return false;
    ++pos;
    while (pos < req.size() && (req[pos] == ' ' || req[pos] == '"')) ++pos;
    auto hex_digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out.clear();
    while (pos + 1 < req.size()) {
        int hi = hex_digit(req[pos]);
        if (hi < 0) break;  // closing quote or end
        int lo = hex_digit(req[pos + 1]);
        if (lo < 0) return false;  // odd nibble count
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
        pos += 2;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────
// mGBA driver
// ─────────────────────────────────────────────────────────────────────

struct OracleAudioTap {
    mAVStream stream{};
    std::vector<int16_t> mono;
    std::size_t read_index = 0;
    uint64_t samples_generated = 0;
    unsigned sample_rate = 32768;

    OracleAudioTap() {
        stream.audioRateChanged = &OracleAudioTap::rate_changed;
        stream.postAudioFrame = &OracleAudioTap::post_audio_frame;
    }

    void clear_samples() {
        mono.clear();
        read_index = 0;
        samples_generated = 0;
    }

    std::vector<int16_t> drain(std::size_t max) {
        std::size_t available = mono.size() - read_index;
        std::size_t n = available < max ? available : max;
        std::vector<int16_t> out;
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            out.push_back(mono[read_index + i]);
        }
        read_index += n;
        if (read_index > 65536 && read_index * 2 > mono.size()) {
            mono.erase(mono.begin(), mono.begin() +
                       static_cast<std::ptrdiff_t>(read_index));
            read_index = 0;
        }
        return out;
    }

    static void rate_changed(mAVStream* stream, unsigned rate) {
        auto* self = reinterpret_cast<OracleAudioTap*>(stream);
        self->sample_rate = rate;
    }

    static void post_audio_frame(mAVStream* stream, int16_t left, int16_t right) {
        auto* self = reinterpret_cast<OracleAudioTap*>(stream);
        int32_t mixed = (static_cast<int32_t>(left) +
                         static_cast<int32_t>(right)) / 2;
        self->mono.push_back(static_cast<int16_t>(mixed));
        ++self->samples_generated;
    }
};

struct Oracle {
    mCore*   core    = nullptr;
    uint32_t vblanks = 0;     // VBlank events observed since reset
    uint64_t frame   = 0;     // mCore frame counter mirror
    uint64_t inst_count = 0;  // emu_step_inst calls since reset
    // mGBA renders the framebuffer into this buffer on each runFrame.
    // 240*160 pixels at BYTES_PER_PIXEL (default 4 = 32-bit ARGB).
    std::vector<uint32_t> video_buf;
    OracleAudioTap audio;

    // Drive mGBA's post-reset CPU state to match real ARM7TDMI
    // hardware conventions (ARM ARM A2.6.5): PC=0, mode=Supervisor,
    // I=F=1, T=0. mGBA's GBAReset pre-bakes the SP banks then leaves
    // the CPU in System mode with I=F=0 — that mismatch skips the
    // first several BIOS instructions, breaking per-instruction
    // lockstep against our native runtime.
    //
    // The banked SP values mGBA programs (IRQ=0x03007FA0,
    // SVC=0x03007FE0, System=0x03007F00) already match our native
    // make_reset_cpu(); writing CPSR triggers _ARMReadCPSR which
    // re-applies the bank-swap, so the active R13 becomes the
    // mode's banked value automatically.
    void apply_real_hw_reset() {
        if (!core) return;
        int32_t cpsr = 0x000000D3;  // SVC=0x13, T=0, F=1, I=1, NZCV=0
        int32_t pc   = 0;
        core->writeRegister(core, "cpsr", &cpsr);
        core->writeRegister(core, "r15",  &pc);
    }

    // GBA-specific regions. Sizes per GBATEK § "GBA Memory Map".
    static constexpr uint32_t kBaseBIOS  = 0x00000000;
    static constexpr uint32_t kBaseEWRAM = 0x02000000;
    static constexpr uint32_t kBaseIWRAM = 0x03000000;
    static constexpr uint32_t kBaseIO    = 0x04000000;
    static constexpr uint32_t kBasePAL   = 0x05000000;
    static constexpr uint32_t kBaseVRAM  = 0x06000000;
    static constexpr uint32_t kBaseOAM   = 0x07000000;
    static constexpr uint32_t kBaseROM   = 0x08000000;

    static constexpr uint32_t kSizeBIOS  = 0x4000;
    static constexpr uint32_t kSizeEWRAM = 0x40000;
    static constexpr uint32_t kSizeIWRAM = 0x8000;
    static constexpr uint32_t kSizeIO    = 0x400;
    static constexpr uint32_t kSizePAL   = 0x400;
    static constexpr uint32_t kSizeVRAM  = 0x18000;
    static constexpr uint32_t kSizeOAM   = 0x400;

    bool init(const char* bios_path, const char* rom_path) {
        core = mCoreCreate(mPLATFORM_GBA);
        if (!core) {
            std::fprintf(stderr, "oracle: mCoreCreate failed\n");
            return false;
        }
        if (!core->init(core)) {
            std::fprintf(stderr, "oracle: core->init failed\n");
            return false;
        }
        mCoreInitConfig(core, nullptr);

        if (bios_path && *bios_path) {
            VFile* vf = VFileOpen(bios_path, O_RDONLY);
            if (!vf) {
                std::fprintf(stderr, "oracle: open BIOS %s failed\n", bios_path);
                return false;
            }
            if (!core->loadBIOS(core, vf, 0)) {
                std::fprintf(stderr, "oracle: loadBIOS failed\n");
                return false;
            }
        }
        if (rom_path && *rom_path) {
            VFile* vf = VFileOpen(rom_path, O_RDONLY);
            if (!vf) {
                std::fprintf(stderr, "oracle: open ROM %s failed\n", rom_path);
                return false;
            }
            if (!core->loadROM(core, vf)) {
                std::fprintf(stderr, "oracle: loadROM failed\n");
                return false;
            }
        }

        // Provide a video buffer so the PPU renders into something
        // we can read back for emu_screenshot. Without this mGBA
        // skips rendering work entirely.
        unsigned w = 0, h = 0;
        core->desiredVideoDimensions(core, &w, &h);
        video_buf.assign(static_cast<std::size_t>(w) * h, 0);
        core->setVideoBuffer(core, video_buf.data(), w);

        core->reset(core);
        apply_real_hw_reset();
        core->setAVStream(core, &audio.stream);
        audio.clear_samples();
        std::printf("oracle: mGBA core ready (bios=%s rom=%s video=%ux%u) "
                    "[reset state forced to real ARM7TDMI: PC=0 SVC I=F=1]\n",
                    bios_path ? bios_path : "(none)",
                    rom_path  ? rom_path  : "(none)", w, h);
        return true;
    }

    void shutdown() {
        if (core) {
            mCoreConfigDeinit(&core->config);
            core->deinit(core);
            core = nullptr;
        }
    }

    // One PPU frame: equivalent to "step to next VBlank end and wrap".
    void step_frame() {
        if (!core) return;
        core->runFrame(core);
        ++vblanks;
        ++frame;
    }

    // Read `len` bytes starting at `base + off` via the bus. Returns
    // false if any byte falls outside the documented region.
    bool read_region(uint32_t base, uint32_t region_size,
                     uint64_t off, uint64_t len,
                     std::vector<uint8_t>& out) const {
        if (off > region_size || len > region_size || off + len > region_size) {
            return false;
        }
        out.resize(static_cast<std::size_t>(len));
        for (uint64_t i = 0; i < len; ++i) {
            out[static_cast<std::size_t>(i)] =
                static_cast<uint8_t>(core->busRead8(core, base + static_cast<uint32_t>(off + i)));
        }
        return true;
    }
};

// ─────────────────────────────────────────────────────────────────────
// Command dispatch
// ─────────────────────────────────────────────────────────────────────

struct CmdResult {
    std::string body;
    bool ok = true;
};

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

void cmd_read_region(Oracle& o, std::string_view req,
                     uint32_t base, uint32_t size, std::string& out) {
    uint64_t addr = 0, len = 0;
    if (!extract_uint(req, "\"addr\"", addr) ||
        !extract_uint(req, "\"len\"", len)) {
        emit_error(out, "missing addr/len");
        return;
    }
    // Allow callers to pass either a region-relative offset or the
    // absolute bus address — both are convenient.
    uint64_t off = (addr >= base) ? addr - base : addr;
    std::vector<uint8_t> bytes;
    if (!o.read_region(base, size, off, len, bytes)) {
        emit_error(out, "out of range");
        return;
    }
    out  = "{\"ok\":true,\"base\":";
    char ab[32];
    std::snprintf(ab, sizeof(ab), "%u", static_cast<unsigned>(base + off));
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

void cmd_audio_samples(Oracle& o, std::string_view req, std::string& out) {
    uint64_t max_samples = 4096;
    uint64_t parsed = 0;
    if (extract_uint(req, "\"max\"", parsed) ||
        extract_uint(req, "\"count\"", parsed) ||
        extract_uint(req, "\"samples\"", parsed)) {
        max_samples = parsed;
    }
    if (max_samples > 16384) max_samples = 16384;

    std::vector<int16_t> samples =
        o.audio.drain(static_cast<std::size_t>(max_samples));
    std::vector<uint8_t> bytes(samples.size() * 2);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        uint16_t u = static_cast<uint16_t>(samples[i]);
        bytes[i * 2 + 0] = static_cast<uint8_t>(u & 0xFFu);
        bytes[i * 2 + 1] = static_cast<uint8_t>((u >> 8) & 0xFFu);
    }

    char hdr[160];
    std::snprintf(hdr, sizeof(hdr),
                  "{\"ok\":true,\"rate\":%u,\"count\":%llu,"
                  "\"samples_generated\":%llu,\"data\":",
                  static_cast<unsigned>(o.audio.sample_rate),
                  static_cast<unsigned long long>(samples.size()),
                  static_cast<unsigned long long>(
                      o.audio.samples_generated));
    out = hdr;
    json_emit_hex(out, bytes.data(), bytes.size());
    out += "}";
}

void dispatch(Oracle& o, std::string_view req, std::string& out) {
    out.clear();

    auto starts = [&](std::string_view tok) {
        return req.find(tok) != std::string_view::npos;
    };

    if (starts("\"ping\"") || req == "ping") {
        out = "{\"ok\":true,\"who\":\"gbarecomp_oracle\"}";
        return;
    }
    if (starts("\"frame\"") || req == "frame") {
        emit_ok_int(out, "frame", o.frame);
        return;
    }
    if (starts("\"emu_vblank_count\"")) {
        emit_ok_int(out, "vblanks", o.vblanks);
        return;
    }
    if (starts("\"emu_step_inst\"")) {
        if (!o.core) { emit_error(out, "no core"); return; }
        uint64_t before_cycles =
            o.core->timing ? mTimingCurrentTime(o.core->timing) : 0;
        o.core->step(o.core);
        uint64_t after_cycles =
            o.core->timing ? mTimingCurrentTime(o.core->timing) : 0;
        uint64_t step_cycles = after_cycles - before_cycles;
        ++o.inst_count;
        int32_t pc = 0, cpsr = 0;
        o.core->readRegister(o.core, "r15",  &pc);
        o.core->readRegister(o.core, "cpsr", &cpsr);
        bool thumb = (cpsr & (1u << 5)) != 0;
        std::string body;
        body.reserve(512);
        char hdr[160];
        std::snprintf(hdr, sizeof(hdr),
            "{\"ok\":true,\"pc\":%u,\"thumb\":%s,\"frame\":%llu,"
            "\"cycles\":%llu,\"cycles_elapsed\":%llu",
            static_cast<unsigned>(pc),
            thumb ? "true" : "false",
            static_cast<unsigned long long>(o.frame),
            static_cast<unsigned long long>(step_cycles),
            static_cast<unsigned long long>(after_cycles));
        body = hdr;
        for (int i = 0; i < 15; ++i) {
            char name[8]; std::snprintf(name, sizeof(name), "r%d", i);
            int32_t v = 0;
            o.core->readRegister(o.core, name, &v);
            char f[48];
            std::snprintf(f, sizeof(f), ",\"r%d\":%u",
                          i, static_cast<unsigned>(v));
            body += f;
        }
        char cf[48];
        std::snprintf(cf, sizeof(cf), ",\"cpsr\":%u}",
                      static_cast<unsigned>(cpsr));
        body += cf;
        out = body;
        return;
    }
    if (starts("\"emu_step\"") || starts("\"emu_step_to_vblank\"")) {
        o.step_frame();
        emit_ok_int(out, "frame", o.frame);
        return;
    }
    if (starts("\"emu_reset\"")) {
        if (o.core) {
            o.core->reset(o.core);
            o.apply_real_hw_reset();
        }
        o.vblanks    = 0;
        o.frame      = 0;
        o.inst_count = 0;
        o.audio.clear_samples();
        out = "{\"ok\":true}";
        return;
    }
    if (starts("\"read_emu_oam\"")) {
        cmd_read_region(o, req, Oracle::kBaseOAM, Oracle::kSizeOAM, out);
        return;
    }
    if (starts("\"read_emu_pal\"")) {
        cmd_read_region(o, req, Oracle::kBasePAL, Oracle::kSizePAL, out);
        return;
    }
    if (starts("\"read_emu_vram\"")) {
        cmd_read_region(o, req, Oracle::kBaseVRAM, Oracle::kSizeVRAM, out);
        return;
    }
    if (starts("\"read_emu_iwram\"")) {
        cmd_read_region(o, req, Oracle::kBaseIWRAM, Oracle::kSizeIWRAM, out);
        return;
    }
    if (starts("\"read_emu_ewram\"")) {
        cmd_read_region(o, req, Oracle::kBaseEWRAM, Oracle::kSizeEWRAM, out);
        return;
    }
    if (starts("\"read_emu_io\"")) {
        cmd_read_region(o, req, Oracle::kBaseIO, Oracle::kSizeIO, out);
        return;
    }
    if (starts("\"read_emu_rom\"")) {
        // ROM cartridge: cap reads at 32 MB so out-of-range queries
        // don't iterate forever.
        cmd_read_region(o, req, Oracle::kBaseROM, 0x02000000u, out);
        return;
    }
    if (starts("\"emu_registers\"")) {
        // mGBA exposes registers via readRegister(name, &out). ARM7TDMI
        // names: "r0".."r15" and "cpsr". Anything missing comes back
        // as 0 with ok=false on the underlying call.
        std::string body = "{\"ok\":true";
        for (int r = 0; r <= 15; ++r) {
            char name[8]; std::snprintf(name, sizeof(name), "r%d", r);
            int32_t v = 0;
            o.core->readRegister(o.core, name, &v);
            char field[64];
            std::snprintf(field, sizeof(field), ",\"r%d\":%u",
                          r, static_cast<unsigned>(v));
            body += field;
        }
        int32_t cpsr = 0;
        o.core->readRegister(o.core, "cpsr", &cpsr);
        char fb[64];
        std::snprintf(fb, sizeof(fb), ",\"cpsr\":%u}",
                      static_cast<unsigned>(cpsr));
        body += fb;
        out = body;
        return;
    }
    if (starts("\"emu_pc\"")) {
        int32_t pc = 0;
        o.core->readRegister(o.core, "r15", &pc);
        emit_ok_int(out, "pc", static_cast<uint32_t>(pc));
        return;
    }
    if (starts("\"emu_inst_count\"")) {
        emit_ok_int(out, "inst_count", o.inst_count);
        return;
    }

    auto io16 = [&](uint32_t off) -> uint16_t {
        return static_cast<uint16_t>(
            o.core->busRead16(o.core, 0x04000000u + off));
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

    if (starts("\"emu_ppu_state\"")) {
        std::string body = "{\"ok\":true";
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
        // BG2/BG3 affine: PA/PB/PC/PD + reference X/Y.
        append_kv(body, "bg2pa", io16(0x20)); append_kv(body, "bg2pb", io16(0x22));
        append_kv(body, "bg2pc", io16(0x24)); append_kv(body, "bg2pd", io16(0x26));
        append_kv(body, "bg2x",  io32(0x28)); append_kv(body, "bg2y",  io32(0x2C));
        append_kv(body, "bg3pa", io16(0x30)); append_kv(body, "bg3pb", io16(0x32));
        append_kv(body, "bg3pc", io16(0x34)); append_kv(body, "bg3pd", io16(0x36));
        append_kv(body, "bg3x",  io32(0x38)); append_kv(body, "bg3y",  io32(0x3C));
        // Windows.
        append_kv(body, "win0h", io16(0x40)); append_kv(body, "win1h", io16(0x42));
        append_kv(body, "win0v", io16(0x44)); append_kv(body, "win1v", io16(0x46));
        append_kv(body, "winin", io16(0x48)); append_kv(body, "winout",io16(0x4A));
        // Mosaic + blending.
        append_kv(body, "mosaic",  io16(0x4C));
        append_kv(body, "bldcnt",  io16(0x50));
        append_kv(body, "bldalpha",io16(0x52));
        append_kv(body, "bldy",    io16(0x54));
        body += "}";
        out = body;
        return;
    }

    if (starts("\"emu_dma_state\"")) {
        std::string body = "{\"ok\":true,\"channels\":[";
        for (int ch = 0; ch < 4; ++ch) {
            if (ch) body += ",";
            uint32_t base = 0xB0u + ch * 12u;
            uint32_t sad   = io32(base);
            uint32_t dad   = io32(base + 4);
            uint16_t cnt_l = io16(base + 8);
            uint16_t cnt_h = io16(base + 10);
            char chunk[256];
            std::snprintf(chunk, sizeof(chunk),
                "{\"ch\":%d,\"sad\":%u,\"dad\":%u,\"cnt_l\":%u,\"cnt_h\":%u,"
                "\"enable\":%d,\"start_mode\":%d}",
                ch,
                static_cast<unsigned>(sad), static_cast<unsigned>(dad),
                static_cast<unsigned>(cnt_l), static_cast<unsigned>(cnt_h),
                (cnt_h >> 15) & 1, (cnt_h >> 12) & 3);
            body += chunk;
        }
        body += "]}";
        out = body;
        return;
    }

    if (starts("\"emu_dma_internal\"")) {
        auto* gba = static_cast<GBA*>(o.core->board);
        std::string body = "{\"ok\":true,\"channels\":[";
        for (int ch = 0; ch < 4; ++ch) {
            if (ch) body += ",";
            const GBADMA& dma = gba->memory.dma[ch];
            char chunk[320];
            std::snprintf(chunk, sizeof(chunk),
                "{\"ch\":%d,\"reg\":%u,\"source\":%u,\"dest\":%u,"
                "\"count\":%d,\"nextSource\":%u,\"nextDest\":%u,"
                "\"nextCount\":%d,\"when\":%u}",
                ch,
                static_cast<unsigned>(dma.reg),
                static_cast<unsigned>(dma.source),
                static_cast<unsigned>(dma.dest),
                static_cast<int>(dma.count),
                static_cast<unsigned>(dma.nextSource),
                static_cast<unsigned>(dma.nextDest),
                static_cast<int>(dma.nextCount),
                static_cast<unsigned>(dma.when));
            body += chunk;
        }
        body += "]}";
        out = body;
        return;
    }

    if (starts("\"emu_timer_state\"")) {
        std::string body = "{\"ok\":true,\"timers\":[";
        for (int t = 0; t < 4; ++t) {
            if (t) body += ",";
            uint32_t base = 0x100u + t * 4u;
            uint16_t count_or_reload = io16(base);
            uint16_t cnt_h           = io16(base + 2);
            char chunk[160];
            std::snprintf(chunk, sizeof(chunk),
                "{\"t\":%d,\"counter\":%u,\"cnt_h\":%u,"
                "\"enable\":%d,\"cascade\":%d,\"prescaler\":%d}",
                t, count_or_reload, cnt_h,
                (cnt_h >> 7) & 1, (cnt_h >> 2) & 1, cnt_h & 3);
            body += chunk;
        }
        body += "]}";
        out = body;
        return;
    }

    if (starts("\"emu_audio_state\"")) {
        auto* gba = static_cast<GBA*>(o.core->board);
        const GBAAudio& audio = gba->audio;
        std::string body;
        char hdr[320];
        std::snprintf(hdr, sizeof(hdr),
            "{\"ok\":true,\"time\":%u,\"rate\":%u,"
            "\"samples_generated\":%llu,\"soundbias\":%u,"
            "\"sampleInterval\":%d,\"sampleIndex\":%d,"
            "\"lastSample\":%d,\"nextSample\":%d",
            static_cast<unsigned>(mTimingCurrentTime(&gba->timing)),
            static_cast<unsigned>(o.audio.sample_rate),
            static_cast<unsigned long long>(o.audio.samples_generated),
            static_cast<unsigned>(audio.soundbias),
            static_cast<int>(audio.sampleInterval),
            static_cast<int>(audio.sampleIndex),
            static_cast<int>(audio.lastSample),
            static_cast<int>(
                mTimingUntil(&gba->timing, &audio.sampleEvent)));
        body = hdr;
        auto append_fifo = [&](const char* name, const GBAAudioFIFO& fifo) {
            char fh[192];
            int fifo_size = fifo.fifoWrite >= fifo.fifoRead
                ? fifo.fifoWrite - fifo.fifoRead
                : GBA_AUDIO_FIFO_SIZE - fifo.fifoRead + fifo.fifoWrite;
            std::snprintf(fh, sizeof(fh),
                ",\"%s\":{\"write\":%d,\"read\":%d,\"count\":%d,"
                "\"internalSample\":%u,\"internalRemaining\":%d,"
                "\"samples\":[",
                name, fifo.fifoWrite, fifo.fifoRead, fifo_size,
                static_cast<unsigned>(fifo.internalSample),
                fifo.internalRemaining);
            body += fh;
            for (int i = 0; i < GBA_MAX_SAMPLES; ++i) {
                if (i) body += ",";
                char b[16];
                std::snprintf(b, sizeof(b), "%d",
                              static_cast<int>(fifo.samples[i]));
                body += b;
            }
            body += "]}";
        };
        append_fifo("fifo_a", audio.chA);
        append_fifo("fifo_b", audio.chB);
        body += "}";
        out = body;
        return;
    }

    if (starts("\"emu_screenshot\"")) {
        // mGBA renders 32-bit color_t into video_buf. In the default
        // 32-bit build, mGBA's native color layout is 0x00BBGGRR
        // (M_COLOR_RED = 0x000000FF). Convert to RGB888 so native and
        // oracle screenshots can be compared byte-for-byte.
        const void* px = nullptr;
        std::size_t stride = 0;  // pixels per row
        o.core->getPixels(o.core, &px, &stride);
        if (!px) {
            emit_error(out, "no framebuffer (mGBA not rendering)");
            return;
        }
        const uint32_t* src = static_cast<const uint32_t*>(px);
        std::vector<uint8_t> rgb(240 * 160 * 3);
        for (uint32_t y = 0; y < 160; ++y) {
            for (uint32_t x = 0; x < 240; ++x) {
                uint32_t p = src[y * stride + x];
                uint8_t r = (p >>  0) & 0xFFu;
                uint8_t g = (p >>  8) & 0xFFu;
                uint8_t b = (p >> 16) & 0xFFu;
                uint8_t* d = rgb.data() + (y * 240 + x) * 3;
                d[0] = r; d[1] = g; d[2] = b;
            }
        }
        out = "{\"ok\":true,\"w\":240,\"h\":160,\"data\":";
        json_emit_hex(out, rgb.data(), rgb.size());
        out += "}";
        return;
    }

    if (starts("\"emu_audio_samples\"")) {
        cmd_audio_samples(o, req, out);
        return;
    }

    if (starts("\"emu_irq_state\"")) {
        uint16_t ie  = io16(0x200);
        uint16_t if_ = io16(0x202);
        uint16_t ime = io16(0x208);
        char body[160];
        std::snprintf(body, sizeof(body),
            "{\"ok\":true,\"ie\":%u,\"if\":%u,\"ime\":%u,\"pending\":%u}",
            ie, if_, ime, static_cast<unsigned>(ie & if_));
        out = body;
        return;
    }
    // ── State-injection bridge ──────────────────────────────────────
    // These let a harness sync mGBA to a native gbarecomp savestate:
    // read the native runtime's RAM + regs over its TCP server, then
    // push them here. Sync point is a frame boundary; mGBA then runs
    // forward and we diff its state against native (find-first-divergence
    // for sound/transition bugs like MC-HP-002/003).

    if (starts("\"emu_set_keys\"")) {
        if (!o.core) { emit_error(out, "no core"); return; }
        uint64_t keys = 0;
        extract_uint(req, "\"keys\"", keys) ||
            extract_uint(req, "\"value\"", keys);
        o.core->setKeys(o.core, static_cast<uint32_t>(keys));
        emit_ok_int(out, "keys", keys);
        return;
    }

    if (starts("\"emu_write\"")) {
        if (!o.core) { emit_error(out, "no core"); return; }
        uint64_t addr = 0;
        if (!extract_uint(req, "\"addr\"", addr)) {
            emit_error(out, "missing addr");
            return;
        }
        std::vector<uint8_t> bytes;
        if (!extract_hex_bytes(req, "\"data\"", bytes)) {
            emit_error(out, "missing/odd data");
            return;
        }
        uint64_t width = 8;
        extract_uint(req, "\"width\"", width);
        uint32_t a = static_cast<uint32_t>(addr);
        if (width == 16 && (bytes.size() % 2) == 0) {
            for (std::size_t i = 0; i < bytes.size(); i += 2) {
                uint16_t v = static_cast<uint16_t>(
                    bytes[i] | (bytes[i + 1] << 8));
                o.core->busWrite16(o.core, a + static_cast<uint32_t>(i), v);
            }
        } else if (width == 32 && (bytes.size() % 4) == 0) {
            for (std::size_t i = 0; i < bytes.size(); i += 4) {
                uint32_t v = static_cast<uint32_t>(bytes[i]) |
                    (static_cast<uint32_t>(bytes[i + 1]) << 8) |
                    (static_cast<uint32_t>(bytes[i + 2]) << 16) |
                    (static_cast<uint32_t>(bytes[i + 3]) << 24);
                o.core->busWrite32(o.core, a + static_cast<uint32_t>(i), v);
            }
        } else {
            for (std::size_t i = 0; i < bytes.size(); ++i) {
                o.core->busWrite8(o.core, a + static_cast<uint32_t>(i),
                                  bytes[i]);
            }
        }
        emit_ok_int(out, "wrote", bytes.size());
        return;
    }

    if (starts("\"emu_set_regs\"")) {
        if (!o.core) { emit_error(out, "no core"); return; }
        // Write CPSR first so the mode (and thus the active r13/r14
        // bank) is correct before the GPRs land; r15 last so the PC
        // sticks after any bank swap. Only keys that are present are
        // written, so a partial reg set is allowed.
        uint64_t v = 0;
        if (extract_uint(req, "\"cpsr\"", v)) {
            int32_t cpsr = static_cast<int32_t>(v);
            o.core->writeRegister(o.core, "cpsr", &cpsr);
        }
        for (int i = 0; i <= 15; ++i) {
            char key[12];
            std::snprintf(key, sizeof(key), "\"r%d\"", i);
            if (extract_uint(req, key, v)) {
                int32_t rv = static_cast<int32_t>(v);
                char name[8];
                std::snprintf(name, sizeof(name), "r%d", i);
                o.core->writeRegister(o.core, name, &rv);
            }
        }
        out = "{\"ok\":true}";
        return;
    }

    if (starts("\"quit\"")) {
        out = "{\"ok\":true,\"bye\":true}";
        return;
    }

    emit_error(out, "unknown command");
}

// ─────────────────────────────────────────────────────────────────────
// Line-delimited TCP server
// ─────────────────────────────────────────────────────────────────────

int run_server(Oracle& o, int port) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::fprintf(stderr, "oracle: WSAStartup failed\n");
        return 1;
    }
#endif

    socket_t srv = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) {
        std::fprintf(stderr, "oracle: socket() failed\n");
        return 1;
    }
    int yes = 1;
    ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::fprintf(stderr, "oracle: bind to 127.0.0.1:%d failed\n", port);
        return 1;
    }
    if (::listen(srv, 1) != 0) {
        std::fprintf(stderr, "oracle: listen failed\n");
        return 1;
    }

    std::printf("oracle: listening on 127.0.0.1:%d\n", port);
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
        while (!client_done) {
            int n = ::recv(c, rbuf, sizeof(rbuf), 0);
            if (n <= 0) break;
            inbuf.append(rbuf, rbuf + n);
            std::size_t newline;
            while ((newline = inbuf.find('\n')) != std::string::npos) {
                std::string_view line(inbuf.data(), newline);
                // Strip trailing \r if present.
                if (!line.empty() && line.back() == '\r')
                    line.remove_suffix(1);
                dispatch(o, line, resp);
                resp.push_back('\n');
                if (::send(c, resp.data(),
                           static_cast<int>(resp.size()), 0) < 0) {
                    client_done = true;
                    break;
                }
                if (resp.find("\"bye\":true") != std::string::npos) {
                    client_done = true;
                    quit_server = true;
                    break;
                }
                inbuf.erase(0, newline + 1);
            }
            if (inbuf.size() > 8192) {
                // Protocol cap from TCP.md.
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
    return 0;
}

struct Args {
    std::string bios = "bios/gba_bios.bin";
    std::string rom;
    int         port = 19843;
};

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string_view s = argv[i];
        if (s == "--bios" && i + 1 < argc) { a.bios = argv[++i]; continue; }
        if (s == "--rom"  && i + 1 < argc) { a.rom  = argv[++i]; continue; }
        if (s == "--port" && i + 1 < argc) { a.port = std::atoi(argv[++i]); continue; }
        std::fprintf(stderr, "oracle: unknown arg %.*s\n",
                     static_cast<int>(s.size()), s.data());
    }
    return a;
}

}  // namespace

int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);
    Oracle o;
    if (!o.init(args.bios.c_str(),
                args.rom.empty() ? nullptr : args.rom.c_str())) {
        return 1;
    }
    int rc = run_server(o, args.port);
    o.shutdown();
    return rc;
}
