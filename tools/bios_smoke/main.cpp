// bios_smoke — Phase 2.1 bring-up canary.
//
// Loads the GBA BIOS, sets the CPU to the reset state (PC=0,
// SVC mode, ARM state, IRQ/FIQ disabled), and steps the interpreter
// N instructions. Prints a one-line trace per instruction so we can
// hand-validate the first dozen against any public BIOS disassembly.
//
// This is NOT a rendering target. It's the smallest test that proves:
//   - The BIOS file loads + hash-verifies.
//   - The bus serves BIOS bytes to the fetch path.
//   - The interpreter can step real BIOS code without panicking.
//
// CLI:
//   bios_smoke [--bios <path>] [--steps N] [--quiet]

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fstream>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "arm_decode.h"
#include "arm_ir.h"
#include "asset_picker.h"
#include "cpu_state.h"
#include "gba_bios.h"
#include "gba_bus.h"
#include "gba_irq.h"      // gba::kIrqWakeDelayCycles (shared with the runtime)
#include "gba_ppu.h"
#include "gba_rom_header.h"
#include "host_window.h"
#include "interpreter.h"
#include "runtime_arm.h"   // ArmCpuState + g_cpu, for savestate_load mapping
#include "snapshot.h"      // debug::load_state (interpreter-as-oracle restore)
#include "tcp_debug_server.h"
#include "thumb_decode.h"

namespace {

constexpr const char* kDefaultBios = "bios/gba_bios.bin";
// Single source of truth in gba_irq.h; the recompiled runtime uses the same.
constexpr uint32_t kGbaIrqDelayCycles = gba::kIrqWakeDelayCycles;

struct Args {
    std::string bios = kDefaultBios;
    std::string rom;     // optional cartridge; when set, bios_smoke runs
                         // the full game under the interpreter (oracle).
    int  steps   = 16;
    bool quiet   = false;
    std::string dump_bmp;  // optional path for final framebuffer dump
    bool window  = false;
    int  scale   = 3;
    int  frames  = -1;     // when >=0, cap by completed frames not steps
    std::string snapshot;  // optional path prefix for OAM/VRAM/PAL/IWRAM dump
    int tcp_port = 0;      // when >0, run as TCP debug server
};

// Minimal 24-bit BMP writer for a 240x160 RGB888 framebuffer (row 0
// at top of image; BMP stores bottom-up, so we flip on write). No
// dependency on stb_image / etc.
bool write_bmp(const std::string& path, const uint8_t* rgb,
               uint32_t w, uint32_t h) {
    uint32_t row_bytes = w * 3;
    uint32_t padded_row = (row_bytes + 3) & ~3u;
    uint32_t pixel_data_size = padded_row * h;
    uint32_t file_size = 14 + 40 + pixel_data_size;

    std::ofstream f(path, std::ios::binary);
    if (!f) return false;

    auto put_u16 = [&](uint16_t v) {
        uint8_t b[2] = { static_cast<uint8_t>(v & 0xFF),
                         static_cast<uint8_t>((v >> 8) & 0xFF) };
        f.write(reinterpret_cast<char*>(b), 2);
    };
    auto put_u32 = [&](uint32_t v) {
        uint8_t b[4] = {
            static_cast<uint8_t>(v & 0xFF),
            static_cast<uint8_t>((v >>  8) & 0xFF),
            static_cast<uint8_t>((v >> 16) & 0xFF),
            static_cast<uint8_t>((v >> 24) & 0xFF),
        };
        f.write(reinterpret_cast<char*>(b), 4);
    };

    // BITMAPFILEHEADER
    f.write("BM", 2);
    put_u32(file_size);
    put_u16(0); put_u16(0);
    put_u32(14 + 40);
    // BITMAPINFOHEADER
    put_u32(40);
    put_u32(w);
    put_u32(h);
    put_u16(1);
    put_u16(24);
    put_u32(0);                  // BI_RGB
    put_u32(pixel_data_size);
    put_u32(2835); put_u32(2835); // 72 dpi
    put_u32(0); put_u32(0);

    // Pixel data, bottom-up, BGR order.
    std::vector<uint8_t> row(padded_row, 0);
    for (int y = static_cast<int>(h) - 1; y >= 0; --y) {
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* src = rgb + (y * w + x) * 3;
            row[x * 3 + 0] = src[2];  // B
            row[x * 3 + 1] = src[1];  // G
            row[x * 3 + 2] = src[0];  // R
        }
        f.write(reinterpret_cast<char*>(row.data()), padded_row);
    }
    return static_cast<bool>(f);
}

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s == "--bios" && i + 1 < argc) { a.bios = argv[++i]; continue; }
        if (s == "--rom"  && i + 1 < argc) { a.rom  = argv[++i]; continue; }
        if (s == "--steps" && i + 1 < argc) {
            a.steps = std::atoi(argv[++i]); continue;
        }
        if (s == "--quiet") { a.quiet = true; continue; }
        if (s == "--dump-bmp" && i + 1 < argc) {
            a.dump_bmp = argv[++i]; continue;
        }
        if (s == "--window") { a.window = true; a.quiet = true; continue; }
        if (s == "--scale" && i + 1 < argc) {
            a.scale = std::atoi(argv[++i]); continue;
        }
        if (s == "--frames" && i + 1 < argc) {
            a.frames = std::atoi(argv[++i]); continue;
        }
        if (s == "--snapshot" && i + 1 < argc) {
            a.snapshot = argv[++i]; continue;
        }
        if (s == "--tcp" && i + 1 < argc) {
            a.tcp_port = std::atoi(argv[++i]);
            a.quiet = true;
            continue;
        }
        std::fprintf(stderr, "bios_smoke: unknown arg %s\n", s.c_str());
    }
    return a;
}

