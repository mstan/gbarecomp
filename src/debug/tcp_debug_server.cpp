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

#include "cpu_state.h"
#include "gba_bus.h"
#include "gba_io.h"
#include "gba_ppu.h"

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
        uint64_t f = ctx.ppu ? ctx.ppu->frame_count() : 0;
        emit_ok_int(out, "frame", f);
        return;
    }
    if (contains("\"step_inst\"")) {
        if (!ctx.step_inst) {
            emit_error(out, "step_inst callback not wired");
            return;
        }
        bool ok = ctx.step_inst();
        if (!ok) step_failed = true;
        uint32_t pc = ctx.cpu ? ctx.cpu->R[15] : 0u;
        uint64_t f  = ctx.ppu ? ctx.ppu->frame_count() : 0u;
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "{\"ok\":%s,\"pc\":%u,\"frame\":%llu}",
                      ok ? "true" : "false",
                      static_cast<unsigned>(pc),
                      static_cast<unsigned long long>(f));
        out = buf;
        return;
    }
    if (contains("\"step\"") || contains("\"step_to_vblank\"")) {
        if (!ctx.step) {
            emit_error(out, "step callback not wired");
            return;
        }
        bool ok = ctx.step();
        if (!ok) step_failed = true;
        uint64_t f = ctx.ppu ? ctx.ppu->frame_count() : 0;
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "{\"ok\":%s,\"frame\":%llu}",
                      ok ? "true" : "false",
                      static_cast<unsigned long long>(f));
        out = buf;
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
    if (contains("\"read_io\"")) {
        if (!ctx.bus) { emit_error(out, "bus unavailable"); return; }
        cmd_read_region(ctx.bus->io().raw(), 0x400, req, out, 0x04000000u);
        return;
    }
    if (contains("\"registers\"")) {
        if (!ctx.cpu) { emit_error(out, "cpu unavailable"); return; }
        std::string body = "{\"ok\":true";
        for (int i = 0; i < 16; ++i) {
            char field[64];
            std::snprintf(field, sizeof(field), ",\"r%d\":%u",
                          i, static_cast<unsigned>(ctx.cpu->R[i]));
            body += field;
        }
        // Pack CPSR the same way enter_irq does.
        uint32_t cpsr = 0;
        if (ctx.cpu->cpsr.n) cpsr |= 1u << 31;
        if (ctx.cpu->cpsr.z) cpsr |= 1u << 30;
        if (ctx.cpu->cpsr.c) cpsr |= 1u << 29;
        if (ctx.cpu->cpsr.v) cpsr |= 1u << 28;
        if (ctx.cpu->cpsr.i) cpsr |= 1u << 7;
        if (ctx.cpu->cpsr.f) cpsr |= 1u << 6;
        if (ctx.cpu->cpsr.t) cpsr |= 1u << 5;
        cpsr |= ctx.cpu->cpsr.mode & 0x1Fu;
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
            "\"vblank_irqs_raised\":%llu}",
            static_cast<unsigned long long>(val(ctx.steps)),
            static_cast<unsigned long long>(val(ctx.irq_entries)),
            static_cast<unsigned long long>(val(ctx.swi_entries)),
            static_cast<unsigned long long>(val(ctx.halt_steps)),
            static_cast<unsigned long long>(val(ctx.vblank_irqs_raised)));
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
