#include "multiplayer_launch.h"
#include "multiplayer_match.h"
#include "multiplayer_pacing.h"
#include "host_window.h"
#include "gba_bios.h"
#include "sha1.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace gbarecomp {
namespace {
void configure_save(gba::GbaSave& save,const SaveConfiguration& config) {
    switch (config.type) {
    case gba::SaveType::SRAM: save.configure_sram(config.size); break;
    case gba::SaveType::EEPROM: save.configure_eeprom(config.size); break;
    case gba::SaveType::Flash512:
    case gba::SaveType::Flash1M: save.configure_flash(config.size); break;
    default: throw std::invalid_argument("netplay cartridge save type is not configured");
    }
}
void load_owner_save(gba::GbaSave& save,const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return; // new cartridge
    std::ifstream input(path,std::ios::binary|std::ios::ate);
    if (!input) throw std::runtime_error("cannot open player's save: "+path.string());
    const auto size=input.tellg();
    if (size<=0 || size>128*1024) throw std::runtime_error("invalid player save length");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(bytes.data()),bytes.size()))
        throw std::runtime_error("cannot read player's save");
    const bool loaded=save.flash_enabled() ? save.load_flash_bytes(bytes.data(),bytes.size()) :
        save.sram_enabled() ? save.load_sram_bytes(bytes.data(),bytes.size()) :
        save.load_eeprom_bytes(bytes.data(),bytes.size());
    if (!loaded) throw std::runtime_error("player save does not fit the cartridge");
    save.clear_dirty();
}
int run(const GbaNetplayLaunch& launch,const GbaNetplayBoot& boot) {
    validate_gba_netplay_launch(launch);
    const auto checkpoint_path=launch.checkpoint_path.empty() ?
        boot.config_directory/"netplay"/("session-"+std::to_string(launch.session_id)+".paired") : launch.checkpoint_path;
    const auto checkpoint_absolute=std::filesystem::weakly_canonical(checkpoint_path);
    for (const auto& original:{boot.local_save,boot.rom_path,boot.bios_path})
        if (!original.empty() && checkpoint_absolute==std::filesystem::weakly_canonical(original))
            throw std::invalid_argument("paired checkpoint must not replace a cartridge save, ROM or BIOS");
    std::filesystem::create_directories(checkpoint_absolute.parent_path());
    if (!boot.bios || !boot.rom || boot.bios->sha1_hex()!=gba::GbaBios::kExpectedSha1 ||
        gba::sha1(boot.rom->data(),boot.rom->size()).hex()!=boot.expected_rom_sha1)
        throw std::invalid_argument("netplay asset verification failed");
    if (!valid_save_size(boot.save.type,boot.save.size))
        throw std::invalid_argument("invalid netplay save hardware");
    GbaSessionConfig config;
    config.machines={{0,launch.program_id,boot.expected_rom_sha1},{1,launch.program_id,boot.expected_rom_sha1}};
    config.links={{GbaLinkMedium::Cable,{0,1}}};
    config.input_machines={launch.seat_machine[0],launch.seat_machine[1]};
    GbaMultiplayerSession simulation(std::move(config));
    simulation.set_native_slices(true);
    for (unsigned id=0;id<2;++id) {
        auto& m=simulation.machine(id);
        m.bus.set_bios(boot.bios); m.bus.set_rom(boot.rom->data(),boot.rom->size());
        configure_save(m.bus.save(),boot.save);
        if (launch.setup_instance) launch.setup_instance(m);
    }
    if (launch.resume_path.empty())
        load_owner_save(simulation.input_machine(launch.local_seat).bus.save(),boot.local_save);
    // Reject unsupported peripheral/enhancement state before opening transport.
    simulation.save_state();
    GbaNetplayMatchOptions options;
    options.network.local_slot=launch.local_seat;
    options.network.session_id=launch.session_id;
    options.network.input_delay=launch.input_delay;
    options.network.occupied_mask=3;
    options.prediction=launch.prediction; options.rollback=launch.rollback;
    options.force_turn=launch.force_turn;
    options.planned_finish_tick=boot.finish_tick;
    options.identity="gba-cable/1:"+launch.program_id+":"+launch.build_identity+":"+
        boot.expected_rom_sha1+":"+boot.bios->sha1_hex()+":"+
        std::to_string(static_cast<unsigned>(boot.save.type))+":"+std::to_string(boot.save.size)+":"+
        std::to_string(launch.seat_machine[0])+":"+std::to_string(launch.seat_machine[1])+":native-lle";
    const auto identity_digest=gba::sha1(reinterpret_cast<const std::uint8_t*>(options.identity.data()),options.identity.size());
    options.build_fingerprint=std::uint32_t(identity_digest.bytes[0])|
        (std::uint32_t(identity_digest.bytes[1])<<8)|(std::uint32_t(identity_digest.bytes[2])<<16)|
        (std::uint32_t(identity_digest.bytes[3])<<24);
    if (!options.build_fingerprint) options.build_fingerprint=1;
    // Wall time is sampled only here; startup exchanges each owner's seed.
    options.rtc_seed_seconds=std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (!launch.resume_path.empty()) {
        GbaConfirmedCheckpoint saved;
        std::string error;
        if (!gba_load_checkpoint_archive(launch.resume_path,options.identity,saved,&error) ||
            !simulation.load_state(saved.state,&error)) throw std::runtime_error("cannot resume paired session: "+error);
        options.restored_pair=true;
    }
    GbaNetplayMatch match(simulation,std::move(options));
    if (rnet_session_start_lan(match.transport(),launch.bind_endpoint.c_str(),launch.peer_endpoint.c_str()))
        throw std::runtime_error("cannot open netplay transport");
    HostWindow window;
    if (!window.open(boot.scale,240,160,boot.title.c_str(),boot.screen.c_str(),
            boot.linear_filter,boot.sharp_filter,false,true))
        throw std::runtime_error("cannot open netplay window");
    window.load_input_config(boot.config_directory.string().c_str(),false);
    window.set_fullscreen(boot.fullscreen); window.set_volume(boot.volume);
    window.set_fps_readout(boot.show_fps);
    std::uint16_t buttons=0;
    match.sample_local=[&](std::uint32_t) { return buttons; };
    GbaNetplayPacer pacer;
    GbaNetplayOutput output;
    std::string previous_title;
    bool closing=false;
    while (true) {
        if (launch.pump_lobby) launch.pump_lobby();
        const auto events=window.pump();
        if (events.terminating) return 0; // keep the previous durable pair
        if (events.quit) {
            if (closing || match.phase()==GbaNetplayMatch::Phase::Starting) return 0;
            closing=true; match.request_checkpoint();
        }
        if (events.save_slot && match.phase()==GbaNetplayMatch::Phase::Running) {
            closing=true; match.request_checkpoint();
        }
        buttons=static_cast<std::uint16_t>(~events.keyinput&0x3ff);
        if (events.toggle_fullscreen) window.set_fullscreen(window.fullscreen() ? 0 : 1);
        if (events.window_bigger) window.adjust_scale(1);
        if (events.window_smaller) window.adjust_scale(-1);
        if (events.volume_up) window.set_volume(window.volume()+5);
        if (events.volume_down) window.set_volume(window.volume()-5);
        if (events.toggle_fps) window.set_fps_readout(!window.fps_readout());
        // Pause, turbo, rewind and single-machine load never reach this
        // simulation. A save hotkey requests a confirmed paired save-and-leave.
        const auto now=GbaNetplayPacer::Clock::now();
        const auto step=match.poll(pacer.ready(now));
        if (step==GbaNetplayMatch::Step::Forward) pacer.forwarded(now);
        if (match.phase()==GbaNetplayMatch::Phase::Failed)
            throw std::runtime_error(match.error());
        if (match.checkpoint_pending() && !boot.finish_tick) closing=true;
        if (match.phase()==GbaNetplayMatch::Phase::CheckpointReady) {
            std::string error;
            if (!match.store_checkpoint(checkpoint_absolute,&error))
                throw std::runtime_error(error);
            std::printf("netplay agreed tick=%u bytes=%zu machine=%u\n",match.checkpoint().next_tick,
                match.checkpoint().state.size(),launch.seat_machine[launch.local_seat]);
            std::printf("netplay saved pair=%s\n",checkpoint_absolute.string().c_str());
            const auto until=GbaNetplayPacer::Clock::now()+std::chrono::seconds(1);
            while (GbaNetplayPacer::Clock::now()<until) {
                if (launch.pump_lobby) launch.pump_lobby();
                window.pump(); match.poll(false); pacer.idle();
            }
            return 0;
        }
        const auto connection=match.connection();
        const auto status=connection.phase==GbaConnectionPhase::Reconnecting ?
            "Reconnecting ("+std::to_string((connection.grace_remaining_ms+999)/1000)+"s)" :
            match.phase()==GbaNetplayMatch::Phase::Starting ? std::string("Connecting") :
            (closing || (match.checkpoint_pending() && !boot.finish_tick)) ?
                std::string("Saving and leaving - close again to discard") :
            "Link cable - Player "+std::to_string(launch.seat_machine[launch.local_seat]+1)+" - Shift+F1 save and leave";
        const auto title=boot.title+" - "+status;
        if (title!=previous_title) { window.set_title(title.c_str()); previous_title=title; }
        if (match.take_output(output)) {
            window.present(output.rgb888.data());
            window.push_audio_samples(output.audio.data(),output.audio.size());
        }
        if (step==GbaNetplayMatch::Step::Idle) pacer.idle();
    }
}
}
std::shared_ptr<GbaNetplayLaunch> make_gba_netplay_launch(std::string program,std::string build,
    std::string content,void (*setup)(GbaInstance&)) {
    auto launch=std::make_shared<GbaNetplayLaunch>();
    launch->program_id=std::move(program); launch->build_identity=std::move(build);
    launch->content_sha256=std::move(content); launch->setup_instance=setup; launch->run=run;
    return launch;
}
}
