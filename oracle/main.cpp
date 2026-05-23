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
#include <mgba/core/log.h>
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

// ─────────────────────────────────────────────────────────────────────
// mGBA driver
// ─────────────────────────────────────────────────────────────────────

struct Oracle {
    mCore*   core    = nullptr;
    uint32_t vblanks = 0;     // VBlank events observed since reset
    uint64_t frame   = 0;     // mCore frame counter mirror

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

        core->reset(core);
        std::printf("oracle: mGBA core ready (bios=%s rom=%s)\n",
                    bios_path ? bios_path : "(none)",
                    rom_path  ? rom_path  : "(none)");
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
    if (starts("\"emu_step\"") || starts("\"emu_step_to_vblank\"")) {
        o.step_frame();
        emit_ok_int(out, "frame", o.frame);
        return;
    }
    if (starts("\"emu_reset\"")) {
        if (o.core) o.core->reset(o.core);
        o.vblanks = 0;
        o.frame   = 0;
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
        // Pull R0..R15 + CPSR via the bus's CPU view. For mGBA the
        // current ARM7TDMI registers live in core->cpu — but the
        // public API doesn't expose them directly. Plumbing this
        // properly needs the GBA-specific cast; do that in the full
        // spec pass (task #19).
        emit_error(out, "emu_registers not implemented yet");
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
