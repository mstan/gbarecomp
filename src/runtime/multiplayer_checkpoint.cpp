#include "multiplayer_checkpoint.h"
#include "multiplayer_startup.h"
#include "simulation_archive.h"
#include "sha1.h"
#include <algorithm>
#include <stdexcept>

namespace gbarecomp {
namespace {
constexpr std::uint32_t kProposal=0x50434247,kArchive=0x41434247,kVersion=1,kMaxState=8*1024*1024;
std::vector<std::uint8_t> proposal(const std::string& identity,const GbaConfirmedCheckpoint& c) {
    gba::SimulationArchive<false> a;
    a.identity(kProposal); a.identity(kVersion); a.text_identity(identity);
    a.identity(c.next_tick); a.identity(gba::sha1(c.state.data(),c.state.size()).bytes);
    return a.take();
}
std::uint32_t boundary(std::span<const std::uint8_t> data,const std::string& identity) {
    gba::SimulationArchive<true> a(data);
    a.identity(kProposal); a.identity(kVersion); a.text_identity(identity);
    std::uint32_t tick=0; std::array<std::uint8_t,20> hash{}; a(tick,hash);
    if (a.remaining()) throw std::invalid_argument("trailing checkpoint proposal data");
    return tick;
}
// Zero-size SAVE probes coordinate without stalling admission. Distinct from
// the ready hash (a reply echoes the probe it answers).
std::uint32_t arrival_token(std::uint32_t tick) { return (tick*2654435761u)^0x41525256u; }
std::uint32_t ready_hash(const std::vector<std::uint8_t>& bytes) {
    const auto h=gba::sha1(bytes.data(),bytes.size()).bytes;
    return std::uint32_t(h[0]) | (std::uint32_t(h[1])<<8) |
        (std::uint32_t(h[2])<<16) | (std::uint32_t(h[3])<<24);
}
}
GbaNetplayCheckpointAgreement::GbaNetplayCheckpointAgreement(const GbaNetplayHost& host,
    RNetSession* network,unsigned seat,std::string identity,std::uint32_t through,std::uint32_t tick,bool wait)
    : host_(host),network_(network),seat_(seat),seats_(static_cast<unsigned>(host.seat_count())),
      identity_(std::move(identity)),through_(through),
      proposed_tick_(tick),wait_for_confirmation_(wait),phase_(seat ? Phase::ReceiveProposal : Phase::SendArrival) {
    if (!network || seats_<2 || seats_>kGbaMaxSessionPlayers || seat>=seats_ ||
        rnet_session_local_slot(network)!=static_cast<int>(seat) ||
        identity_.empty() || identity_.size()>1024 || (wait && !tick))
        throw std::invalid_argument("invalid checkpoint agreement configuration");
    if (!seat_ && !wait) {
        if (!host_.copy_checkpoint(tick,through_,checkpoint_))
            throw std::invalid_argument("proposed checkpoint is unavailable or unconfirmed");
        proposal_=proposal(identity_,checkpoint_); ready_hash_=ready_hash(proposal_);
    }
}
GbaNetplayCheckpointAgreement::Status GbaNetplayCheckpointAgreement::poll(std::uint64_t now) {
    if (phase_==Phase::Done) return Status::Ready;
    if (phase_==Phase::Failed) return Status::Failed;
    if (!started_) { started_=true; started_ms_=now; }
    const std::uint32_t guests=((1u<<seats_)-1)&~1u;
    try {
        if (now<started_ms_ || now-started_ms_>=60000) {
            std::uint32_t expect=0,replied=0,matched=0;
            const auto waiting=!seat_ && phase_==Phase::SendProposal ? guests&~receipts_ :
                phase_==Phase::SendReady && rnet_session_state_probe_replies(network_,&expect,&replied,&matched) ?
                    expect&~replied : 0u;
            throw std::runtime_error("checkpoint agreement timed out"+
                (waiting ? " waiting for "+gba_netplay_seat_names(waiting) : std::string()));
        }
        rnet_session_pump(network_);
        if (const auto gone=rnet_session_peer_gone_mask(network_)&((1u<<seats_)-1)&~(1u<<seat_))
            throw std::runtime_error(gba_netplay_seat_names(gone)+" left before checkpoint agreement");
        if (!rnet_session_is_running(network_)) return Status::Waiting;
        if (wait_for_confirmation_ && proposal_.empty()) {
            if (host_.replaying() || through_==UINT32_MAX || through_==0 ||
                through_<proposed_tick_-1 || host_.next_tick()<proposed_tick_)
                return Status::Waiting;
            if (!host_.copy_checkpoint(proposed_tick_,through_,checkpoint_))
                throw std::runtime_error("checkpoint aged out before bilateral confirmation");
            proposal_=proposal(identity_,checkpoint_); ready_hash_=ready_hash(proposal_);
        }
        int from=-1; rnet_u8 op=0,slot=0; const void* data=nullptr; std::size_t size=0;
        const bool ready=rnet_session_state_take_ready_from(network_,&from,&op,&slot,&data,&size)!=0;
        const auto bytes=std::span(static_cast<const std::uint8_t*>(data),ready ? size : 0);
        switch (phase_) {
        case Phase::SendArrival:
            // Every seat answers only once it holds its own candidate at the
            // boundary (the confirmation gate above); nobody is left behind it.
            if (!sending_) {
                if (rnet_session_state_probe(network_,RNET_STATE_OP_SAVE,0,0,arrival_token(checkpoint_.next_tick))==0)
                    sending_=true;
            } else {
                int match=0;
                if (rnet_session_state_probe_take_reply(network_,&match)) {
                    if (!match) throw std::runtime_error("a peer did not reach the checkpoint boundary");
                    rnet_session_state_probe_finish(network_); phase_=Phase::SendProposal; sending_=false;
                }
            }
            break;
        case Phase::SendProposal:
            // Host: one broadcast, then a receipt from every other seat. A
            // matching receipt is stronger than that seat's transport ACK, so
            // receipts may complete the phase before the proposal's own ACKs
            // (with one guest its receipt even supersedes the transfer).
            if (!sending_) {
                if (rnet_session_state_begin(network_,RNET_STATE_OP_SAVE,0,proposal_.data(),proposal_.size())==0)
                    sending_=true;
            } else if (ready) {
                if (from==0) {
                    if (op!=RNET_STATE_OP_SAVE || slot!=0) throw std::runtime_error("unexpected checkpoint transfer response");
                } else if (from<1 || from>=static_cast<int>(seats_) || op!=RNET_STATE_OP_MEMCARD || slot!=from ||
                           (receipts_&(1u<<from)) || size!=proposal_.size() ||
                           !std::equal(proposal_.begin(),proposal_.end(),bytes.begin())) {
                    throw std::runtime_error((from>=1 && from<static_cast<int>(seats_) ?
                        gba_netplay_seat_names(1u<<from) : std::string("a peer"))+" rejected the exact checkpoint");
                } else receipts_|=1u<<from;
                rnet_session_state_finish_from(network_,from,0);
            }
            if (sending_ && receipts_==guests) {
                rnet_session_state_finish_from(network_,0,0); // receipts prove delivery
                phase_=Phase::SendReady; sending_=false;
            }
            break;
        case Phase::ReceiveProposal:
            if (!arrived_) {
                rnet_u8 probe_op=0,probe_slot=0; rnet_u32 probe_bytes=0,probe_hash=0;
                if (!rnet_session_state_probe_pending(network_,&probe_op,&probe_slot,&probe_bytes,&probe_hash)) break;
                if (probe_op!=RNET_STATE_OP_SAVE || probe_slot!=0 || probe_bytes!=0)
                    throw std::runtime_error("unexpected checkpoint arrival barrier");
                // Every caller constructs the agreement with the agreed tick.
                const auto local=proposed_tick_;
                const bool here=probe_hash==arrival_token(local);
                if (rnet_session_state_probe_reply(network_,here)!=0 || !here)
                    throw std::runtime_error("peer chose a different checkpoint boundary");
                arrived_=true;
            }
            if (ready) {
                if (from!=0 || op!=RNET_STATE_OP_SAVE || slot!=0 || size>1100)
                    throw std::runtime_error("unexpected checkpoint proposal");
                const auto tick=boundary(bytes,identity_);
                if (wait_for_confirmation_ && tick!=proposed_tick_)
                    throw std::runtime_error("peer chose a different checkpoint boundary");
                if (!host_.copy_checkpoint(tick,through_,checkpoint_))
                    throw std::runtime_error("peer checkpoint is unavailable or unconfirmed locally");
                proposal_=proposal(identity_,checkpoint_);
                if (!std::equal(bytes.begin(),bytes.end(),proposal_.begin(),proposal_.end()))
                    throw std::runtime_error("checkpoint state digest mismatch");
                ready_hash_=ready_hash(proposal_);
                rnet_session_state_finish_from(network_,0,0);
                phase_=Phase::SendReceipt; sending_=false;
            }
            break;
        case Phase::SendReceipt: {
            rnet_u8 probe_op=0,probe_slot=0; rnet_u32 probe_bytes=0,probe_hash=0;
            if (!sending_) {
                if (rnet_session_state_begin(network_,RNET_STATE_OP_MEMCARD,static_cast<rnet_u8>(seat_),
                        proposal_.data(),proposal_.size())==0) sending_=true;
            } else if (ready) {
                if (from!=static_cast<int>(seat_) || op!=RNET_STATE_OP_MEMCARD || slot!=seat_)
                    throw std::runtime_error("unexpected checkpoint transfer response");
                rnet_session_state_finish_from(network_,from,0);
                phase_=Phase::ReceiveReady; sending_=false;
            } else if (rnet_session_state_probe_pending(network_,&probe_op,&probe_slot,&probe_bytes,&probe_hash)) {
                // The host probes only after holding every receipt, ours included.
                rnet_session_state_finish_from(network_,static_cast<int>(seat_),0);
                phase_=Phase::ReceiveReady; sending_=false;
            }
            if (phase_!=Phase::ReceiveReady) break;
            [[fallthrough]];
        }
        case Phase::ReceiveReady: {
            rnet_u32 probe_bytes=0,hash=0;
            if (rnet_session_state_probe_pending(network_,&op,&slot,&probe_bytes,&hash)) {
                const bool match=op==RNET_STATE_OP_SAVE && slot==0 && probe_bytes==0 && hash==ready_hash_;
                if (rnet_session_state_probe_reply(network_,match)!=0 || !match)
                    throw std::runtime_error("checkpoint ready barrier mismatch");
                phase_=Phase::Done;
            }
            break;
        }
        case Phase::SendReady:
            if (!sending_) {
                if (rnet_session_state_probe(network_,RNET_STATE_OP_SAVE,0,0,ready_hash_)==0) sending_=true;
            } else {
                int match=0;
                if (rnet_session_state_probe_take_reply(network_,&match)) {
                    if (!match) {
                        std::uint32_t expect=0,replied=0,matched=0;
                        rnet_session_state_probe_replies(network_,&expect,&replied,&matched);
                        throw std::runtime_error(gba_netplay_seat_names(expect&~matched)+" rejected the checkpoint ready barrier");
                    }
                    rnet_session_state_probe_finish(network_); phase_=Phase::Done;
                }
            }
            break;
        default: break;
        }
    } catch (const std::exception& e) { error_=e.what(); phase_=Phase::Failed; }
    return phase_==Phase::Done ? Status::Ready : phase_==Phase::Failed ? Status::Failed : Status::Waiting;
}
const GbaConfirmedCheckpoint& GbaNetplayCheckpointAgreement::checkpoint() const {
    if (phase_!=Phase::Done) throw std::logic_error("checkpoint is not agreed by both peers");
    return checkpoint_;
}
std::vector<std::uint8_t> GbaNetplayCheckpointAgreement::archive() const {
    const auto& c=checkpoint();
    gba::SimulationArchive<false> a;
    a.identity(kArchive); a.identity(kVersion); a.text_identity(identity_);
    a.identity(c.next_tick); a.identity(gba::sha1(c.state.data(),c.state.size()).bytes);
    auto state=c.state; a.vector(state,kMaxState);
    a.identity(gba::sha1(a.bytes().data(),a.bytes().size()).bytes);
    return a.take();
}
bool gba_decode_checkpoint_archive(std::span<const std::uint8_t> bytes,const std::string& identity,
                                   GbaConfirmedCheckpoint& out,std::string* error) {
    try {
        if (bytes.size()<20 || bytes.size()>kMaxState+1100)
            throw std::invalid_argument("invalid paired checkpoint archive size");
        const auto payload=bytes.first(bytes.size()-20);
        const auto digest=gba::sha1(payload.data(),payload.size()).bytes;
        if (!std::equal(digest.begin(),digest.end(),bytes.end()-20))
            throw std::invalid_argument("paired checkpoint archive checksum mismatch");
        gba::SimulationArchive<true> a(payload);
        a.identity(kArchive); a.identity(kVersion); a.text_identity(identity);
        GbaConfirmedCheckpoint c; std::array<std::uint8_t,20> hash{};
        a(c.next_tick,hash); a.vector(c.state,kMaxState);
        if (a.remaining() || c.state.empty() || gba::sha1(c.state.data(),c.state.size()).bytes!=hash)
            throw std::invalid_argument("invalid paired checkpoint archive");
        out=std::move(c); if (error) error->clear(); return true;
    } catch (const std::exception& e) { if (error) *error=e.what(); return false; }
}
}
