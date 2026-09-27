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
    std::puts("shared launcher seat/policy bridge tests passed");
}
