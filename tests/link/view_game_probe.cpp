// Real-game differential: changing the local view during a live cable session
// must not change even one canonical state byte. Images are supplied locally.
#include "multiplayer_launch.h"
#include "multiplayer_session.h"
#include "gba_bios.h"
#include "sha1.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <stdexcept>

gbarecomp::GbaNetplayLaunch gba_view_probe_game();
namespace {
std::vector<std::uint8_t> read(const char* path) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    const auto n=f.tellg();
    if (!f || n<=0 || n>32*1024*1024) throw std::runtime_error("invalid probe input");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(n)); f.seekg(0);
    if (!f.read(reinterpret_cast<char*>(bytes.data()),bytes.size())) throw std::runtime_error("input read failed");
    return bytes;
}
}
int main(int argc,char** argv) {
    if (argc!=5) { std::fprintf(stderr,"usage: view-probe ROM BIOS paired-state output-prefix\n"); return 2; }
    try {
        using namespace gbarecomp;
        auto game=gba_view_probe_game();
        const auto rom=read(argv[1]), initial=read(argv[3]);
        const auto rom_hash=gba::sha1(rom.data(),rom.size()).hex();
        gba::GbaBios bios; std::string error;
        if (!bios.load_from_file(argv[2],gba::GbaBios::kExpectedSha1,&error)) throw std::runtime_error(error);
        GbaSessionConfig config;
        config.machines={{0,game.program_id,rom_hash},{1,game.program_id,rom_hash}};
        config.links={{GbaLinkMedium::Cable,{0,1}}}; config.input_machines={0,1};
        GbaMultiplayerSession session(config); session.set_native_slices(true);
        for (unsigned id=0;id<2;++id) {
            auto& m=session.machine(id); m.bus.set_bios(&bios); m.bus.set_rom(rom.data(),rom.size());
            if (game.setup_instance) game.setup_instance(m);
        }
        constexpr unsigned frames=72;
        std::vector<std::vector<std::uint8_t>> canonical;
        for (const auto mode:{GbaNetplayView::Native,GbaNetplayView::Wide16x9,
                GbaNetplayView::Wide21x9,GbaNetplayView::Wide32x9,GbaNetplayView::Adaptive}) {
            if (!session.load_state(initial,&error)) throw std::runtime_error(error);
            GbaNetplayPresentation presentation(game.view_policy,true);
            if (mode!=GbaNetplayView::Native)
                session.machine(0).ppu.set_presentation_observer(&presentation);
            const auto width_at=[&](unsigned frame) {
                return mode==GbaNetplayView::Adaptive ?
                    std::array<unsigned,6>{240,284,382,569,317,576}[(frame/12)%6] : gba_netplay_view_width(mode);
            };
            const std::array<std::uint16_t,2> input{};
            double simulation_ms=0; unsigned replays=0;
            for (unsigned frame=0;frame<frames;++frame) {
                presentation.request_width(width_at(frame));
                const auto start=std::chrono::steady_clock::now();
                session.run_frame(input); session.discard_audio_output();
                simulation_ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
                const auto state=session.save_state();
                if (mode==GbaNetplayView::Native) canonical.push_back(state);
                else if (state!=canonical[frame]) throw std::runtime_error("view changed canonical state");
                if (mode!=GbaNetplayView::Native && frame==36) {
                    // Restore twelve frames, resize again while replaying, and
                    // require the entire cable session to reach the same state.
                    if (!session.load_state(canonical[24],&error)) throw std::runtime_error(error);
                    for (unsigned i=25;i<=36;++i) {
                        presentation.request_width(width_at(i)); session.run_frame(input); session.discard_audio_output();
                        ++replays;
                    }
                    if (session.save_state()!=state) throw std::runtime_error("view rollback diverged");
                }
            }
            const auto* pixels=mode==GbaNetplayView::Native ? session.machine(0).ppu.latched_framebuffer() : presentation.pixels();
            const unsigned width=mode==GbaNetplayView::Native ? 240 : presentation.width();
            if (!pixels) throw std::runtime_error("no complete presentation frame");
            std::ofstream ppm(std::string(argv[4])+"-"+std::to_string(static_cast<unsigned>(mode))+".ppm",std::ios::binary);
            ppm<<"P6\n"<<width<<" 160\n255\n";
            ppm.write(reinterpret_cast<const char*>(pixels),width*160*3);
            if (!ppm) throw std::runtime_error("capture failed");
            std::printf("view=%u width=%u canonical=identical frames=%u replay=%u render_work_fps=%.2f\n",
                static_cast<unsigned>(mode),width,frames,replays,frames*1000.0/simulation_ms);
            std::fflush(stdout);
            session.machine(0).ppu.set_presentation_observer(nullptr);
        }
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr,"view probe: %s\n",e.what()); return 1; }
}
