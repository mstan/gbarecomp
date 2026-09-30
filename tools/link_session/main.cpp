// Headless two- to four-cartridge qualification. Images and saves are read-only inputs;
// all speculative save writes remain inside the snapshotted guest hardware.
#include "multiplayer_session.h"
#include "gba_bios.h"
#include "sha1.h"
#include <charconv>
#include <cstdio>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <span>
#include <string_view>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <chrono>
#ifdef GBA_LINK_PROBE_NETPLAY
#include "netplay_probe.h"
#endif
#ifdef GBA_LINK_PROBE_SETUP
void gba_link_probe_setup(gbarecomp::GbaInstance&);
#endif

namespace {
std::vector<std::uint8_t> read_file(const char* path, std::size_t limit) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error(std::string("cannot read ") + path);
    const auto length = f.tellg();
    if (length <= 0 || static_cast<std::uint64_t>(length) > limit)
        throw std::runtime_error("input file has invalid length");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    f.seekg(0);
    if (!f.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
        throw std::runtime_error("short input file read");
    return bytes;
}
}
int main(int argc, char** argv) {
    unsigned players=2;
    if (const auto* value=std::getenv("GBA_LINK_PROBE_PLAYERS")) {
        const std::string_view text(value);
        const auto parsed=std::from_chars(text.data(),text.data()+text.size(),players);
        if (parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size() || players<2 || players>4) {
            std::fprintf(stderr,"GBA_LINK_PROBE_PLAYERS must be 2, 3 or 4\n");
            return 2;
        }
    }
    if (argc != 5 && argc != 5+static_cast<int>(players)) {
        std::fprintf(stderr, "usage: gba_link_probe ROM BIOS FRAMES SAVE_TYPE [SAVE0 .. SAVE(N-1)]\n"
            "SAVE_TYPE: flash1m, flash512, sram, eeprom8k, eeprom512, none\n"
            "GBA_LINK_PROBE_PLAYERS: consoles on the cable, 2 (default) to 4.\n"
            "GBA_LINK_PROBE_SLICES=1 enables experimental native batching.\n"
            "GBA_LINK_PROBE_COSTS=1: per-frame simulate/serialize/digest/restore cost.\n"
            "GBA_LINK_PROBE_INPUTS: rows of relative frame, then one button word per console.\n"
            "GBA_LINK_PROBE_STATE_IN / STATE_OUT: paired session snapshots.\n"
            "GBA_LINK_PROBE_CAPTURE_PREFIX: write each console's final PPM.\n"
            "GBA_LINK_PROBE_REPLAY=1: verify five additional frames after restore.\n"
            "GBA_LINK_PROBE_REPLAY_RUN=1: restore and verify the full local input segment.\n");
        return 2;
    }
    std::unique_ptr<gbarecomp::GbaMultiplayerSession> session;
    try {
        unsigned frames = 0;
        const std::string_view value(argv[3]);
        const auto parsed = std::from_chars(value.data(), value.data()+value.size(), frames);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data()+value.size() || !frames)
            throw std::runtime_error("FRAMES must be a positive integer");
        const auto rom = read_file(argv[1], 32*1024*1024);
        if (gba::sha1(rom.data(),rom.size()).hex() != GBA_LINK_PROBE_SHA1)
            throw std::runtime_error("ROM does not match the generated program's SHA-1");
        gba::GbaBios bios;
        std::string error;
        if (!bios.load_from_file(argv[2],gba::GbaBios::kExpectedSha1,&error))
            throw std::runtime_error(error);
        using namespace gbarecomp;
        GbaSessionConfig config;
        for (unsigned port=0; port<players; ++port)
            config.machines.push_back({port,GBA_LINK_PROBE_PROGRAM,GBA_LINK_PROBE_SHA1});
        auto medium=GbaLinkMedium::Cable;
        if (const auto* requested=std::getenv("GBA_LINK_PROBE_DEVICE")) {
            if (std::string_view(requested)=="wireless") medium=GbaLinkMedium::Wireless;
            else if (std::string_view(requested)!="cable") throw std::runtime_error("unknown probe serial device");
        }
        config.links = {{medium,{}}};
        for (unsigned port=0; port<players; ++port) {
            config.links[0].machines.push_back(port);
            config.input_machines.push_back(port);
        }
        session = std::make_unique<GbaMultiplayerSession>(std::move(config));
        if (const auto* slices = std::getenv("GBA_LINK_PROBE_SLICES"))
            session->set_native_slices(std::string_view(slices)=="1");
        if (std::getenv("GBA_LINK_PROBE_EXCEPTION_YIELDS")) session->set_return_yields(false);
        for (unsigned port=0; port<players; ++port) {
            auto& m = session->machine(port);
            m.bus.set_bios(&bios);
            m.bus.set_rom(rom.data(),rom.size());
#ifdef GBA_LINK_PROBE_SETUP
            gba_link_probe_setup(m);
#endif
            auto& save = m.bus.save();
            const std::string_view type(argv[4]);
            if (type=="flash1m") save.configure_flash(128*1024);
            else if (type=="flash512") save.configure_flash(64*1024);
            else if (type=="sram") save.configure_sram();
            else if (type=="eeprom8k") save.configure_eeprom(8192);
            else if (type=="eeprom512") save.configure_eeprom(512);
            else if (type!="none") throw std::runtime_error("unknown save type");
            if (argc>5) {
                const auto bytes=read_file(argv[5+port],128*1024);
                const bool ok=save.flash_enabled() ? save.load_flash_bytes(bytes.data(),bytes.size()) :
                    save.sram_enabled() ? save.load_sram_bytes(bytes.data(),bytes.size()) :
                    save.eeprom_enabled() && save.load_eeprom_bytes(bytes.data(),bytes.size());
                if (!ok) throw std::runtime_error("save image has the wrong size/type");
                save.clear_dirty();
            }
        }
        // Exercise snapshot qualification before executing a single instruction.
        auto cold=session->save_state();
        if (const auto* path=std::getenv("GBA_LINK_PROBE_STATE_IN")) cold=read_file(path,8*1024*1024);
        if (!session->load_state(cold,&error)) throw std::runtime_error(error);
        using Buttons=std::array<std::uint16_t,4>;
        struct Input { unsigned frame; Buttons buttons; };
        std::vector<Input> script;
        if (const auto* path=std::getenv("GBA_LINK_PROBE_INPUTS")) {
            std::ifstream input(path);
            if (!input) throw std::runtime_error("cannot read input script");
            std::string line;
            while (std::getline(input,line)) {
                std::istringstream row(line);
                row>>std::ws;
                if (row.eof()) continue;
                unsigned frame;
                Buttons keys{};
                if (!(row>>std::setbase(0)>>frame)) throw std::runtime_error("malformed input script row");
                for (unsigned port=0; port<players; ++port) {
                    unsigned key;
                    if (!(row>>std::setbase(0)>>key) || key>0x3ff) throw std::runtime_error("malformed input script row");
                    keys[port]=static_cast<std::uint16_t>(key);
                }
                if (!(row>>std::ws).eof()) throw std::runtime_error("malformed input script row");
                if (frame>=frames || (!script.empty() && frame<=script.back().frame))
                    throw std::runtime_error("invalid input script row");
                script.push_back({frame,keys});
            }
            if (!input.eof()) throw std::runtime_error("cannot read input script");
        }
        Buttons buttons{};
        const auto keys=[&] { return std::span<const std::uint16_t>(buttons.data(),players); };
        const bool costs=std::getenv("GBA_LINK_PROBE_COSTS")!=nullptr;
        std::vector<double> save_ms, digest_ms, load_ms;
        std::size_t snapshot_bytes=0;
        std::vector<double> frame_ms;
        frame_ms.reserve(frames);
        std::size_t input_index=0;
        const bool replay_run=std::getenv("GBA_LINK_PROBE_REPLAY_RUN")!=nullptr;
        if (replay_run && std::getenv("GBA_LINK_PROBE_NET_SEAT"))
            throw std::runtime_error("full-segment replay is a local probe option; use the network rollback harness for peers");
        if (std::getenv("GBA_LINK_PROBE_NET_SEAT")) {
#ifdef GBA_LINK_PROBE_NETPLAY
            const auto inputs=[&](std::uint32_t tick) {
                const auto after=std::upper_bound(script.begin(),script.end(),tick,
                    [](auto t,const auto& row) { return t<row.frame; });
                return after==script.begin() ? Buttons{} : std::prev(after)->buttons;
            };
            gba_link_probe_netplay(*session,frames,inputs,std::string(GBA_LINK_PROBE_PROGRAM)+
                (medium==GbaLinkMedium::Wireless ? "/wireless-probe-v1" : "/native-probe-v1"));
            buttons=inputs(frames-1);
#else
            throw std::runtime_error("network probe requires GBARECOMP_NETPLAY=ON");
#endif
        } else for (unsigned frame=0; frame<frames; ++frame) {
            if (input_index<script.size() && script[input_index].frame==frame)
                buttons=script[input_index++].buttons;
            using Clock=std::chrono::steady_clock;
            const auto elapsed=[](Clock::time_point since) {
                return std::chrono::duration<double,std::milli>(Clock::now()-since).count();
            };
            const auto frame_start=Clock::now();
            session->run_frame(keys());
            session->discard_audio_output();
            frame_ms.push_back(elapsed(frame_start));
            if (costs) {
                // What a rollback peer adds per forward tick: one boundary
                // serialization (shared by the snapshot and the digest), its
                // digest, and one restore per correction episode.
                auto phase=Clock::now();
                const auto state=session->save_state();
                save_ms.push_back(elapsed(phase)); snapshot_bytes=state.size();
                phase=Clock::now();
                volatile auto digest=gbarecomp::GbaMultiplayerSession::state_digest(state); (void)digest;
                digest_ms.push_back(elapsed(phase));
                phase=Clock::now();
                if (!session->load_state(state,&error)) throw std::runtime_error(error);
                load_ms.push_back(elapsed(phase));
            }
            if (frame%10==0 || frame+1==frames) {
                std::printf("frame=%u cycles=%llu", frame+1, static_cast<unsigned long long>(session->cycle()));
                for (unsigned port=0; port<players; ++port)
                    std::printf(" pc%u=%08x",port,session->machine(port).execution.cpu.R[15]);
                std::printf(" hash=%08x\n",session->state_hash());
                std::fflush(stdout);
            }
        }
        if (!frame_ms.empty()) {
            double total=0; for (auto ms:frame_ms) total+=ms;
            std::sort(frame_ms.begin(),frame_ms.end());
            std::printf("simulation players=%u frames=%zu fps=%.2f mean_ms=%.3f p95_ms=%.3f max_ms=%.3f\n",
                players,frame_ms.size(),frame_ms.size()*1000.0/total,total/frame_ms.size(),
                frame_ms[(frame_ms.size()-1)*95/100],frame_ms.back());
        }
        if (costs && !save_ms.empty()) {
            const auto summary=[](const char* name,std::vector<double> ms) {
                double total=0; for (auto v:ms) total+=v;
                std::sort(ms.begin(),ms.end());
                std::printf("cost %s mean_ms=%.3f p95_ms=%.3f max_ms=%.3f\n",name,total/ms.size(),
                    ms[(ms.size()-1)*95/100],ms.back());
            };
            summary("save_state",save_ms); summary("state_digest",digest_ms); summary("load_state",load_ms);
            std::printf("cost snapshot_bytes=%zu players=%u\n",snapshot_bytes,players);
        }
        if (replay_run) {
            const auto expected=session->save_state();
            if (!session->load_state(cold,&error)) throw std::runtime_error(error);
            buttons={}; input_index=0;
            for (unsigned frame=0; frame<frames; ++frame) {
                if (input_index<script.size() && script[input_index].frame==frame)
                    buttons=script[input_index++].buttons;
                session->run_frame(keys());
                session->discard_audio_output();
            }
            if (session->save_state()!=expected)
                throw std::runtime_error("commercial probe full-segment replay diverged");
            std::printf("whole-session input replay matched (%u frames, %zu snapshot bytes)\n",
                frames,expected.size());
        }
        if (std::getenv("GBA_LINK_PROBE_REPLAY")) {
            const auto before=session->save_state();
            for (unsigned i=0;i<5;++i) { session->run_frame(keys()); session->discard_audio_output(); }
            const auto after=session->save_state();
            if (!session->load_state(before,&error)) throw std::runtime_error(error);
            for (unsigned i=0;i<5;++i) { session->run_frame(keys()); session->discard_audio_output(); }
            if (session->save_state()!=after) throw std::runtime_error("commercial probe replay diverged");
            std::printf("five-frame whole-session replay matched (%zu snapshot bytes)\n",after.size());
        }
        if (const auto* path=std::getenv("GBA_LINK_PROBE_STATE_OUT")) {
            const auto state=session->save_state();
            std::ofstream out(path,std::ios::binary);
            if (!out.write(reinterpret_cast<const char*>(state.data()),state.size()))
                throw std::runtime_error("cannot write probe snapshot");
        }
        if (const auto* prefix=std::getenv("GBA_LINK_PROBE_CAPTURE_PREFIX")) for (unsigned port=0;port<players;++port) {
            const auto path=std::string(prefix)+std::to_string(port)+".ppm";
            std::ofstream out(path,std::ios::binary);
            out<<"P6\n240 160\n255\n";
            if (!out.write(reinterpret_cast<const char*>(session->machine(port).ppu.latched_framebuffer()),
                gba::GbaPpu::kFramebufferBytes)) throw std::runtime_error("cannot write probe frame");
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr,"link probe: %s\n",e.what());
        if (session) for (unsigned port=0; port<session->machine_count(); ++port) {
            const auto& cpu=session->machine(port).execution.cpu;
            std::fprintf(stderr,"machine=%u pc=%08x cpsr=%08x cycles=%llu\n",port,cpu.R[15],cpu.cpsr,
                static_cast<unsigned long long>(session->cycle()));
            std::fprintf(stderr,"sp=%08x lr=%08x opcode=",cpu.R[13],cpu.R[14]);
            // Only RAM remains owned by the instance here; ROM/BIOS inputs
            // have left their scope during exception unwinding.
            if (cpu.R[15]>=0x02000000 && cpu.R[15]<0x04000000) {
                auto& bus=session->machine(port).bus;
                for (unsigned i=0;i<8;++i) std::fprintf(stderr,"%08x ",bus.read32(cpu.R[15]+i*4));
            }
            std::fputc('\n',stderr);
        }
        return 1;
    }
}
