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
    if (!l.enabled || l.local_seat>1 || !l.session_id || l.input_delay<2 || l.input_delay>20 ||
        l.prediction<6 || l.prediction>16 || l.seat_machine[0]>1 || l.seat_machine[1]>1 ||
        l.seat_machine[0]==l.seat_machine[1] || l.bind_endpoint.empty() ||
        (l.peer_endpoint.empty() && l.local_seat!=0) ||
        l.bind_endpoint.size()>63 || l.peer_endpoint.size()>63 || l.program_id.empty() || l.program_id.size()>128 ||
        !hex_digest(l.build_identity,64) || !hex_digest(l.content_sha256,64))
        throw std::invalid_argument("invalid two-player GBA multiplayer launch");
}
void parse_gba_netplay_arguments(std::vector<std::string>& args,GbaNetplayLaunch& launch) {
    auto staged=launch;
    std::vector<std::string> kept;
    unsigned fields=0;
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
        if (key=="--netplay-delay-sync") { staged.rollback=false; continue; }
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
        else throw std::invalid_argument("unknown option "+key);
    }
    if (staged.enabled) {
        if (fields!=15) throw std::invalid_argument("direct netplay requires bind, peer, seat and session");
        validate_gba_netplay_launch(staged);
    }
    launch=std::move(staged); args=std::move(kept);
}
}
