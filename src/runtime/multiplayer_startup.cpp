#include "multiplayer_startup.h"
#include "simulation_archive.h"
#include "sha1.h"
#include <stdexcept>

namespace gbarecomp {
namespace {
constexpr std::uint32_t kSaveMagic=0x56534247, kBootMagic=0x42534247, kVersion=1;
constexpr std::uint32_t kMaxSnapshot=8*1024*1024;
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
}
GbaNetplayStartup::GbaNetplayStartup(GbaMultiplayerSession& simulation,RNetSession* network,
    unsigned seat,std::string identity,std::int64_t seed)
    : simulation_(simulation),network_(network),seat_(seat),identity_(std::move(identity)),
      phase_(seat ? Phase::Upload : Phase::ReceiveUpload) {
    if (!network || seat>1 || simulation.input_count()!=2 || simulation.cycle()!=0 ||
        rnet_session_local_slot(network)!=static_cast<int>(seat) || identity_.empty() || identity_.size()>1024)
        throw std::invalid_argument("invalid two-player startup boundary");
    owned_=owner_packet(simulation_.input_machine(seat_),identity_,seed);
    if (seat_) guest_receipt_=gba::sha1(owned_.data(),owned_.size()).bytes;
}
void GbaNetplayStartup::finish() {
    rnet_session_hard_resync(network_);
    const std::uint8_t neutral[2]{};
    rnet_session_prime_delay_inputs(network_,neutral,sizeof(neutral));
    phase_=Phase::Done;
}
GbaNetplayStartup::Status GbaNetplayStartup::poll(std::uint64_t now) {
    if (phase_==Phase::Done) return Status::Ready;
    if (phase_==Phase::Failed) return Status::Failed;
    if (!started_) { started_=true; started_ms_=now; }
    try {
        if (now<started_ms_ || now-started_ms_>=60000) throw std::runtime_error("multiplayer startup timed out");
        rnet_session_pump(network_);
        if (rnet_session_peer_disconnected(network_,0)) throw std::runtime_error("peer left during startup");
        if (!rnet_session_is_running(network_)) return Status::Waiting;
        rnet_u8 op=0,slot=0; const void* data=nullptr; std::size_t size=0;
        const bool ready=rnet_session_state_take_ready(network_,&op,&slot,&data,&size)!=0;
        // BOOT can overtake the upload's final ACK (or replace it if lost).
        // Its owner receipt proves the host consumed this exact upload. The
        // transport has already replaced the sending transfer with BOOT, so
        // do not finish/clear it as though it were still MEMCARD.
        if (phase_==Phase::Upload && sending_ && ready && op==RNET_STATE_OP_BOOT && slot==0)
            phase_=Phase::ReceiveBoot;
        switch (phase_) {
        case Phase::Upload:
            if (!sending_) {
                if (rnet_session_state_begin(network_,RNET_STATE_OP_MEMCARD,1,owned_.data(),owned_.size())==0) sending_=true;
            } else if (ready) {
                if (op!=RNET_STATE_OP_MEMCARD || slot!=1) throw std::runtime_error("unexpected save upload response");
                rnet_session_state_finish(network_,0); phase_=Phase::ReceiveBoot;
            }
            break;
        case Phase::ReceiveUpload:
            if (ready) {
                if (op!=RNET_STATE_OP_MEMCARD || slot!=1) throw std::runtime_error("unexpected player save transfer");
                const auto packet=std::span(static_cast<const std::uint8_t*>(data),size);
                apply_owner(simulation_.input_machine(1),identity_,packet);
                guest_receipt_=gba::sha1(data,size).bytes;
                gba::SimulationArchive<false> a;
                a.identity(kBootMagic); a.identity(kVersion); a.text_identity(identity_); a(guest_receipt_);
                auto state=simulation_.save_state(); a.vector(state,kMaxSnapshot); boot_=a.take();
                rnet_session_state_finish(network_,0); phase_=Phase::SendBoot; sending_=false;
            }
            break;
        case Phase::SendBoot:
            if (!sending_) {
                if (rnet_session_state_begin(network_,RNET_STATE_OP_BOOT,0,boot_.data(),boot_.size())==0) sending_=true;
            } else if (ready) {
                if (op!=RNET_STATE_OP_BOOT || slot!=0) throw std::runtime_error("unexpected boot transfer response");
                rnet_session_state_finish(network_,0); phase_=Phase::SendReady; sending_=false;
            }
            break;
        case Phase::ReceiveBoot:
            if (ready) {
                if (op!=RNET_STATE_OP_BOOT || slot!=0 || size>kMaxSnapshot+2048)
                    throw std::runtime_error("unexpected boot transfer");
                gba::SimulationArchive<true> a({static_cast<const std::uint8_t*>(data),size});
                a.identity(kBootMagic); a.identity(kVersion); a.text_identity(identity_);
                a.identity(guest_receipt_);
                std::vector<std::uint8_t> state; a.vector(state,kMaxSnapshot);
                if (a.remaining()) throw std::runtime_error("trailing boot transfer data");
                std::string error;
                if (!simulation_.load_state(state,&error)) throw std::runtime_error(error);
                rnet_session_state_finish(network_,0); phase_=Phase::ReceiveReady;
            }
            break;
        case Phase::SendReady:
            if (!sending_) {
                if (rnet_session_state_probe(network_,RNET_STATE_OP_SAVE,0,0,simulation_.state_hash())==0) sending_=true;
            } else {
                int match=0;
                if (rnet_session_state_probe_take_reply(network_,&match)) {
                    if (!match) throw std::runtime_error("peer rejected the assembled multiplayer state");
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