// Initial CPU state per ARM ARM A2.6.5 and GBATEK § "GBA Reset":
//   CPSR: I=1, F=1, T=0, mode = Supervisor (0x13).
//   PC = 0.
//   R13_svc = 0x03007FE0 (BIOS sets it up early; we mirror what
//     real hardware leaves it at after BIOS reset).
//   All other regs unspecified but typically 0.
//
// We start with everything zeroed except CPSR + PC + banked SP, and
// let the BIOS itself initialize the rest. This matches power-on
// behavior closely enough for our smoke purposes.
armv4t::CPUState make_reset_cpu() {
    armv4t::CPUState cpu{};
    cpu.cpsr.i = true;
    cpu.cpsr.f = true;
    cpu.cpsr.t = false;
    cpu.cpsr.mode = static_cast<uint8_t>(armv4t::Mode::Supervisor);
    cpu.thumb = false;
    cpu.R[15] = 0x00000000;
    // Hardware leaves SVC SP at 0x03007FE0 after BIOS reset. We
    // pre-populate that into the banked slot AND the active R13 so
    // pre-BIOS code that touches the stack has a sane pointer.
    cpu.R[13] = 0x03007FE0;
    cpu.banked_sp[armv4t::Bank_Supervisor] = 0x03007FE0;
    cpu.banked_sp[armv4t::Bank_IRQ]        = 0x03007FA0;
    cpu.banked_sp[armv4t::Bank_User]       = 0x03007F00;
    return cpu;
}

}  // namespace

