#include "multiplayer_launch.h"
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::exit(1); } } while (0)
int main() {
    using namespace gbarecomp;
    auto launch=std::make_shared<GbaNetplayLaunch>();
    launch->program_id="fixture"; launch->build_identity=std::string(64,'a'); launch->content_sha256=std::string(64,'b');
    CHECK(!launch->enabled);
    std::vector<std::string> args={"game","--rom","mine.gba","--netplay-bind","127.0.0.1:5000",
        "--netplay-peer","127.0.0.1:5001","--netplay-seat","1","--netplay-session","123",
        "--netplay-delay-sync"};
    parse_gba_netplay_arguments(args,*launch);
    CHECK((args==std::vector<std::string>{"game","--rom","mine.gba"}));
    CHECK(launch->enabled && !launch->rollback && launch->local_seat==1 && launch->session_id==123);
    launch->seat_machine={1,0}; validate_gba_netplay_launch(*launch);
    const auto rejects=[](const auto& launch) {
        try { validate_gba_netplay_launch(launch); } catch (const std::invalid_argument&) { return true; }
        return false;
    };
    auto invalid=*launch; invalid.seat_machine={1,1}; CHECK(rejects(invalid));
    invalid=*launch; invalid.seat_machine={0,2}; CHECK(rejects(invalid));
    invalid=*launch; invalid.build_identity="friendly-v1"; CHECK(rejects(invalid));
    invalid=*launch; invalid.session_id=0; CHECK(rejects(invalid));
    invalid=*launch; invalid.input_delay=21; CHECK(rejects(invalid));
    invalid=*launch; invalid.prediction=17; CHECK(rejects(invalid));
    invalid=*launch; invalid.medium=GbaLinkMedium::Wireless; CHECK(rejects(invalid));
    invalid.supported_media=3; validate_gba_netplay_launch(invalid);
    invalid.medium=static_cast<GbaLinkMedium>(31); CHECK(rejects(invalid));
    invalid=*launch; invalid.peer_endpoint.clear(); CHECK(rejects(invalid));
    invalid.local_seat=0; validate_gba_netplay_launch(invalid); // passive LAN host
    launch->enabled=false;
    launch->supported_media=3;
    args={"game","--netplay-device","wireless"};
    parse_gba_netplay_arguments(args,*launch);
    CHECK(!launch->enabled && launch->medium==GbaLinkMedium::Wireless);
    CHECK(args==std::vector<std::string>{"game"});
    args={"game","--netplay-resume","previous.paired","--netplay-checkpoint","next.paired"};
    parse_gba_netplay_arguments(args,*launch);
    CHECK(!launch->enabled && args==std::vector<std::string>{"game"});
    CHECK(launch->resume_path=="previous.paired" && launch->checkpoint_path=="next.paired");
    for (const auto& tail: {std::vector<std::string>{"--netplay-seat","-1"},
                          std::vector<std::string>{"--netplay-peer","peer"},
                          std::vector<std::string>{"--netplay-unknown","value"}}) {
        args={"game"}; args.insert(args.end(),tail.begin(),tail.end());
        const auto original=args; bool refused=false;
        try { parse_gba_netplay_arguments(args,*launch); } catch (const std::invalid_argument&) { refused=true; }
        CHECK(refused && !launch->enabled && args==original);
    }
    // Two-seat hosts may listen passively on the command line too.
    {
        auto host=std::make_shared<GbaNetplayLaunch>(*launch); host->enabled=false; host->supported_media=1;
        host->medium=GbaLinkMedium::Cable; host->peer_endpoint.clear();
        args={"game","--netplay-bind","0.0.0.0:5000","--netplay-seat","0","--netplay-session","9"};
        parse_gba_netplay_arguments(args,*host);
        CHECK(host->enabled && host->peer_endpoint.empty() && gba_netplay_transport(*host)==GbaNetplayTransport::Pair);
        CHECK(gba_netplay_seat_identity(*host)=="1:0"); // unchanged two-seat identity
    }
    // Three and four consoles: a game capability, then star or relay transport.
    {
        GbaNetplayLaunch four; four.program_id="kirby"; four.build_identity=std::string(64,'a');
        four.content_sha256=std::string(64,'b');
        args={"game","--netplay-players","4","--netplay-bind","0.0.0.0:5000","--netplay-seat","0","--netplay-session","7"};
        auto copy=args; bool capped=false;
        try { parse_gba_netplay_arguments(copy,four); } catch (const std::invalid_argument&) { capped=true; }
        CHECK(capped && !four.enabled); // max_players defaults to two
        four.max_players=4;
        parse_gba_netplay_arguments(args,four);
        CHECK(four.enabled && four.player_count==4 && gba_netplay_transport(four)==GbaNetplayTransport::Hub);
        CHECK(gba_netplay_seat_identity(four)=="0:1:2:3");
        auto guest=four; guest.local_seat=2; CHECK(rejects(guest)); // a star guest must dial the hub
        guest.peer_endpoint="192.168.1.2:5000"; validate_gba_netplay_launch(guest);
        CHECK(gba_netplay_transport(guest)==GbaNetplayTransport::HubGuest);
        auto hub=four; hub.peer_endpoint="192.168.1.3:5000"; CHECK(rejects(hub)); // a hub has no peer
        auto relay=four; relay.force_input_relay=true; CHECK(rejects(relay)); // relay needs its endpoint
        relay.peer_endpoint="relay.example:9000"; validate_gba_netplay_launch(relay);
        CHECK(gba_netplay_transport(relay)==GbaNetplayTransport::Relay);
        auto two=relay; two.player_count=2; CHECK(gba_netplay_transport(two)==GbaNetplayTransport::Relay);
        auto bad=four; bad.local_seat=4; CHECK(rejects(bad));
        bad=four; bad.player_count=5; CHECK(rejects(bad));
        bad=four; bad.player_count=1; CHECK(rejects(bad));
        bad=four; bad.max_players=5; CHECK(rejects(bad));
        bad=four; bad.seat_machine={0,1,2,2}; CHECK(rejects(bad));
        bad=four; bad.seat_machine={0,1,2,4}; CHECK(rejects(bad));
        bad=four; bad.supported_media=3; bad.medium=GbaLinkMedium::Wireless; CHECK(rejects(bad));
        auto three=four; three.player_count=3; three.seat_machine={2,0,1,3}; // port 3 unused
        validate_gba_netplay_launch(three);
        CHECK(gba_netplay_seat_identity(three)=="2:0:1");
        three.seat_machine={0,1,3,2}; CHECK(rejects(three));
        // Port lists, relay flag, and a guest dialing the relay.
        GbaNetplayLaunch parsed=four; parsed.enabled=false;
        args={"game","--netplay-players","3","--netplay-ports","2,0,1","--netplay-relay","--netplay-bind","0.0.0.0:0",
              "--netplay-peer","203.0.113.5:7000","--netplay-seat","1","--netplay-session","8"};
        parse_gba_netplay_arguments(args,parsed);
        CHECK(args==std::vector<std::string>{"game"} && parsed.force_input_relay && parsed.player_count==3);
        CHECK(parsed.seat_machine[0]==2 && parsed.seat_machine[1]==0 && parsed.seat_machine[2]==1);
        for (const auto& ports: {"2,0","2,0,1,3","2,,1","2,0,x","",","}) {
            GbaNetplayLaunch fresh=four; fresh.enabled=false;
            args={"game","--netplay-players","3","--netplay-ports",ports,"--netplay-bind","0.0.0.0:0",
                  "--netplay-seat","0","--netplay-session","8"};
            const auto original=args; bool refused=false;
            try { parse_gba_netplay_arguments(args,fresh); } catch (const std::invalid_argument&) { refused=true; }
            CHECK(refused && !fresh.enabled && args==original);
        }
    }
    std::puts("netplay launch policy tests passed");
}
