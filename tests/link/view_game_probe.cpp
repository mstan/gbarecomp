// Real-game differential: changing the local view during a live cable session
// must not change even one canonical state byte. Images are supplied locally.
#include "multiplayer_launch.h"
#include "multiplayer_session.h"
#include "gba_bios.h"
#include "sha1.h"
#include "../../tools/link_session/netplay_probe.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <cstdlib>
#include <iomanip>
#include <sstream>

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
    if (argc<5 || argc>8) { std::fprintf(stderr,"usage: view-probe ROM BIOS raw-state-or-cold output-prefix [players [prepare-frames [inputs]]]\n"); return 2; }
    try {
        using namespace gbarecomp;
        auto game=gba_view_probe_game();
        const auto rom=read(argv[1]);
        const unsigned players=argc>5 ? std::stoul(argv[5]) : 2;
        const unsigned prepare=argc>6 ? std::stoul(argv[6]) : 0;
        if(players<2 || players>4) throw std::runtime_error("players must be 2..4");
        const auto rom_hash=gba::sha1(rom.data(),rom.size()).hex();
        gba::GbaBios bios; std::string error;
        if (!bios.load_from_file(argv[2],gba::GbaBios::kExpectedSha1,&error)) throw std::runtime_error(error);
        GbaSessionConfig config;
        config.links={{GbaLinkMedium::Cable,{}}};
        for(unsigned id=0;id<players;++id) {
            config.machines.push_back({id,game.program_id,rom_hash});
            config.links[0].machines.push_back(id); config.input_machines.push_back(id);
        }
        GbaMultiplayerSession session(config); session.set_native_slices(true);
        for (unsigned id=0;id<players;++id) {
            auto& m=session.machine(id); m.bus.set_bios(&bios); m.bus.set_rom(rom.data(),rom.size());
            if(!prepare || std::getenv("GBA_LINK_PROBE_NET_SEAT")) m.ppu.set_rasterization_enabled(false);
            if (game.setup_instance) game.setup_instance(m);
        }
        auto initial=std::string(argv[3])=="cold" ? session.save_state() : read(argv[3]);
        if(prepare) {
            if(!session.load_state(initial,&error)) throw std::runtime_error(error);
            std::vector<std::array<std::uint16_t,4>> inputs(prepare);
            if(argc>7) {
                std::ifstream script(argv[7]); if(!script) throw std::runtime_error("cannot open inputs");
                std::string line; unsigned previous=0; bool first=true;
                while(std::getline(script,line)) {
                    if(line.empty() || line[0]=='#') continue;
                    std::istringstream row(line); unsigned frame,key; std::array<std::uint16_t,4> keys{};
                    if(!(row>>std::setbase(0)>>frame) || frame>=prepare || (!first && frame<=previous)) throw std::runtime_error("invalid input frame");
                    for(unsigned id=0;id<players;++id) { if(!(row>>std::setbase(0)>>key) || key>1023) throw std::runtime_error("invalid buttons"); keys[id]=key; }
                    for(unsigned i=frame;i<prepare;++i) inputs[i]=keys;
                    previous=frame; first=false;
                }
            }
            if(const auto* seat_text=std::getenv("GBA_LINK_PROBE_NET_SEAT")) {
                const unsigned seat=std::stoul(seat_text);
                if(seat>=players) throw std::runtime_error("invalid network seat");
                GbaNetplayPresentation presentation(game.view_policy,true);
                const auto* view=std::getenv("GBA_VIEW_PROBE_VIEW");
                const unsigned width=view ? std::stoul(view) : 240;
                presentation.request_width(width);
                session.machine(seat).ppu.set_presentation_observer(&presentation);
                struct Detach { GbaMultiplayerSession& session; unsigned seat; ~Detach(){session.machine(seat).ppu.set_presentation_observer(nullptr);} } detach{session,seat};
                gba_link_probe_netplay(session,prepare,[&](unsigned frame) { return inputs.at(std::min(frame,prepare-1)); },game.program_id+"/view-probe-v1");
            } else for(unsigned frame=0;frame<prepare;++frame) {
                session.run_frame(std::span(inputs[frame].data(),players)); session.discard_audio_output();
                if(frame%60==0) { std::printf("prepare frame=%u\n",frame+1); std::fflush(stdout); }
            }
            auto state=session.save_state();
            std::ofstream raw(std::string(argv[4])+".state",std::ios::binary);
            raw.write(reinterpret_cast<const char*>(state.data()),state.size());
            if(!raw) throw std::runtime_error("state export failed");
            for(unsigned id=0;id<players;++id) {
                const auto name=std::string(argv[4])+"-seat"+std::to_string(id);
                if(session.machine(id).ppu.rasterization_enabled()) {
                    std::ofstream ppm(name+".ppm",std::ios::binary); ppm<<"P6\n240 160\n255\n";
                    ppm.write(reinterpret_cast<const char*>(session.machine(id).ppu.latched_framebuffer()),240*160*3);
                    if(!ppm) throw std::runtime_error("capture failed");
                }
                std::ofstream ram(name+".ewram",std::ios::binary);
                ram.write(reinterpret_cast<const char*>(session.machine(id).bus.ewram_ptr()),0x40000);
                if(!ram) throw std::runtime_error("RAM export failed");
            }
            std::printf("prepared players=%u frames=%u bytes=%zu\n",players,prepare,state.size());
            return 0;
        }
        constexpr unsigned frames=72;
        std::vector<std::vector<std::uint8_t>> canonical;
        for (const auto mode:{GbaNetplayView::Native,GbaNetplayView::Wide16x9,
                GbaNetplayView::Wide21x9,GbaNetplayView::Wide32x9,GbaNetplayView::Adaptive}) {
            if (!session.load_state(initial,&error)) throw std::runtime_error(error);
            GbaNetplayPresentation presentation(game.view_policy,true);
            session.machine(0).ppu.set_presentation_observer(&presentation);
            const auto width_at=[&](unsigned frame) {
                return mode==GbaNetplayView::Adaptive ?
                    std::array<unsigned,6>{240,284,382,569,317,game.view_policy.max_width}[(frame/12)%6] : gba_netplay_view_width(mode);
            };
            const std::vector<std::uint16_t> input(players);
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
            const auto* pixels=presentation.pixels();
            const unsigned width=presentation.width();
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