int main(int argc, char** argv) {
    auto args = parse_args(argc, argv);

    // No-args launch (e.g. double-clicked gba.exe from Explorer):
    // default to windowed open-ended mode so the user actually sees
    // the BIOS chime + GAME BOY logo instead of a console flashing.
    // CLI-driven runs (CI, dev) keep the previous headless defaults.
    if (argc <= 1) {
        args.window = true;
        args.quiet = true;
    }

    // 1. Resolve the BIOS path. Released builds end up next to a user
    //    who doesn't know about --bios; the picker falls back to a
    //    Win32 file dialog (and caches the validated path) when the
    //    CLI / default path isn't present. CRC32 + SHA-1 mismatch is a
    //    user-warning, not a hard fail — the user may have a region
    //    revision we haven't catalogued. Wrong SIZE still hard-fails.
    {
        gbarecomp::AssetSpec spec;
        spec.display_name   = "GBA BIOS";
        spec.dialog_filter  = "GBA BIOS (*.bin;*.BIN)\0*.bin;*.BIN\0"
                              "All Files (*.*)\0*.*\0";
        spec.dialog_title   = "Select your GBA BIOS dump (gba_bios.bin)";
        spec.cache_filename = "bios.cfg";
        spec.expected_size  = gba::GbaBios::kSize;
        spec.expected_sha1  = gba::GbaBios::kExpectedSha1;
        spec.expected_crc32 = 0;  // SHA-1 is the gate; CRC32 is informational.
        auto r = gbarecomp::resolve_asset(args.bios, spec, argv[0]);
        if (!r.ok) {
            std::fprintf(stderr, "bios_smoke: %s\n", r.error.c_str());
#if defined(_WIN32)
            MessageBoxA(nullptr, r.error.c_str(), "GBA BIOS",
                        MB_OK | MB_ICONERROR);
#endif
            return 1;
        }
        args.bios = r.path;
    }

    // 2. Load the BIOS bytes. The picker has already verified SHA-1
    //    (warn-and-try semantics); don't re-validate strictly here or
    //    a non-canonical-but-warned dump fails the second check.
    gba::GbaBios bios;
    std::string err;
    if (!bios.load_from_file(args.bios, std::string{}, &err)) {
        std::fprintf(stderr, "bios_smoke: %s\n", err.c_str());
#if defined(_WIN32)
        MessageBoxA(nullptr, err.c_str(), "GBA BIOS",
                    MB_OK | MB_ICONERROR);
#endif
        return 1;
    }
    if (!args.quiet) {
        std::printf("bios_loaded sha1=%s size=%zu\n",
                    bios.sha1_hex().c_str(), gba::GbaBios::kSize);
    }

    // 2. Set up the bus with the BIOS attached. EWRAM/IWRAM/PAL/VRAM/OAM
    //    are zero-backed; ROM is absent (this is BIOS-only smoke).
    //    PPU is wired through the bus's IO dispatcher so VCOUNT /
    //    DISPSTAT flag reads reflect live scanline state.
    gba::GbaBus bus;
    gba::GbaPpu ppu;
    bus.set_bios(&bios);
    bus.io().set_ppu(&ppu);
    bus.io().set_bus(&bus);

    // 2b. Optional cartridge. With --rom, bios_smoke becomes a full
    //     interpreter-driven oracle for the game: it runs the same
    //     gba::GbaBus/GbaPpu/GbaIo/GbaAudio the recompiled runtime uses,
    //     so the ONLY difference from the recomp is the CPU execution
    //     path (interpreted vs recompiled). Mirrors runtime.cpp's
    //     set_rom + EEPROM configuration.
    std::vector<uint8_t> rom_bytes;  // must outlive the run
    if (!args.rom.empty()) {
        std::ifstream rf(args.rom, std::ios::binary | std::ios::ate);
        if (!rf) {
            std::fprintf(stderr, "bios_smoke: cannot open ROM %s\n",
                         args.rom.c_str());
            return 1;
        }
        std::streamoff sz = rf.tellg();
        rom_bytes.resize(static_cast<std::size_t>(sz));
        rf.seekg(0, std::ios::beg);
        rf.read(reinterpret_cast<char*>(rom_bytes.data()),
                static_cast<std::streamsize>(rom_bytes.size()));
        gba::RomHeader header = gba::parse_rom(rom_bytes.data(),
                                               rom_bytes.size());
        bus.set_rom(rom_bytes.data(), rom_bytes.size());
        if (header.save_type == gba::SaveType::EEPROM) {
            bus.save().configure_eeprom(8 * 1024);
        }
        if (!args.quiet) {
            std::printf("rom_loaded %s size=%zu save=%d\n",
                        args.rom.c_str(), rom_bytes.size(),
                        static_cast<int>(header.save_type));
        }
    }

    // 3. Reset CPU and step the interpreter.
    auto cpu = make_reset_cpu();

    if (!args.quiet) {
        std::printf("step    pc        mode insn                            "
                    "r0       r1       r2       r3       r12      lr       sp       NZCV result\n");
    }

    uint64_t vblank_irqs_raised = 0;
    uint64_t vblank_count = 0;
    uint64_t cycles_elapsed = 0;
    uint32_t last_step_cycles = 0;
    bool frame_just_completed = false;
    auto pump_ppu = [&](uint32_t cycles) {
        uint32_t remaining = cycles;
        while (remaining != 0) {
            uint32_t chunk = remaining;
            uint32_t until_sample = bus.audio().cycles_until_next_sample();
            uint32_t until_timer = bus.io().cycles_until_next_timer_event();
            if (until_sample < chunk) chunk = until_sample;
            if (until_timer < chunk) chunk = until_timer;
            if (chunk == 0) chunk = 1;

            cycles_elapsed += chunk;
            bus.audio().tick(chunk);
            bus.io().tick_timers(chunk);
            // VCount-match value lives in DISPSTAT[15:8].
            uint16_t vc_compare = static_cast<uint16_t>(
                (bus.io().dispstat() >> 8) & 0xFFu);
            auto events = ppu.tick(chunk, vc_compare);
            uint16_t ds = bus.io().dispstat();
            if (events.hblank_started &&
                ppu.vcount() < gba::GbaPpu::kLinesVisible) {
                ppu.render_scanline(ppu.vcount(),
                                    bus.io().read16(0x000),
                                    bus.io().raw(),
                                    bus.vram_ptr(),
                                    bus.oam_ptr(),
                                    bus.pal_ptr());
            }
            if (events.vblank_started) {
                ++vblank_count;
                ppu.mark_framebuffer_latched();
            }
            if (events.vblank_started && (ds & 0x0008u)) {
                bus.io().request_irq(gba::GbaIo::IrqVBlank);
                ++vblank_irqs_raised;
            }
            if (events.hblank_started && (ds & 0x0010u)) {
                bus.io().request_irq(gba::GbaIo::IrqHBlank);
            }
            if (events.vcount_matched && (ds & 0x0020u)) {
                bus.io().request_irq(gba::GbaIo::IrqVCount);
            }
            if (events.frame_completed) frame_just_completed = true;
            remaining -= chunk;
        }
    };

    // Live window setup (optional). When --window is given we open an
    // SDL2 surface, present at every PPU frame boundary, and let the
    // host close the window or press Escape to terminate. The window
    // path also pulls KEYINPUT from the host keyboard so future ROMs
    // can read input naturally.
    gbarecomp::HostWindow win;
    std::vector<uint8_t> live_fb;
    if (args.window) {
        if (!gbarecomp::HostWindow::is_available()) {
            std::fprintf(stderr,
                         "bios_smoke: --window requested but this build has no SDL2\n");
            return 1;
        }
        if (!win.open(args.scale, "gbarecomp — BIOS")) {
            return 1;
        }
        live_fb.assign(gba::GbaPpu::kFramebufferBytes, 0);
    }

    uint64_t taken = 0;
    uint64_t irq_entries = 0;
    uint64_t halt_steps  = 0;
    uint64_t swi_entries = 0;
    int frames_presented = 0;
    bool host_quit = false;

    // ── Per-instruction fingerprint ring (interp oracle mirror) ───────
    // Mirrors the recomp's runtime_insn_fp ring (runtime_arm.cpp) in the SAME
    // GFP1 binary layout so oracle/diff_cycle_trace.py can align the two engines
    // by cumulative cycle and find the first divergent instruction (MC-HP-002).
    // Armed for the whole run via GBARECOMP_INSN_TRACE (same env as the recomp),
    // so the ring is on from reset and we query it after the fact.
    const bool fp_armed = [] {
        const char* it = std::getenv("GBARECOMP_INSN_TRACE");
        return it && it[0] && it[0] != '0';
    }();
    constexpr size_t kFpSize = 1u << 20;  // ~1M instructions, matches the recomp
    std::vector<RuntimeFpEntry> fp_ring;
    size_t fp_write = 0, fp_count = 0;
    if (fp_armed) fp_ring.resize(kFpSize);
    auto pack_cpsr = [](const armv4t::CPSR& c) -> uint32_t {
        uint32_t v = 0;
        if (c.n) v |= 1u << 31;
        if (c.z) v |= 1u << 30;
        if (c.c) v |= 1u << 29;
        if (c.v) v |= 1u << 28;
        if (c.i) v |= 1u << 7;
        if (c.f) v |= 1u << 6;
        if (c.t) v |= 1u << 5;
        v |= static_cast<uint32_t>(c.mode & 0x1Fu);
        return v;
    };
    auto fp_emit = [&]() {
        if (!fp_armed) return;
        RuntimeFpEntry& e = fp_ring[fp_write];
        e.cycles = cycles_elapsed;  // cumulative cycles BEFORE this instruction
        e.pc = cpu.R[15];
        e.cpsr = pack_cpsr(cpu.cpsr);
        for (int i = 0; i < 16; ++i) e.r[i] = cpu.R[i];
        fp_write = (fp_write + 1) % kFpSize;
        if (fp_count < kFpSize) ++fp_count;
    };
    auto fp_save = [&](const std::string& path) -> uint32_t {
        if (!fp_armed || fp_count == 0) return 0;
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return 0;
        uint32_t magic = 0x31504647u;  // 'GFP1'
        uint32_t esz = static_cast<uint32_t>(sizeof(RuntimeFpEntry));
        unsigned long long count = fp_count;
        std::fwrite(&magic, sizeof(magic), 1, f);
        std::fwrite(&esz, sizeof(esz), 1, f);
        std::fwrite(&count, sizeof(count), 1, f);
        size_t start = (fp_write + kFpSize - fp_count) % kFpSize;
        for (size_t i = 0; i < fp_count; ++i) {
            std::fwrite(&fp_ring[(start + i) % kFpSize],
                        sizeof(RuntimeFpEntry), 1, f);
        }
        std::fclose(f);
        return static_cast<uint32_t>(fp_count);
    };

    auto run_cpu_step = [&](int trace_index) -> bool {
        uint32_t step_cycles = 0;
        auto pump_step = [&](uint32_t cycles) {
            pump_ppu(cycles);
            step_cycles += cycles;
        };

        if (bus.io().irq_pending() && !cpu.cpsr.i) {
            if (bus.io().halted()) bus.io().clear_halt();
            armv4t::Interpreter::enter_irq(cpu, cpu.R[15]);
            ++irq_entries;
        }

        if (bus.io().halted()) {
            ++halt_steps;
            while (bus.io().halted() && !bus.io().irq_pending()) {
                uint32_t chunk = ppu.cycles_until_next_event();
                if (chunk == 0) chunk = 1;
                pump_step(chunk);
            }
            if (bus.io().irq_pending() && !cpu.cpsr.i) {
                pump_step(kGbaIrqDelayCycles);
                bus.io().clear_halt();
                armv4t::Interpreter::enter_irq(cpu, cpu.R[15]);
                ++irq_entries;
            } else {
                last_step_cycles = step_cycles;
                ++taken;
                return true;
            }
        }

        // Fingerprint the about-to-execute instruction (post IRQ/halt handling,
        // so an IRQ-vector entry is captured here just as the recomp captures
        // it at the vector function's prologue). Only reached when we actually
        // step the CPU — the halted-no-IRQ path returns above without emitting.
        fp_emit();

        uint32_t pc = cpu.R[15];
        armv4t::Instr insn{};
        if (cpu.thumb) {
            uint16_t hw = bus.read16(pc);
            insn = armv4t::ThumbDecoder::decode(hw, pc);
        } else {
            uint32_t word = bus.read32(pc);
            insn = armv4t::ArmDecoder::decode(word, pc);
        }
        uint32_t insn_cycles = 1;
        auto r = armv4t::Interpreter::step(cpu, bus, insn, &insn_cycles);
        if (r == armv4t::Interpreter::Result::Swi) {
            uint32_t next_pc = pc + (cpu.thumb ? 2u : 4u);
            armv4t::Interpreter::enter_swi(cpu, next_pc, cpu.thumb);
            ++swi_entries;
        }
        pump_step(insn_cycles);
        last_step_cycles = step_cycles;

        if (!args.quiet && trace_index >= 0) {
            const char* r_name = "?";
            switch (r) {
                case armv4t::Interpreter::Result::Normal:         r_name = "Normal";    break;
                case armv4t::Interpreter::Result::Branched:       r_name = "Branched";  break;
                case armv4t::Interpreter::Result::Swi:            r_name = "Swi";       break;
                case armv4t::Interpreter::Result::Undefined:      r_name = "Undefined"; break;
                case armv4t::Interpreter::Result::NotImplemented: r_name = "NotImpl";   break;
            }
            std::printf("%6d %08x  %s   %-32s %08x %08x %08x %08x %08x %08x %08x %d%d%d%d %s\n",
                        trace_index, pc, cpu.thumb ? "T" : "A",
                        armv4t::format_ir(insn).c_str(),
                        cpu.R[0], cpu.R[1], cpu.R[2], cpu.R[3],
                        cpu.R[12], cpu.R[14], cpu.R[13],
                        cpu.cpsr.n ? 1 : 0, cpu.cpsr.z ? 1 : 0,
                        cpu.cpsr.c ? 1 : 0, cpu.cpsr.v ? 1 : 0,
                        r_name);
        }

        ++taken;
        if (r == armv4t::Interpreter::Result::Undefined ||
            r == armv4t::Interpreter::Result::NotImplemented) {
            return false;
        }
        return true;
    };

    // Per-CPU-step body used by the TCP server. Returns false on
    // Undefined / NotImplemented; the caller terminates in that case.
    auto run_one_cpu_step = [&]() -> bool {
        return run_cpu_step(-1);
    };

    // Step until the PPU reaches the next VBlank start. mGBA's
    // runFrame returns at this boundary, so the TCP frame clock uses
    // VBlank starts rather than scanline wrap.
    auto step_one_frame = [&]() -> bool {
        frame_just_completed = false;
        uint64_t start = vblank_count;
        while (vblank_count == start) {
            if (!run_one_cpu_step()) return false;
        }
        return true;
    };

    // Restore a runtime savestate INTO THE INTERPRETER (offline oracle).
    // load_state restores memory/IO/PPU/devices plus the recomp global
    // g_cpu; we then map g_cpu (ArmCpuState) into the interpreter's CPUState
    // (a different struct) and drop the recomp-only host call-return stack
    // (the interpreter resumes purely from R[15] + the guest stack in IWRAM).
    // The ROM-SHA1 gate is skipped (rom_sha1=nullptr) since the oracle is
    // launched with the matching --rom. This lets the interpreter execute
    // the SAME ARM code from a recomp savestate, so we can diff intended vs
    // recompiled behavior at a transition (e.g. an entity's animation index).
    auto do_savestate_load = [&](const std::string& path,
                                 std::string& e) -> bool {
        gbarecomp::debug::SnapshotContext sc;
        sc.bus = &bus;
        sc.ppu = &ppu;
        sc.rom_sha1 = nullptr;
        sc.taken = &taken;
        sc.cycles_elapsed = &cycles_elapsed;
        sc.vblank_count = &vblank_count;
        if (!gbarecomp::debug::load_state(path.c_str(), sc, &e)) return false;
        const ArmCpuState& g = g_cpu;  // populated by load_state
        for (int i = 0; i < 16; ++i) cpu.R[i] = g.R[i];
        cpu.cpsr.n = (g.cpsr >> 31) & 1u;
        cpu.cpsr.z = (g.cpsr >> 30) & 1u;
        cpu.cpsr.c = (g.cpsr >> 29) & 1u;
        cpu.cpsr.v = (g.cpsr >> 28) & 1u;
        cpu.cpsr.i = (g.cpsr >> 7) & 1u;
        cpu.cpsr.f = (g.cpsr >> 6) & 1u;
        cpu.cpsr.t = (g.cpsr >> 5) & 1u;
        cpu.cpsr.mode = g.cpsr & 0x1Fu;
        for (int i = 0; i < 6; ++i) {
            cpu.banked_sp[i]   = g.banked_sp[i];
            cpu.banked_lr[i]   = g.banked_lr[i];
            cpu.banked_spsr[i] = g.banked_spsr[i];
        }
        for (int i = 0; i < 5; ++i) {
            cpu.r8_12_user[i] = g.r8_12_user[i];
            cpu.r8_12_fiq[i]  = g.r8_12_fiq[i];
        }
        cpu.thumb = cpu.cpsr.t;
        vblank_count = ppu.frame_count();  // re-sync frame clock to restored PPU
        // Re-origin the fingerprint cycle clock + ring at the load point so the
        // interp and recomp share a cycle origin for diff_cycle_trace.py. The
        // snapshot restored cycles_elapsed to the (large) absolute count saved in
        // the state; the recomp's g_runtime_cycles is NOT snapshotted and starts
        // at 0, so without this the two clocks were ~860x apart and the diff's
        // cycle-based anchor picked the wrong loop iteration. We diff relative to
        // the load, so zeroing both is correct.
        cycles_elapsed = 0;
        fp_write = 0;
        fp_count = 0;
        return true;
    };

    // TCP debug server mode. Open the listener, hand it the live
    // CPU/bus/PPU plus the step callback, block until the client
    // disconnects. No --frames / --window logic in this path; the
    // client drives execution explicitly via `step` commands.
    if (args.tcp_port > 0) {
        gbarecomp::debug::TcpDebugServer server;
        gbarecomp::debug::TcpDebugServer::Context srv_ctx;
        srv_ctx.cpu = &cpu;
        srv_ctx.bus = &bus;
        srv_ctx.ppu = &ppu;
        srv_ctx.step      = step_one_frame;
        srv_ctx.step_inst = run_one_cpu_step;
        srv_ctx.savestate_load = do_savestate_load;
        srv_ctx.fp_save        = fp_save;
        srv_ctx.irq_entries        = &irq_entries;
        srv_ctx.swi_entries        = &swi_entries;
        srv_ctx.halt_steps         = &halt_steps;
        srv_ctx.vblank_irqs_raised = &vblank_irqs_raised;
        srv_ctx.steps              = &taken;
        srv_ctx.cycles_elapsed     = &cycles_elapsed;
        srv_ctx.last_step_cycles   = &last_step_cycles;
        srv_ctx.sync_frames        = &vblank_count;
        server.run(args.tcp_port, srv_ctx);
        return 0;
    }

    // When --window or --frames is set without an explicit --steps,
    // run open-ended (capped at INT_MAX/2) and let the frame cap or
    // window quit terminate the loop instead.
    const bool open_ended = (args.window || args.frames >= 0);
    const int step_budget = open_ended
        ? (args.steps > 16 ? args.steps : 0x7FFFFFFF / 2)
        : args.steps;
    for (int i = 0; i < step_budget && !host_quit; ++i) {
        if (!run_cpu_step(i)) break;
        if (args.window && frame_just_completed) {
            frame_just_completed = false;
            if (ppu.has_latched_framebuffer()) {
                std::memcpy(live_fb.data(), ppu.latched_framebuffer(),
                            gba::GbaPpu::kFramebufferBytes);
            } else {
                uint16_t live_dispcnt = bus.io().read16(0x000);
                ppu.render(live_fb.data(), live_dispcnt,
                           bus.io().raw(),
                           bus.vram_ptr(), bus.oam_ptr(), bus.pal_ptr());
            }
            win.present(live_fb.data());
            // Drain any audio samples generated this frame and feed
            // them to the host. ~547 samples per frame at 32768 Hz.
            int16_t audio_buf[2048];
            std::size_t n = bus.audio().drain_samples(audio_buf, 2048);
            if (n > 0) win.push_audio_samples(audio_buf, n);
            auto ev = win.pump();
            bus.io().set_keyinput(ev.keyinput);
            if (ev.quit) host_quit = true;
            ++frames_presented;
            if (args.frames >= 0 && frames_presented >= args.frames) {
                host_quit = true;
            }
        }
        if (!args.window && args.frames >= 0 &&
            ppu.frame_count() >= static_cast<uint64_t>(args.frames)) break;
    }
    if (args.window) win.close();

    // Final-state summary always prints, including under --quiet, so
    // long runs can be benchmarked / regression-checked without
    // dumping per-step traces.
    std::printf("\nfinal_pc=0x%08x thumb=%d unmapped=%zu io_unhandled=%zu "
                "steps=%llu ppu_vcount=%u ppu_frames=%llu frames_presented=%d\n",
                cpu.R[15], cpu.thumb ? 1 : 0,
                bus.unmapped_count(), bus.io().unmapped_count(),
                static_cast<unsigned long long>(taken),
                static_cast<unsigned>(ppu.vcount()),
                static_cast<unsigned long long>(ppu.frame_count()),
                frames_presented);

    // Sanity probes — count non-zero bytes in PAL, VRAM, OAM. If the
    // BIOS rendered anything, PAL will have palette entries; VRAM
    // will have tile data; OAM may have sprite entries.
    auto count_nonzero = [](const uint8_t* p, std::size_t n) {
        std::size_t c = 0;
        for (std::size_t k = 0; k < n; ++k) if (p[k]) ++c;
        return c;
    };
    std::printf("pal_nonzero=%zu/1024  vram_nonzero=%zu/98304  oam_nonzero=%zu/1024\n",
                count_nonzero(bus.pal_ptr(),  1024),
                count_nonzero(bus.vram_ptr(), 96 * 1024),
                count_nonzero(bus.oam_ptr(),  1024));
    // Print the first 32 PAL entries (as 16-bit colors) — the BIOS
    // intro sets a specific palette.
    std::printf("pal[0..15]:");
    const uint8_t* pal = bus.pal_ptr();
    for (int k = 0; k < 16; ++k) {
        uint16_t v = static_cast<uint16_t>(pal[k * 2] | (pal[k * 2 + 1] << 8));
        std::printf(" %04x", v);
    }
    std::printf("\n");
    // DISPCNT — knowing the BG mode tells us how to interpret VRAM.
    uint16_t final_dispcnt = bus.io().read16(0x000);
    {
        const uint8_t* io = bus.io().raw();
        uint16_t bg0cnt = io[0x08] | (io[0x09] << 8);
        uint16_t bg1cnt = io[0x0A] | (io[0x0B] << 8);
        uint16_t bg2cnt = io[0x0C] | (io[0x0D] << 8);
        uint16_t bg3cnt = io[0x0E] | (io[0x0F] << 8);
        int32_t pa = static_cast<int16_t>(io[0x30] | (io[0x31] << 8));
        int32_t pb = static_cast<int16_t>(io[0x32] | (io[0x33] << 8));
        int32_t pc = static_cast<int16_t>(io[0x34] | (io[0x35] << 8));
        int32_t pd = static_cast<int16_t>(io[0x36] | (io[0x37] << 8));
        uint32_t refx_raw = io[0x38] | (io[0x39] << 8) | (io[0x3A] << 16) | (io[0x3B] << 24);
        uint32_t refy_raw = io[0x3C] | (io[0x3D] << 8) | (io[0x3E] << 16) | (io[0x3F] << 24);
        std::printf("BG0CNT=0x%04x BG1CNT=0x%04x BG2CNT=0x%04x BG3CNT=0x%04x\n",
                    bg0cnt, bg1cnt, bg2cnt, bg3cnt);
        std::printf("BG3 details: char_base=%u screen_base=%u wrap=%u size=%u  "
                    "PA=%d PB=%d PC=%d PD=%d  refx=0x%08x refy=0x%08x\n",
                    static_cast<unsigned>((bg3cnt >> 2) & 3),
                    static_cast<unsigned>((bg3cnt >> 8) & 31),
                    static_cast<unsigned>((bg3cnt >> 13) & 1),
                    static_cast<unsigned>((bg3cnt >> 14) & 3),
                    pa, pb, pc, pd, refx_raw, refy_raw);
    }
    std::printf("DISPCNT=0x%04x (mode=%u, bg_enabled=0x%x, obj=%u, forced_blank=%u)\n",
                static_cast<unsigned>(final_dispcnt),
                static_cast<unsigned>(final_dispcnt & 0x7),
                static_cast<unsigned>((final_dispcnt >> 8) & 0xF),
                static_cast<unsigned>((final_dispcnt >> 12) & 1),
                static_cast<unsigned>((final_dispcnt >> 7) & 1));
    std::printf("DISPSTAT=0x%04x  IE=0x%04x  IF=0x%04x  IME=%u\n",
                static_cast<unsigned>(bus.io().dispstat()),
                static_cast<unsigned>(bus.io().ie()),
                static_cast<unsigned>(bus.io().if_reg()),
                static_cast<unsigned>(bus.io().ime() ? 1 : 0));
    std::printf("irq_entries=%llu halt_steps=%llu vblank_irqs_raised=%llu swi_entries=%llu\n",
                static_cast<unsigned long long>(irq_entries),
                static_cast<unsigned long long>(halt_steps),
                static_cast<unsigned long long>(vblank_irqs_raised),
                static_cast<unsigned long long>(swi_entries));
    std::printf("DMA runs: ch0=%zu(%zu w)  ch1=%zu(%zu w)  ch2=%zu(%zu w)  ch3=%zu(%zu w)\n",
                bus.io().dma_runs(0), bus.io().dma_words(0),
                bus.io().dma_runs(1), bus.io().dma_words(1),
                bus.io().dma_runs(2), bus.io().dma_words(2),
                bus.io().dma_runs(3), bus.io().dma_words(3));

    // BIOS_IF flag is at IWRAM offset 0x7FF8 (IWRAM[0x7FF8..7FF9]).
    // The BIOS IRQ handler sets bits here; SWI IntrWait reads them.
    const uint8_t* iwram = bus.iwram_ptr();
    uint16_t bios_if = static_cast<uint16_t>(iwram[0x7FF8] | (iwram[0x7FF9] << 8));
    uint32_t user_irq_handler = static_cast<uint32_t>(
        iwram[0x7FFC]        |
        (iwram[0x7FFD] <<  8) |
        (iwram[0x7FFE] << 16) |
        (iwram[0x7FFF] << 24));
    std::printf("BIOS_IF (IWRAM[0x7FF8])=0x%04x  user_handler_ptr (IWRAM[0x7FFC])=0x%08x\n",
                static_cast<unsigned>(bios_if), user_irq_handler);

    // DMA channel state.
    for (int ch = 0; ch < 4; ++ch) {
        uint32_t base = 0xB0 + ch * 12;  // SAD(4) DAD(4) CNT_L(2) CNT_H(2)
        uint32_t sad = bus.io().read16(base) | (bus.io().read16(base + 2) << 16);
        uint32_t dad = bus.io().read16(base + 4) | (bus.io().read16(base + 6) << 16);
        uint16_t cnt_l = bus.io().read16(base + 8);
        uint16_t cnt_h = bus.io().read16(base + 10);
        if (sad || dad || cnt_l || cnt_h) {
            std::printf("DMA%d: SAD=0x%08x DAD=0x%08x CNT_L=0x%04x CNT_H=0x%04x  "
                        "(en=%u start_mode=%u)\n",
                        ch, sad, dad, cnt_l, cnt_h,
                        (cnt_h >> 15) & 1, (cnt_h >> 12) & 0x3);
        }
    }

    // Raw OAM hex dump — 32 bytes per line, first 256 bytes.
    std::printf("OAM raw (first 256 bytes):\n");
    const uint8_t* oam = bus.oam_ptr();
    for (int row = 0; row < 8; ++row) {
        std::printf("  0x%03x:", row * 32);
        for (int col = 0; col < 32; ++col) std::printf(" %02x", oam[row * 32 + col]);
        std::printf("\n");
    }

    std::printf("OAM enabled / non-empty sprites:\n");
    for (int s = 0; s < 128; ++s) {
        uint16_t a0 = oam[s * 8 + 0] | (oam[s * 8 + 1] << 8);
        uint16_t a1 = oam[s * 8 + 2] | (oam[s * 8 + 3] << 8);
        uint16_t a2 = oam[s * 8 + 4] | (oam[s * 8 + 5] << 8);
        if (a0 == 0 && a1 == 0 && a2 == 0) continue;
        bool rot = (a0 & 0x0100) != 0;
        bool dis_or_dbl = (a0 & 0x0200) != 0;
        int yv = a0 & 0xFF; int xv = a1 & 0x1FF;
        int shape = (a0 >> 14) & 3;
        int size = (a1 >> 14) & 3;
        uint32_t tile_num = a2 & 0x3FF;
        std::printf("  [%2d] a0=%04x a1=%04x a2=%04x  y=%3d x=%3d rot=%d dis=%d shape=%d size=%d tile=%u\n",
                    s, a0, a1, a2, yv, xv, rot, dis_or_dbl, shape, size, tile_num);
    }
    // Find every nonzero PAL slot so we know where colors live.
    std::printf("PAL nonzero slots:\n");
    const uint8_t* pal_bytes = bus.pal_ptr();
    for (int k = 0; k < 512; ++k) {
        uint16_t v = static_cast<uint16_t>(pal_bytes[k * 2] | (pal_bytes[k * 2 + 1] << 8));
        if (v != 0) {
            const char* region = (k < 256) ? "BG" : "OBJ";
            int local = (k < 256) ? k : (k - 256);
            std::printf("  PAL[0x%03x] %s[%d] = 0x%04x\n", k, region, local, v);
        }
    }
    // Dump sprite 7's first ROW of tiles (8x8 each, 8 tiles wide).
    // In 8bpp 1D mapping each tile is 64 bytes, but tile *indices*
    // advance by 2 per tile in the row → addresses go 0x12800,
    // 0x12840, 0x12880, ... 0x129C0.
    const uint8_t* vram = bus.vram_ptr();

    // BG3 region inspection.
    {
        int nz_char0 = 0;
        for (int k = 0; k < 0x4000; ++k) if (vram[k]) ++nz_char0;
        int nz_screen23 = 0;
        for (int k = 0xB800; k < 0xC000; ++k) if (vram[k]) ++nz_screen23;
        std::printf("BG3 char_base=0 region VRAM[0..0x4000]: %d nonzero\n", nz_char0);
        std::printf("BG3 screen_base=23 region VRAM[0xB800..0xC000]: %d nonzero\n", nz_screen23);
        if (nz_screen23 > 0) {
            std::printf("  nonzero tilemap entries:\n");
            for (int k = 0; k < 0x800; ++k) {
                if (vram[0xB800 + k] != 0) {
                    int tile_y = k / 32;
                    int tile_x = k % 32;
                    std::printf("    [%d,%d] vram[0x%05x] = 0x%02x\n",
                                tile_x, tile_y, 0xB800 + k, vram[0xB800 + k]);
                }
            }
        }
        // Sample BG3 tile 0x71 (the first logo tile).
        for (int t : {0x71, 0x72, 0x80}) {
            uint32_t addr = t * 64;
            int nz = 0;
            for (int k = 0; k < 64; ++k) if (vram[addr + k]) ++nz;
            std::printf("  BG3 tile 0x%02x at VRAM[0x%04x]: %d nonzero",
                        t, addr, nz);
            if (nz > 0) {
                std::printf(", first row:");
                for (int k = 0; k < 8; ++k) std::printf(" %02x", vram[addr + k]);
            }
            std::printf("\n");
        }
    }

    std::printf("Sprite 7 row 0, all 8 tiles:\n");
    for (int t = 0; t < 8; ++t) {
        uint32_t ta = 0x12800 + t * 64;  // 1D-mapped 8bpp stride
        std::size_t nz = 0;
        for (int k = 0; k < 64; ++k) if (vram[ta + k]) ++nz;
        std::printf("  tile %d (vram 0x%05x): %zu nonzero\n", t, ta, nz);
        if (nz > 0) {
            std::printf("    top-row bytes: ");
            for (int k = 0; k < 8; ++k) std::printf("%02x ", vram[ta + k]);
            std::printf("\n    mid-row bytes: ");
            for (int k = 0; k < 8; ++k) std::printf("%02x ", vram[ta + 4 * 8 + k]);
            std::printf("\n");
        }
    }
    std::printf("OBJ pal[24..39]:\n");
    const uint8_t* opal = bus.pal_ptr() + 0x200;
    for (int k = 24; k < 40; ++k) {
        uint16_t v = static_cast<uint16_t>(opal[k * 2] | (opal[k * 2 + 1] << 8));
        std::printf("  OBJ[%d] = 0x%04x\n", k, v);
    }

    if (!args.dump_bmp.empty()) {
        std::vector<uint8_t> fb(gba::GbaPpu::kFramebufferBytes, 0);
        if (ppu.has_latched_framebuffer()) {
            std::memcpy(fb.data(), ppu.latched_framebuffer(),
                        gba::GbaPpu::kFramebufferBytes);
        } else {
            ppu.render(fb.data(), final_dispcnt,
                       bus.io().raw(),
                       bus.vram_ptr(), bus.oam_ptr(), bus.pal_ptr());
        }
        if (write_bmp(args.dump_bmp, fb.data(),
                      gba::GbaPpu::kScreenWidth,
                      gba::GbaPpu::kScreenHeight)) {
            std::printf("wrote framebuffer to %s\n", args.dump_bmp.c_str());
        } else {
            std::fprintf(stderr, "bios_smoke: failed to write %s\n",
                         args.dump_bmp.c_str());
        }
    }

    // Raw memory snapshot for oracle diffing. Writes <prefix>.oam,
    // .pal, .vram, .iwram, .io as little-endian byte blobs sized to
    // their hardware region. The oracle (via TCP) returns matching
    // shapes so a single byte-for-byte diff finds the first
    // divergence.
    if (!args.snapshot.empty()) {
        auto write_blob = [&](const std::string& path, const uint8_t* p, std::size_t n) {
            std::ofstream f(path, std::ios::binary);
            if (!f) { std::fprintf(stderr, "bios_smoke: open %s failed\n", path.c_str()); return; }
            f.write(reinterpret_cast<const char*>(p), n);
        };
        write_blob(args.snapshot + ".oam",   bus.oam_ptr(),   1024);
        write_blob(args.snapshot + ".pal",   bus.pal_ptr(),   1024);
        write_blob(args.snapshot + ".vram",  bus.vram_ptr(),  96 * 1024);
        write_blob(args.snapshot + ".iwram", bus.iwram_ptr(), 32 * 1024);
        write_blob(args.snapshot + ".io",    bus.io().raw(),  0x400);
        std::printf("wrote snapshot to %s.{oam,pal,vram,iwram,io}\n",
                    args.snapshot.c_str());
    }
    return 0;
}
