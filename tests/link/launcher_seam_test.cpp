#include "runtime.h"
#include "launcher_netplay_seam.h"
#include <cstdlib>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::exit(1); } } while (0)
int main() {
    gbarecomp::RunOptions opts;
    opts.netplay=std::make_shared<gbarecomp::GbaNetplayLaunch>();
    opts.netplay->program_id="fixture"; opts.netplay->build_identity=std::string(64,'a');
    opts.netplay->content_sha256=std::string(64,'b');
    RecompLauncherCNetplayLaunch selected{};
    selected.enabled=1; selected.local_slot=0; selected.session_id=123;
    selected.max_slots=selected.player_count=2; selected.occupied_mask=3;
    selected.input_delay=6; selected.input_prediction=10; selected.rollback=1;
    selected.slot_port_valid=1; selected.slot_port[0]=1; selected.slot_port[1]=0;
    std::snprintf(selected.bind_hostport,sizeof(selected.bind_hostport),"127.0.0.1:5000");
    std::snprintf(selected.peer_hostport,sizeof(selected.peer_hostport),"127.0.0.1:5001");
    gbarecomp_seam::accept_netplay(selected,opts);
    CHECK(opts.netplay->enabled && opts.netplay->local_seat==0 && opts.netplay->seat_machine[0]==1);
    CHECK(opts.netplay->seat_machine[1]==0 && opts.netplay->prediction==10);
    const auto refused=[&](auto bad) {
        try { gbarecomp_seam::accept_netplay(bad,opts); } catch (const std::invalid_argument&) { return true; }
        return false;
    };
    auto bad=selected; bad.is_spectator=1; CHECK(refused(bad));
    bad=selected; bad.host_spectates=1; CHECK(refused(bad));
    bad=selected; bad.slot_port[1]=1; CHECK(refused(bad));
    bad=selected; bad.slot_port[0]=-1; CHECK(refused(bad));
    bad=selected; bad.player_count=1; CHECK(refused(bad));
    bad=selected; bad.occupied_mask=1; CHECK(refused(bad));
    bad=selected; bad.max_slots=4; CHECK(refused(bad));
#if defined(RECOMP_LAUNCHER_HAS_SESSION_VARIANT)
    bad=selected; bad.session_variant=1; CHECK(refused(bad));
    opts.netplay->supported_media=3;
    selected.session_variant=1;
    gbarecomp_seam::accept_netplay(selected,opts);
    CHECK(opts.netplay->medium==gbarecomp::GbaLinkMedium::Wireless);
    bad=selected; bad.session_variant=2; CHECK(refused(bad));
    selected.session_variant=0;
#endif
    selected.slot_port_valid=0; selected.local_slot=1; selected.rollback=0;
    gbarecomp_seam::accept_netplay(selected,opts);
    CHECK(opts.netplay->local_seat==1 && opts.netplay->seat_machine[1]==1 && !opts.netplay->rollback);
    // Exact shared backend LAN contract: host listens without a fixed peer;
    // guest binds an ephemeral port and supplies the host endpoint. The host
    // may drive cable port one while remaining transport/session slot zero.
    selected.local_slot=0; selected.slot_port_valid=1;
    selected.slot_port[0]=1; selected.slot_port[1]=0;
    selected.input_prediction=0; selected.peer_hostport[0]=0;
    std::snprintf(selected.bind_hostport,sizeof(selected.bind_hostport),"0.0.0.0:5010");
    gbarecomp_seam::accept_netplay(selected,opts);
    CHECK(opts.netplay->peer_endpoint.empty() && opts.netplay->local_seat==0 && opts.netplay->seat_machine[0]==1);
    bad=selected; bad.local_slot=1; CHECK(refused(bad));
    selected.local_slot=1;
    std::snprintf(selected.bind_hostport,sizeof(selected.bind_hostport),"0.0.0.0:0");
    std::snprintf(selected.peer_hostport,sizeof(selected.peer_hostport),"127.0.0.1:5010");
    gbarecomp_seam::accept_netplay(selected,opts);
    CHECK(opts.netplay->bind_endpoint=="0.0.0.0:0" && opts.netplay->prediction==6 && !opts.netplay->rollback);
    selected.enabled=0; gbarecomp_seam::accept_netplay(selected,opts); CHECK(!opts.netplay->enabled);
    // A four-console game in an online relayed room. Session slots are dense
    // (host first), lobby seats sparse: seats {0,2,3} -> cable ports {0,1,2}.
    opts.netplay->max_players=4;
    RecompLauncherCNetplayLaunch room{};
    room.enabled=1; room.session_id=77; room.max_slots=4; room.player_count=3; room.occupied_mask=7;
    room.input_delay=6; room.input_prediction=10; room.rollback=1; room.force_input_relay=1;
    room.slot_port_valid=1; room.slot_port[0]=2; room.slot_port[1]=0; room.slot_port[2]=3;
    std::snprintf(room.bind_hostport,sizeof(room.bind_hostport),"0.0.0.0:0");
    std::snprintf(room.peer_hostport,sizeof(room.peer_hostport),"198.51.100.4:9000");
    for (int slot=0;slot<3;++slot) {
        room.local_slot=slot;
        gbarecomp_seam::accept_netplay(room,opts);
        const auto& l=*opts.netplay;
        CHECK(l.enabled && l.player_count==3 && l.local_seat==unsigned(slot) && l.force_input_relay);
        CHECK(l.seat_machine[0]==1 && l.seat_machine[1]==0 && l.seat_machine[2]==2);
        CHECK(gbarecomp::gba_netplay_transport(l)==gbarecomp::GbaNetplayTransport::Relay);
    }
    room.player_count=4; room.occupied_mask=15; room.slot_port[3]=1;
    gbarecomp_seam::accept_netplay(room,opts);
    CHECK(opts.netplay->player_count==4 && opts.netplay->seat_machine[3]==1);
    // Two players in a four-seat room keep the two-console session.
    room.player_count=2; room.occupied_mask=3; room.local_slot=1;
    gbarecomp_seam::accept_netplay(room,opts);
    CHECK(opts.netplay->player_count==2 && opts.netplay->seat_machine[0]==1 && opts.netplay->seat_machine[1]==0);
    room.player_count=4; room.occupied_mask=15;
    auto wrong=room; wrong.occupied_mask=0b1011; CHECK(refused(wrong)); // SEAT-policy sparse slots
    wrong=room; wrong.local_slot=4; CHECK(refused(wrong));
    wrong=room; wrong.slot_port[3]=2; CHECK(refused(wrong));
    wrong=room; wrong.slot_port[2]=-1; CHECK(refused(wrong));
    wrong=room; wrong.max_slots=5; wrong.player_count=5; wrong.occupied_mask=31; CHECK(refused(wrong));
    wrong=room; wrong.host_spectates=1; CHECK(refused(wrong));
    wrong=room; wrong.force_input_relay=0; wrong.local_slot=0; wrong.peer_hostport[0]=0;
    gbarecomp_seam::accept_netplay(wrong,opts); // LAN star host (direct-IP launches only today)
    CHECK(gbarecomp::gba_netplay_transport(*opts.netplay)==gbarecomp::GbaNetplayTransport::Hub);
    opts.netplay->max_players=3;
    CHECK(refused(room)); // four seats exceed a three-console title
    room.max_slots=3; room.player_count=3; room.occupied_mask=7; gbarecomp_seam::accept_netplay(room,opts);
    CHECK(opts.netplay->player_count==3);
    std::puts("shared launcher seat/policy bridge tests passed");
}
