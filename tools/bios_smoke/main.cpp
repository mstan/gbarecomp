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
#include <cstring>
#include <string>

#include <fstream>
#include <vector>

#include "arm_decode.h"
#include "arm_ir.h"
#include "cpu_state.h"
#include "gba_bios.h"
#include "gba_bus.h"
#include "gba_ppu.h"
#include "host_window.h"
#include "interpreter.h"
#include "thumb_decode.h"

namespace {

constexpr const char* kDefaultBios = "bios/gba_bios.bin";

struct Args {
    std::string bios = kDefaultBios;
    int  steps   = 16;
    bool quiet   = false;
    std::string dump_bmp;  // optional path for final framebuffer dump
    bool window  = false;
    int  scale   = 3;
    int  frames  = -1;     // when >=0, cap by completed frames not steps
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

    // 1. Load + hash-verify the BIOS.
    gba::GbaBios bios;
    std::string err;
    if (!bios.load_from_file(args.bios, gba::GbaBios::kExpectedSha1, &err)) {
        std::fprintf(stderr, "bios_smoke: %s\n", err.c_str());
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

    // 3. Reset CPU and step the interpreter.
    auto cpu = make_reset_cpu();

    if (!args.quiet) {
        std::printf("step    pc        mode insn                            "
                    "r0       r1       r2       r3       r12      lr       sp       NZCV result\n");
    }

    uint64_t vblank_irqs_raised = 0;
    bool frame_just_completed = false;
    auto pump_ppu = [&](uint32_t cycles) {
        // VCount-match value lives in DISPSTAT[15:8].
        uint16_t vc_compare = static_cast<uint16_t>((bus.io().dispstat() >> 8) & 0xFFu);
        auto events = ppu.tick(cycles, vc_compare);
        uint16_t ds = bus.io().dispstat();
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

    int taken = 0;
    uint64_t irq_entries = 0;
    uint64_t halt_steps  = 0;
    int frames_presented = 0;
    bool host_quit = false;
    // When --window is set without --steps we run open-ended.
    // INT_MAX/2 keeps the existing `i < steps` form usable.
    const int step_budget = args.window
        ? (args.steps > 16 ? args.steps : 0x7FFFFFFF / 2)
        : args.steps;
    for (int i = 0; i < step_budget && !host_quit; ++i) {
        // Check pending IRQ before fetching. A pending IRQ also wakes
        // the CPU out of HALT.
        if (bus.io().irq_pending() && !cpu.cpsr.i) {
            if (bus.io().halted()) bus.io().clear_halt();
            armv4t::Interpreter::enter_irq(cpu, cpu.R[15]);
            ++irq_entries;
        }

        // HALT: skip instruction execution but keep advancing time
        // until an IRQ becomes pending and CPSR.I allows entry. Tick
        // a chunk of cycles at once so HALT doesn't burn 1 step per
        // cycle of real time.
        if (bus.io().halted()) {
            pump_ppu(64);
            ++halt_steps;
            ++taken;
            if (args.window && frame_just_completed) {
                frame_just_completed = false;
                uint16_t live_dispcnt = bus.io().read16(0x000);
                ppu.render(live_fb.data(), live_dispcnt,
                           bus.io().raw(),
                           bus.vram_ptr(), bus.oam_ptr(), bus.pal_ptr());
                win.present(live_fb.data());
                auto ev = win.pump();
                bus.io().set_keyinput(ev.keyinput);
                if (ev.quit) host_quit = true;
                ++frames_presented;
                if (args.frames >= 0 && frames_presented >= args.frames) {
                    host_quit = true;
                }
            }
            continue;
        }

        uint32_t pc = cpu.R[15];
        armv4t::Instr insn{};
        if (cpu.thumb) {
            uint16_t hw = bus.read16(pc);
            insn = armv4t::ThumbDecoder::decode(hw, pc);
        } else {
            uint32_t word = bus.read32(pc);
            insn = armv4t::ArmDecoder::decode(word, pc);
        }
        auto r = armv4t::Interpreter::step(cpu, bus, insn);
        // Advance hardware time. ARM7TDMI instructions on the GBA
        // BIOS region average ~1 cycle each (BIOS is in zero-waitstate
        // ROM, plus the prefetch buffer hides most pipeline cost).
        // Real cycle accuracy lands with the scheduler in Phase 3.
        pump_ppu(1);
        if (!args.quiet) {
            const char* r_name = "?";
            switch (r) {
                case armv4t::Interpreter::Result::Normal:         r_name = "Normal";    break;
                case armv4t::Interpreter::Result::Branched:       r_name = "Branched";  break;
                case armv4t::Interpreter::Result::Swi:            r_name = "Swi";       break;
                case armv4t::Interpreter::Result::Undefined:      r_name = "Undefined"; break;
                case armv4t::Interpreter::Result::NotImplemented: r_name = "NotImpl";   break;
            }
            std::printf("%6d %08x  %s   %-32s %08x %08x %08x %08x %08x %08x %08x %d%d%d%d %s\n",
                        i, pc, cpu.thumb ? "T" : "A",
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
            break;
        }

        if (args.window && frame_just_completed) {
            frame_just_completed = false;
            // Render the current PPU snapshot into the live buffer and
            // upload to the host texture.
            uint16_t live_dispcnt = bus.io().read16(0x000);
            ppu.render(live_fb.data(), live_dispcnt,
                       bus.io().raw(),
                       bus.vram_ptr(), bus.oam_ptr(), bus.pal_ptr());
            win.present(live_fb.data());
            auto ev = win.pump();
            bus.io().set_keyinput(ev.keyinput);
            if (ev.quit) host_quit = true;
            ++frames_presented;
            if (args.frames >= 0 && frames_presented >= args.frames) {
                host_quit = true;
            }
        }
    }
    if (args.window) win.close();

    // Final-state summary always prints, including under --quiet, so
    // long runs can be benchmarked / regression-checked without
    // dumping per-step traces.
    std::printf("\nfinal_pc=0x%08x thumb=%d unmapped=%zu io_unhandled=%zu "
                "steps=%d ppu_vcount=%u ppu_frames=%llu frames_presented=%d\n",
                cpu.R[15], cpu.thumb ? 1 : 0,
                bus.unmapped_count(), bus.io().unmapped_count(), taken,
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
    std::printf("irq_entries=%llu halt_steps=%llu vblank_irqs_raised=%llu\n",
                static_cast<unsigned long long>(irq_entries),
                static_cast<unsigned long long>(halt_steps),
                static_cast<unsigned long long>(vblank_irqs_raised));
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
        ppu.render(fb.data(), final_dispcnt,
                   bus.io().raw(),
                   bus.vram_ptr(), bus.oam_ptr(), bus.pal_ptr());
        if (write_bmp(args.dump_bmp, fb.data(),
                      gba::GbaPpu::kScreenWidth,
                      gba::GbaPpu::kScreenHeight)) {
            std::printf("wrote framebuffer to %s\n", args.dump_bmp.c_str());
        } else {
            std::fprintf(stderr, "bios_smoke: failed to write %s\n",
                         args.dump_bmp.c_str());
        }
    }
    return 0;
}
