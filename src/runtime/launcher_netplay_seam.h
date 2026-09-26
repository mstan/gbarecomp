#pragma once
// Included only by the shared launcher adapter when its backend is linked.
#include "multiplayer_launch.h"
#include "recomp_netplay_host.h"
#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace gbarecomp_seam {
struct NetplayBackend {
    std::shared_ptr<gbarecomp::GbaNetplayLaunch> launch;
    std::string game,version,content,directory;
    ~NetplayBackend() { if (launch) recomp_netplay_host_shutdown(); }
};
inline NetplayBackend& netplay_backend() { static NetplayBackend state; return state; }

inline void configure_netplay(RecompLauncherCGameInfo& gi,const gbarecomp::RunOptions& opts,
                              const std::string& directory) {
    if (!opts.netplay) return;
    auto& state=netplay_backend();
    if (state.launch && state.launch!=opts.netplay) recomp_netplay_host_shutdown();
    if (state.launch!=opts.netplay) {
        state.launch=opts.netplay; state.game=opts.netplay->program_id;
        // Lobby version fields hold 31 characters plus NUL. The match startup
        // separately verifies the complete source digest before guest execution.
        state.version=opts.netplay->build_identity.substr(0,31); state.content=opts.netplay->content_sha256;
        state.directory=directory;
        RecompNetplayHostHooks hooks{};
        hooks.game_name=state.game.c_str(); hooks.game_version=state.version.c_str();
        hooks.content_fingerprint=state.content.c_str(); hooks.platform="gba";
        hooks.default_lobby_name="GBA Link Cable"; hooks.max_players=2;
        hooks.slot_policy=RECOMP_NETPLAY_SLOTS_HOST_FIRST;
        hooks.input_player=RECOMP_NETPLAY_INPUT_AUTO; hooks.ctx=&state;
        hooks.exe_dir_path=[](void* ctx,const char* leaf,char* out,std::size_t size) {
            const auto path=(std::filesystem::path(static_cast<NetplayBackend*>(ctx)->directory)/leaf).string();
            if (path.size()>=size) return 0;
            std::snprintf(out,size,"%s",path.c_str()); return 1;
        };
        hooks.fill_match_caps=[](void*,const RecompLauncherCSettings*,RNetLobbyMatchCaps* caps) {
            // Shared UI defaults to six frames; retain the host's explicit
            // delay choice while respecting the MVP's prediction window.
            caps->input_delay=std::clamp(caps->input_delay,2,20);
            caps->input_prediction=std::clamp(caps->input_prediction,6,16);
        };
        // No mod hooks: the shared backend publishes vanilla and refuses
        // required mod plans. The multiplayer runtime skips plugin activation.
        if (recomp_netplay_host_init(&hooks)) throw std::runtime_error("cannot initialize netplay lobby");
    }
    gi.netplay_supported=1; gi.netplay=recomp_netplay_host_callbacks();
}

inline void accept_netplay(const RecompLauncherCNetplayLaunch& selected,const gbarecomp::RunOptions& opts) {
    if (!opts.netplay) return;
    auto& launch=*opts.netplay;
    launch.enabled=selected.enabled!=0;
    if (!launch.enabled) return;
    if (selected.is_spectator || selected.host_spectates || selected.lobby_kind ||
        selected.local_slot<0 || selected.local_slot>1 || selected.max_slots!=2 ||
        (selected.player_count && selected.player_count!=2) ||
        (selected.occupied_mask && selected.occupied_mask!=3))
        throw std::invalid_argument("this GBA build supports two players over link cable");
    launch.local_seat=selected.local_slot; launch.session_id=selected.session_id;
    launch.bind_endpoint=selected.bind_hostport; launch.peer_endpoint=selected.peer_hostport;
    launch.input_delay=selected.input_delay; launch.prediction=std::clamp(selected.input_prediction,6,16);
    launch.rollback=selected.rollback!=0; launch.force_turn=selected.force_turn!=0;
    for (unsigned slot=0;slot<2;++slot)
        launch.seat_machine[slot]=selected.slot_port_valid ? selected.slot_port[slot] : slot;
    launch.pump_lobby=[] {
        if (auto* callbacks=recomp_netplay_host_callbacks(); callbacks && callbacks->pump)
            callbacks->pump(callbacks->ctx);
    };
    gbarecomp::validate_gba_netplay_launch(launch);
}
}
