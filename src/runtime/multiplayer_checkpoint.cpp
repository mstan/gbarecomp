#include "multiplayer_checkpoint.h"
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
std::uint32_t ready_hash(const std::vector<std::uint8_t>& bytes) {
    const auto h=gba::sha1(bytes.data(),bytes.size()).bytes;
    return std::uint32_t(h[0]) | (std::uint32_t(h[1])<<8) |
        (std::uint32_t(h[2])<<16) | (std::uint32_t(h[3])<<24);
}
}
GbaNetplayCheckpointAgreement::GbaNetplayCheckpointAgreement(const GbaNetplayHost& host,
    RNetSession* network,unsigned seat,std::string identity,std::uint32_t through,std::uint32_t tick)
    : host_(host),network_(network),seat_(seat),identity_(std::move(identity)),through_(through),
      phase_(seat ? Phase::ReceiveProposal : Phase::SendProposal) {
    if (!network || seat>1 || rnet_session_local_slot(network)!=static_cast<int>(seat) ||
        identity_.empty() || identity_.size()>1024)
        throw std::invalid_argument("invalid checkpoint agreement configuration");
    if (!seat_) {
        if (!host_.copy_checkpoint(tick,through_,checkpoint_))
            throw std::invalid_argument("proposed checkpoint is unavailable or unconfirmed");
        proposal_=proposal(identity_,checkpoint_); ready_hash_=ready_hash(proposal_);
    }
}
GbaNetplayCheckpointAgreement::Status GbaNetplayCheckpointAgreement::poll(std::uint64_t now) {
    if (phase_==Phase::Done) return Status::Ready;
    if (phase_==Phase::Failed) return Status::Failed;
    if (!started_) { started_=true; started_ms_=now; }
    try {
        if (now<started_ms_ || now-started_ms_>=60000)
            throw std::runtime_error("checkpoint agreement timed out");
        rnet_session_pump(network_);
        if (rnet_session_peer_disconnected(network_,0)) throw std::runtime_error("peer left before checkpoint agreement");
        if (!rnet_session_is_running(network_)) return Status::Waiting;
        rnet_u8 op=0,slot=0; const void* data=nullptr; std::size_t size=0;
        const bool ready=rnet_session_state_take_ready(network_,&op,&slot,&data,&size)!=0;
        // The receipt may supersede the proposal sender before its last ACK
        // arrives. A matching full receipt is stronger than that transport ACK.
        if (phase_==Phase::SendProposal && sending_ && ready && op==RNET_STATE_OP_MEMCARD && slot==1)
            phase_=Phase::ReceiveReceipt;
        switch (phase_) {
        case Phase::SendProposal:
        case Phase::SendReceipt: {
            const bool receipt=phase_==Phase::SendReceipt;
            const auto expected_op=receipt ? RNET_STATE_OP_MEMCARD : RNET_STATE_OP_SAVE;
            const auto expected_slot=receipt ? 1 : 0;
            if (!sending_) {
                if (rnet_session_state_begin(network_,expected_op,expected_slot,proposal_.data(),proposal_.size())==0)
                    sending_=true;
            } else if (ready) {
                if (op!=expected_op || slot!=expected_slot) throw std::runtime_error("unexpected checkpoint transfer response");
                rnet_session_state_finish(network_,0);
                phase_=receipt ? Phase::ReceiveReady : Phase::ReceiveReceipt;
                sending_=false;
            }
            break;
        }
        case Phase::ReceiveProposal:
            if (ready) {
                if (op!=RNET_STATE_OP_SAVE || slot!=0 || size>1100)
                    throw std::runtime_error("unexpected checkpoint proposal");
                const auto bytes=std::span(static_cast<const std::uint8_t*>(data),size);
                const auto tick=boundary(bytes,identity_);
                if (!host_.copy_checkpoint(tick,through_,checkpoint_))
                    throw std::runtime_error("peer checkpoint is unavailable or unconfirmed locally");
                proposal_=proposal(identity_,checkpoint_);
                if (!std::equal(bytes.begin(),bytes.end(),proposal_.begin(),proposal_.end()))
                    throw std::runtime_error("checkpoint state digest mismatch");
                ready_hash_=ready_hash(proposal_);
                rnet_session_state_finish(network_,0);
                phase_=Phase::SendReceipt; sending_=false;
            }
            break;
        case Phase::ReceiveReceipt:
            if (ready) {
                if (op!=RNET_STATE_OP_MEMCARD || slot!=1 || size!=proposal_.size() ||
                    !std::equal(proposal_.begin(),proposal_.end(),static_cast<const std::uint8_t*>(data)))
                    throw std::runtime_error("peer rejected the exact checkpoint");
                rnet_session_state_finish(network_,0);
                phase_=Phase::SendReady; sending_=false;
            }
            break;
        case Phase::SendReady:
            if (!sending_) {
                if (rnet_session_state_probe(network_,RNET_STATE_OP_SAVE,0,0,ready_hash_)==0) sending_=true;
            } else {
                int match=0;
                if (rnet_session_state_probe_take_reply(network_,&match)) {
                    if (!match) throw std::runtime_error("checkpoint ready barrier rejected");
                    rnet_session_state_probe_finish(network_); phase_=Phase::Done;
                }
            }
            break;
        case Phase::ReceiveReady: {
            rnet_u32 bytes=0,hash=0;
            if (rnet_session_state_probe_pending(network_,&op,&slot,&bytes,&hash)) {
                const bool match=op==RNET_STATE_OP_SAVE && slot==0 && bytes==0 && hash==ready_hash_;
                if (rnet_session_state_probe_reply(network_,match)!=0 || !match)
                    throw std::runtime_error("checkpoint ready barrier mismatch");
                phase_=Phase::Done;
            }
            break;
        }
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
