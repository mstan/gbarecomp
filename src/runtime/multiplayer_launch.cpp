#include "multiplayer_launch.h"
#include <algorithm>
#include <charconv>
#include <stdexcept>

namespace gbarecomp {
namespace {
bool hex_digest(const std::string& s,std::size_t size) {
    return s.size()==size && std::all_of(s.begin(),s.end(),[](char c) {
        return (c>='0' && c<='9') || (c>='a' && c<='f');
    });
}
}
void validate_gba_netplay_launch(const GbaNetplayLaunch& l) {
    validate_gba_netplay_view(l.view, l.view_policy);
    if ((l.medium!=GbaLinkMedium::Cable && l.medium!=GbaLinkMedium::Wireless) ||
        !(l.supported_media&(1u<<static_cast<unsigned>(l.medium))))
        throw std::invalid_argument("game does not support the selected serial device");
    if (l.max_players<2 || l.max_players>kGbaMaxSessionPlayers)
        throw std::invalid_argument("game multiplayer capability must be two to four consoles");
    if (l.player_count<2 || l.player_count>l.max_players)
        throw std::invalid_argument("this game links "+std::string(l.max_players==2 ? "two" :
            l.max_players==3 ? "two or three" : "two to four")+" consoles");
    if (l.medium==GbaLinkMedium::Wireless && l.player_count!=2)
        throw std::invalid_argument("wireless adapter sessions are qualified for two consoles only");
    unsigned ports=0;
    for (unsigned seat=0;seat<l.player_count;++seat) {
        const auto port=l.seat_machine[seat];
        if (port>=l.player_count || (ports&(1u<<port)))
            throw std::invalid_argument("netplay seats must map to distinct cable ports");
        ports|=1u<<port;
    }
    // A pair host may listen passively; a star hub never has a peer; every
    // relayed seat and every guest dials an explicit endpoint.
    const auto transport=gba_netplay_transport(l);
    const bool peer_optional=transport==GbaNetplayTransport::Hub ||
        (transport==GbaNetplayTransport::Pair && l.local_seat==0);
    if (!l.enabled || l.local_seat>=l.player_count || !l.session_id || l.input_delay<2 || l.input_delay>20 ||
        l.prediction<6 || l.prediction>16 || l.bind_endpoint.empty() ||
        (l.peer_endpoint.empty() && !peer_optional) ||
        (transport==GbaNetplayTransport::Hub && !l.peer_endpoint.empty()) ||
        l.bind_endpoint.size()>63 || l.peer_endpoint.size()>63 || l.program_id.empty() || l.program_id.size()>128 ||
        !hex_digest(l.build_identity,64) || !hex_digest(l.content_sha256,64))
        throw std::invalid_argument("invalid GBA multiplayer launch");
}
std::string gba_netplay_seat_identity(const GbaNetplayLaunch& launch) {
    // Two seats keep the original "a:b" spelling so existing paired
    // checkpoints still decode; larger cables append their extra ports.
    std::string identity;
    for (unsigned seat=0;seat<launch.player_count;++seat)
        identity+=(seat ? ":" : "")+std::to_string(launch.seat_machine[seat]);
    return identity;
}
bool gba_netplay_default_rollback(unsigned players) { return players<=2; }
GbaNetplayTransport gba_netplay_transport(const GbaNetplayLaunch& l) {
    if (l.force_input_relay) return GbaNetplayTransport::Relay;
    if (l.player_count<=2) return GbaNetplayTransport::Pair;
    return l.local_seat==0 ? GbaNetplayTransport::Hub : GbaNetplayTransport::HubGuest;
}
void parse_gba_netplay_arguments(std::vector<std::string>& args,GbaNetplayLaunch& launch) {
    auto staged=launch;
    std::vector<std::string> kept;
    unsigned fields=0, ports_given=0;
    bool mode_given=false;
    for (std::size_t i=0;i<args.size();++i) {
        const auto& key=args[i];
        if (i==0 || !key.starts_with("--netplay-")) { kept.push_back(key); continue; }
        if (key=="--netplay-device") {
            if (i+1==args.size()) throw std::invalid_argument("missing value for "+key);
            const auto& device=args[++i];
            if (device=="cable") staged.medium=GbaLinkMedium::Cable;
            else if (device=="wireless") staged.medium=GbaLinkMedium::Wireless;
            else throw std::invalid_argument("netplay device must be cable or wireless");
            if (!(staged.supported_media&(1u<<static_cast<unsigned>(staged.medium))))
                throw std::invalid_argument("game does not support the selected serial device");
            continue;
        }
        if (key=="--netplay-view") {
            if (i+1==args.size()) throw std::invalid_argument("missing value for "+key);
            staged.view=parse_gba_netplay_view(args[++i]);
            staged.view_explicit=true;
            validate_gba_netplay_view(staged.view, staged.view_policy);
            continue;
        }
        if (key=="--netplay-resume" || key=="--netplay-checkpoint") {
            if (i+1==args.size()) throw std::invalid_argument("missing value for "+key);
            (key=="--netplay-resume" ? staged.resume_path : staged.checkpoint_path)=args[++i];
            continue;
        }
        staged.enabled=true;
        if (key=="--netplay-delay-sync") { staged.rollback=false; mode_given=true; continue; }
        if (key=="--netplay-rollback") { staged.rollback=true; mode_given=true; continue; }
        if (key=="--netplay-relay") { staged.force_input_relay=true; continue; }
        if (i+1==args.size()) throw std::invalid_argument("missing value for "+key);
        const auto& value=args[++i];
        const auto number=[&]() {
            unsigned n=0; const auto parsed=std::from_chars(value.data(),value.data()+value.size(),n);
            if (parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size())
                throw std::invalid_argument("invalid value for "+key);
            return n;
        };
        if (key=="--netplay-bind") { staged.bind_endpoint=value; fields|=1; }
        else if (key=="--netplay-peer") { staged.peer_endpoint=value; fields|=2; }
        else if (key=="--netplay-seat") { staged.local_seat=number(); fields|=4; }
        else if (key=="--netplay-session") { staged.session_id=number(); fields|=8; }
        else if (key=="--netplay-delay") staged.input_delay=number();
        else if (key=="--netplay-players") staged.player_count=number();
        else if (key=="--netplay-ports") {
            // Comma-separated cable port per seat, e.g. "1,0,2" (default identity).
            unsigned seat=0; std::size_t begin=0;
            while (begin<=value.size()) {
                const auto end=std::min(value.find(',',begin),value.size());
                unsigned port=0;
                const auto parsed=std::from_chars(value.data()+begin,value.data()+end,port);
                if (seat>=kGbaMaxSessionPlayers || end==begin || parsed.ec!=std::errc{} ||
                    parsed.ptr!=value.data()+end)
                    throw std::invalid_argument("invalid value for "+key);
                staged.seat_machine[seat++]=port;
                begin=end+1;
            }
            fields|=16;
            ports_given=seat;
        }
        else throw std::invalid_argument("unknown option "+key);
    }
    if (staged.enabled) {
        // Both modes run at every cable size; unless one is named, two seats
        // default to rollback and three or four to delay-sync.
        if (!mode_given) staged.rollback=gba_netplay_default_rollback(staged.player_count);
        // A pair host and a star hub may listen without a peer; transport
        // validation below decides which seats must name one.
        if ((fields&13)!=13) throw std::invalid_argument("direct netplay requires bind, seat and session");
        if ((fields&16) && ports_given!=staged.player_count)
            throw std::invalid_argument("--netplay-ports must name one port per player");
        validate_gba_netplay_launch(staged);
    }
    launch=std::move(staged); args=std::move(kept);
}
}
