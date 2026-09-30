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
    std::vector<const char*> view_labels;
#if defined(RECOMP_LAUNCHER_HAS_SESSION_VARIANT)
    std::vector<RecompNetplaySessionVariant> variants;
#endif
    ~NetplayBackend() { if (launch) recomp_netplay_host_shutdown(); }
};
inline NetplayBackend& netplay_backend() { static NetplayBackend state; return state; }

inline int netplay_view_index(gbarecomp::GbaNetplayView view,const gbarecomp::GbaNetplayViewPolicy& policy) {
    const auto views=gbarecomp::gba_netplay_available_views(policy);
    const auto found=std::find(views.begin(),views.end(),view);
    return found==views.end() ? 0 : static_cast<int>(found-views.begin());
}
inline gbarecomp::GbaNetplayView netplay_view_at(int index,const gbarecomp::GbaNetplayViewPolicy& policy) {
    const auto views=gbarecomp::gba_netplay_available_views(policy);
    return index>=0 && index<static_cast<int>(views.size()) ? views[index] : gbarecomp::GbaNetplayView::Native;
}

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
        hooks.default_lobby_name="GBA Multiplayer"; hooks.max_players=2;
#if defined(RECOMP_NETPLAY_HOST_HAS_NETPLAY_MAX_PLAYERS)
        // This title's online seats: the game's cable capability. max_players
        // stays the engine-wide ceiling; LAN/direct rooms stay two seats.
        hooks.netplay_max_players=static_cast<int>(opts.netplay->max_players);
#endif
#if defined(RECOMP_NETPLAY_HOST_HAS_DELAY_SYNC_FROM_PLAYERS)
        // Three or four cable consoles start in delay-sync; two keep rollback.
        hooks.netplay_delay_sync_from_players=opts.netplay->max_players>2 ? 3 : 0;
#endif
#if defined(RECOMP_LAUNCHER_HAS_SESSION_VARIANT)
        state.variants.clear();
        if (opts.netplay->supported_media & (1u<<static_cast<unsigned>(gbarecomp::GbaLinkMedium::Cable)))
            state.variants.push_back({0,"Link Cable"});
        if (opts.netplay->supported_media & (1u<<static_cast<unsigned>(gbarecomp::GbaLinkMedium::Wireless)))
            state.variants.push_back({1,"Wireless Adapter"});
        hooks.session_variants=state.variants.data();
        hooks.session_variant_count=static_cast<int>(state.variants.size());
        hooks.default_session_variant=static_cast<int>(opts.netplay->medium);
#else
        if (opts.netplay->supported_media!=1 || opts.netplay->medium!=gbarecomp::GbaLinkMedium::Cable)
            throw std::runtime_error("wireless lobby selection requires an updated recomp-ui");
#endif
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
#if defined(RECOMP_LAUNCHER_HAS_NETPLAY_VIEW)
    static const char* const views[]={"Native (3:2)","16:9","21:9","32:9","Adaptive"};
    if (opts.netplay->view_policy.supported) {
        state.view_labels.clear();
        for (auto view:gbarecomp::gba_netplay_available_views(opts.netplay->view_policy))
            state.view_labels.push_back(views[static_cast<unsigned>(view)]);
        gi.netplay_view_labels=state.view_labels.data();
        gi.num_netplay_view_labels=static_cast<int>(state.view_labels.size());
    }
#endif
}

// HOST_FIRST session slots arrive dense (host = slot 0, then ascending lobby
// seat); slot_port[] names each slot's lobby seat, which may be sparse (P1,
// P3, P4 in a four-seat room). Cable ports are those seats' ranks, 0..N-1:
// recomp-ui's shared dense-position rule where available, else the same rank.
inline void compact_netplay_ports(const RecompLauncherCNetplayLaunch& selected,unsigned players,
                                  gbarecomp::GbaNetplayLaunch& launch) {
#if defined(RECOMP_LAUNCHER_HAS_NETPLAY_DENSE_POSITION)
    for (unsigned slot=0;slot<players;++slot) {
        const int port=recomp_launcher_netplay_dense_position(&selected,static_cast<int>(slot));
        if (port<0) throw std::invalid_argument("netplay seat has no lobby player");
        launch.seat_machine[slot]=static_cast<unsigned>(port); // duplicates refused by validation
    }
#else
    if (!selected.slot_port_valid) {
        for (unsigned slot=0;slot<players;++slot) launch.seat_machine[slot]=slot;
        return;
    }
    constexpr int seats=static_cast<int>(sizeof(selected.slot_port)/sizeof(selected.slot_port[0]));
    for (unsigned slot=0;slot<players;++slot) {
        const int seat=selected.slot_port[slot];
        if (seat<0 || seat>=seats) throw std::invalid_argument("netplay seat has no lobby player");
        unsigned rank=0;
        for (unsigned other=0;other<players;++other) {
            if (other==slot) continue;
            if (selected.slot_port[other]==seat) throw std::invalid_argument("two netplay seats share a lobby player");
            if (selected.slot_port[other]<seat) ++rank;
        }
        launch.seat_machine[slot]=rank;
    }
#endif
}
inline void accept_netplay(const RecompLauncherCNetplayLaunch& selected,const gbarecomp::RunOptions& opts) {
    if (!opts.netplay) return;
    auto& launch=*opts.netplay;
    launch.enabled=selected.enabled!=0;
    if (!launch.enabled) return;
    const int capacity=static_cast<int>(launch.max_players);
    const int players=selected.player_count ? selected.player_count : selected.max_slots;
    const std::uint32_t dense=players>=2 && players<=31 ? (1u<<players)-1 : 0;
    if (selected.is_spectator || selected.host_spectates || selected.lobby_kind ||
        selected.max_slots<2 || selected.max_slots>capacity || players<2 || players>selected.max_slots ||
        selected.local_slot<0 || selected.local_slot>=players ||
        (selected.occupied_mask && selected.occupied_mask!=dense))
        throw std::invalid_argument("this GBA build links "+std::to_string(capacity)+
            " or fewer consoles, one player per console");
#if defined(RECOMP_LAUNCHER_HAS_SESSION_VARIANT)
    if (selected.session_variant<0 || selected.session_variant>1)
        throw std::invalid_argument("unknown GBA connection type");
    launch.medium=static_cast<gbarecomp::GbaLinkMedium>(selected.session_variant);
#else
    launch.medium=gbarecomp::GbaLinkMedium::Cable;
#endif
    launch.player_count=static_cast<unsigned>(players);
    launch.local_seat=selected.local_slot; launch.session_id=selected.session_id;
    launch.bind_endpoint=selected.bind_hostport; launch.peer_endpoint=selected.peer_hostport;
    launch.input_delay=selected.input_delay; launch.prediction=std::clamp(selected.input_prediction,6,16);
    launch.rollback=selected.rollback!=0; launch.force_turn=selected.force_turn!=0;
    // The launch's own transport statement: online rooms dial the lobby UDP
    // relay; its absence with 3+ seats is a host-relayed LAN star.
    launch.force_input_relay=selected.force_input_relay!=0;
    compact_netplay_ports(selected,launch.player_count,launch);
    launch.pump_lobby=[] {
        if (auto* callbacks=recomp_netplay_host_callbacks(); callbacks && callbacks->pump)
            callbacks->pump(callbacks->ctx);
    };
    gbarecomp::validate_gba_netplay_launch(launch);
}
}
