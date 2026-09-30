#include "multiplayer_startup.h"
#include "simulation_archive.h"
#include "sha1.h"
#include <algorithm>
#include <stdexcept>

namespace gbarecomp {
namespace {
constexpr std::uint32_t kSaveMagic=0x56534247, kAssemblyMagic=0x41534247, kVersion=2;
constexpr std::uint32_t kMaxPacket=128*1024+2048;
std::uint8_t save_kind(const gba::GbaSave& s) {
    return s.flash_enabled() ? 1 : s.sram_enabled() ? 2 : s.eeprom_enabled() ? 3 : 0;
}
std::vector<std::uint8_t> save_bytes(const gba::GbaSave& s) {
    return s.flash_enabled() ? s.flash_bytes() : s.sram_enabled() ? s.sram_bytes() :
        s.eeprom_enabled() ? s.eeprom_bytes() : std::vector<std::uint8_t>{};
}
std::vector<std::uint8_t> owner_packet(GbaInstance& m, const std::string& identity,
                                     std::int64_t seed) {
    // Civil dates covering the supported game era, bounded against overflow in
    // RTC conversion. Host timezone and clock never participate after startup.
    if (seed<0 || seed>4102444799LL) throw std::invalid_argument("RTC seed outside 1970..2099");
    gba::SimulationArchive<false> a;
    a.identity(kSaveMagic); a.identity(kVersion); a.text_identity(identity);
    a.identity(m.descriptor.id); a.text_identity(m.descriptor.program_id); a.text_identity(m.descriptor.rom_sha1);
    auto kind=save_kind(m.bus.save()); auto bytes=save_bytes(m.bus.save());
    a(kind,seed); a.vector(bytes,128*1024);
    m.bus.rtc().set_emulated_clock(seed);
    return a.take();
}
void apply_owner(GbaInstance& m, const std::string& identity, std::span<const std::uint8_t> packet) {
    gba::SimulationArchive<true> a(packet);
    a.identity(kSaveMagic); a.identity(kVersion); a.text_identity(identity);
    a.identity(m.descriptor.id); a.text_identity(m.descriptor.program_id); a.text_identity(m.descriptor.rom_sha1);
    std::uint8_t kind=0; std::int64_t seed=0; std::vector<std::uint8_t> bytes;
    a(kind,seed); a.vector(bytes,128*1024);
    auto& s=m.bus.save();
    if (a.remaining() || kind!=save_kind(s) || bytes.size()!=save_bytes(s).size() || seed<0 || seed>4102444799LL)
        throw std::invalid_argument("peer save/RTC configuration mismatch");
    const bool loaded=kind==0 || (kind==1 ? s.load_flash_bytes(bytes.data(),bytes.size()) :
        kind==2 ? s.load_sram_bytes(bytes.data(),bytes.size()) : s.load_eeprom_bytes(bytes.data(),bytes.size()));
    if (!loaded) throw std::invalid_argument("invalid peer save image");
    s.clear_dirty(); m.bus.rtc().set_emulated_clock(seed);
}
std::uint32_t others(unsigned seats,unsigned seat) { return ((1u<<seats)-1)&~(1u<<seat); }
}
std::string gba_netplay_seat_names(std::uint32_t mask) {
    std::string names;
    for (unsigned seat=0;seat<32;++seat) if (mask&(1u<<seat))
        names+=(names.empty() ? "player " : ", player ")+std::to_string(seat+1);
    return names.empty() ? "no player" : names;
}
GbaNetplayStartup::GbaNetplayStartup(GbaMultiplayerSession& simulation,RNetSession* network,
    unsigned seat,std::string identity,std::int64_t seed)
    : simulation_(simulation),network_(network),seat_(seat),
      seats_(static_cast<unsigned>(simulation.input_count())),identity_(std::move(identity)),
      phase_(seat ? Phase::Upload : Phase::ReceiveUploads) {
    if (!network || seats_<2 || seats_>kGbaMaxSessionPlayers || seat>=seats_ || simulation.cycle()!=0 ||
        rnet_session_local_slot(network)!=static_cast<int>(seat) || identity_.empty() || identity_.size()>1024)
        throw std::invalid_argument("invalid multiplayer startup boundary");
    packets_.resize(seats_);
    packets_[seat_]=owner_packet(simulation_.input_machine(seat_),identity_,seed);
}
void GbaNetplayStartup::finish() {
    rnet_session_hard_resync(network_);
    const std::uint8_t neutral[2]{};
    rnet_session_prime_delay_inputs(network_,neutral,sizeof(neutral));
    phase_=Phase::Done;
}
void GbaNetplayStartup::apply_assembly(std::span<const std::uint8_t> bytes) {
    gba::SimulationArchive<true> a(bytes);
    a.identity(kAssemblyMagic); a.identity(kVersion); a.text_identity(identity_);
    a.identity(static_cast<std::uint32_t>(seats_));
    std::vector<std::vector<std::uint8_t>> packets(seats_);
    for (auto& packet:packets) a.vector(packet,kMaxPacket);
    if (a.remaining()) throw std::runtime_error("trailing startup assembly data");
    // The assembly is also this upload's receipt: seat 0 must carry our
    // exact bytes, never a stale, altered or another player's packet.
    if (packets[seat_]!=packets_[seat_])
        throw std::runtime_error("host did not assemble this player's exact save and clock");
    for (unsigned seat=0;seat<seats_;++seat)
        apply_owner(simulation_.input_machine(seat),identity_,packets[seat]);
}
GbaNetplayStartup::Status GbaNetplayStartup::poll(std::uint64_t now) {
    if (phase_==Phase::Done) return Status::Ready;
    if (phase_==Phase::Failed) return Status::Failed;
    if (!started_) { started_=true; started_ms_=now; }
    try {
        if (now<started_ms_ || now-started_ms_>=60000) {
            std::uint32_t expect=0,replied=0,match=0;
            const auto waiting=phase_==Phase::SendReady && rnet_session_state_probe_replies(network_,&expect,&replied,&match) ?
                expect&~replied : phase_==Phase::SendAssembly ? rnet_session_state_pending_receivers(network_) : 0u;
            throw std::runtime_error("multiplayer startup timed out"+
                (waiting ? " waiting for "+gba_netplay_seat_names(waiting) : std::string()));
        }
        rnet_session_pump(network_);
        if (const auto gone=rnet_session_peer_gone_mask(network_)&others(seats_,seat_))
            throw std::runtime_error(gba_netplay_seat_names(gone)+" left during startup");
        if (!rnet_session_is_running(network_)) return Status::Waiting;
        int from=-1; rnet_u8 op=0,slot=0; const void* data=nullptr; std::size_t size=0;
        const bool ready=rnet_session_state_take_ready_from(network_,&from,&op,&slot,&data,&size)!=0;
        const auto packet=std::span(static_cast<const std::uint8_t*>(data),ready ? size : 0);
        // The assembly can overtake the upload's final ACK (or replace it if
        // lost): seat 0 starts it only after consuming every upload, and the
        // transport has already replaced our sending transfer with it.
        if (phase_==Phase::Upload && sending_ && ready && from==0) phase_=Phase::ReceiveAssembly;
        switch (phase_) {
        case Phase::Upload:
            if (!sending_) {
                if (rnet_session_state_begin(network_,RNET_STATE_OP_MEMCARD,static_cast<rnet_u8>(seat_),
                        packets_[seat_].data(),packets_[seat_].size())==0) sending_=true;
            } else if (ready) {
                if (from!=static_cast<int>(seat_) || op!=RNET_STATE_OP_MEMCARD || slot!=seat_)
                    throw std::runtime_error("unexpected save upload response");
                rnet_session_state_finish_from(network_,from,0); phase_=Phase::ReceiveAssembly;
            }
            break;
        case Phase::ReceiveUploads:
            if (ready) {
                if (op!=RNET_STATE_OP_MEMCARD || from<1 || from>=static_cast<int>(seats_) ||
                    slot!=from || !packets_[from].empty() || size>kMaxPacket)
                    throw std::runtime_error("unexpected player save transfer");
                apply_owner(simulation_.input_machine(from),identity_,packet);
                packets_[from].assign(packet.begin(),packet.end());
                rnet_session_state_finish_from(network_,from,0);
            }
            if (std::all_of(packets_.begin(),packets_.end(),[](const auto& p) { return !p.empty(); })) {
                apply_owner(simulation_.input_machine(0),identity_,packets_[0]); // same order as every guest
                gba::SimulationArchive<false> a;
                a.identity(kAssemblyMagic); a.identity(kVersion); a.text_identity(identity_);
                a.identity(static_cast<std::uint32_t>(seats_));
                for (auto& p:packets_) a.vector(p,kMaxPacket);
                assembly_=a.take(); phase_=Phase::SendAssembly; sending_=false;
            }
            break;
        case Phase::SendAssembly:
            if (!sending_) {
                if (rnet_session_state_begin(network_,RNET_STATE_OP_BOOT,0,assembly_.data(),assembly_.size())==0) sending_=true;
            } else if (ready) {
                if (from!=0 || op!=RNET_STATE_OP_BOOT || slot!=0) throw std::runtime_error("unexpected startup broadcast response");
                rnet_session_state_finish_from(network_,0,0); phase_=Phase::SendReady; sending_=false;
            }
            break;
        case Phase::ReceiveAssembly:
            if (ready) {
                if (from!=0 || op!=RNET_STATE_OP_BOOT || slot!=0 || size>kMaxPacket*kGbaMaxSessionPlayers+2048)
                    throw std::runtime_error("unexpected startup broadcast");
                apply_assembly(packet);
                rnet_session_state_finish_from(network_,0,0); phase_=Phase::ReceiveReady;
            }
            break;
        case Phase::SendReady:
            if (!sending_) {
                if (rnet_session_state_probe(network_,RNET_STATE_OP_SAVE,0,0,simulation_.state_hash())==0) sending_=true;
            } else {
                std::uint32_t expect=0,replied=0,matched=0;
                if (rnet_session_state_probe_replies(network_,&expect,&replied,&matched) &&
                    (replied&expect&~matched))
                    throw std::runtime_error(gba_netplay_seat_names(replied&expect&~matched)+
                        " rejected the assembled multiplayer state");
                int match=0;
                if (rnet_session_state_probe_take_reply(network_,&match)) {
                    if (!match) throw std::runtime_error("a peer rejected the assembled multiplayer state");
                    rnet_session_state_probe_finish(network_); finish();
                }
            }
            break;
        case Phase::ReceiveReady: {
            rnet_u32 bytes=0,hash=0;
            if (rnet_session_state_probe_pending(network_,&op,&slot,&bytes,&hash)) {
                const bool match=op==RNET_STATE_OP_SAVE && slot==0 && bytes==0 && hash==simulation_.state_hash();
                if (rnet_session_state_probe_reply(network_,match)!=0) throw std::runtime_error("boot ready reply failed");
                if (!match) throw std::runtime_error("boot state digest mismatch");
                // SAVE's zero-size coordination probe retains its response
                // for automatic retransmission. The pinned BOOT probe clears
                // it immediately, stranding the host if this reply is lost.
                finish();
            }
            break;
        }
        default: break;
        }
    } catch (const std::exception& e) { error_=e.what(); phase_=Phase::Failed; }
    return phase_==Phase::Done ? Status::Ready : phase_==Phase::Failed ? Status::Failed : Status::Waiting;
}
}
