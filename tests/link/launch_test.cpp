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
    invalid=*launch; invalid.peer_endpoint.clear(); CHECK(rejects(invalid));
    invalid.local_seat=0; validate_gba_netplay_launch(invalid); // passive LAN host
    launch->enabled=false;
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
    std::puts("netplay launch policy tests passed");
}
