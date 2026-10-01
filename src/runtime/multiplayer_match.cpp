#include "multiplayer_match.h"
#include <retcomm_rbengine/mono_ms.h>
#include <cstdio>
#include <stdexcept>

namespace gbarecomp {
GbaNetplayMatch::GbaNetplayMatch(GbaMultiplayerSession& simulation, GbaNetplayMatchOptions options)
    : simulation_(simulation), options_(std::move(options)), host_(simulation) {
    const auto& n=options_.network;
    const auto seats=simulation.input_count();
    const auto full=(1u<<seats)-1;
    // Seats are dense: the launcher compacts lobby seats before this point.
    if (seats<2 || seats>kGbaMaxSessionPlayers || n.slot_count!=seats || n.local_slot>=seats ||
        n.wire_slot || (n.occupied_mask && n.occupied_mask!=full) || !n.session_id ||
        n.input_delay<2 || n.input_delay>20 || options_.prediction<6 || options_.prediction>16 ||
        !options_.build_fingerprint || options_.identity.empty())
        throw std::invalid_argument("invalid GBA cable match configuration");
    if (!options_.restored_pair && simulation.cycle())
        throw std::invalid_argument("an advanced session requires paired checkpoint startup");
    seat_=n.local_slot; slots_=static_cast<int>(seats); occupied_=full;
    delay_=n.input_delay; prediction_=options_.prediction;
    // Refuse mismatched admission policies before either peer can simulate.
    const auto handshake_identity=options_.identity+"|gba-match/2:"+std::to_string(options_.build_fingerprint)+":"+
        std::to_string(n.protocol_magic)+":"+std::to_string(delay_)+":"+
        std::to_string(prediction_)+":"+std::to_string(options_.rollback)+":"+std::to_string(options_.force_turn)+":"+
        std::to_string(options_.planned_finish_tick);
    host_.sample_local=[this](std::uint32_t tick) {
        const auto buttons=sample_local ? sample_local(tick)&0x3ff : 0;
        return static_cast<std::uint16_t>(buttons|(checkpoint_requested_ ? GbaNetplayHost::kCheckpointRequest : 0));
    };
    const auto callbacks=host_.delay_callbacks();
    network_.reset(rnet_session_create(&n,&callbacks)); session_=network_.get();
    if (!session_) throw std::runtime_error("cannot create GBA match transport");
    if (options_.restored_pair)
        agreement_=std::make_unique<GbaNetplayCheckpointAgreement>(host_,session_,seat_,handshake_identity,UINT32_MAX,0);
    else
        startup_=std::make_unique<GbaNetplayStartup>(simulation_,session_,seat_,handshake_identity,options_.rtc_seed_seconds);
    if (options_.planned_finish_tick) finish_at(options_.planned_finish_tick);
}
GbaNetplayMatch::~GbaNetplayMatch() = default;

void GbaNetplayMatch::start_driver() {
    startup_.reset(); agreement_.reset(); host_.begin_match();
    if (options_.restored_pair) {
        rnet_session_hard_resync(session_);
        const std::uint8_t neutral[2]{};
        rnet_session_prime_delay_inputs(session_,neutral,sizeof(neutral));
    }
    if (options_.rollback) {
        driver_.reset(rnet_rb_driver_create());
        if (!driver_) throw std::bad_alloc();
        RNetRbDriverConfig cfg{};
        cfg.session=&session_; cfg.local_slot=&seat_; cfg.slot_count=&slots_;
        cfg.input_delay=&delay_; cfg.input_prediction=&prediction_;
        cfg.force_turn=options_.force_turn; cfg.occupied_mask=occupied_;
        cfg.replay_mode=RNET_RB_REPLAY_INCREMENTAL; cfg.snap_depth=host_.snapshot_depth();
        cfg.part_names[0]="GBA0"; cfg.part_names[1]="other-GBAs"; cfg.part_names[2]="cable-and-scheduler";
        cfg.log_prefix="gba_match_rb"; cfg.env_alias="GBA_RB";
        rnet_rb_driver_set_identity(driver_.get(),options_.build_fingerprint,simulation_.state_hash());
        const auto callbacks=host_.callbacks();
        if (!rnet_rb_driver_start(driver_.get(),&cfg,&callbacks))
            throw std::runtime_error("cannot start GBA rollback driver");
    }
    phase_=Phase::Running;
}
void GbaNetplayMatch::fail(std::string message) {
    error_=message.empty() ? "rollback driver requested return to lobby" : std::move(message);
    phase_=Phase::Failed;
    replay_ticks_=replay_ticks(); driver_.reset();
    simulation_.discard_audio_output();
}
std::uint64_t GbaNetplayMatch::replay_ticks() const {
    return driver_ ? rnet_rb_driver_resim_ticks(driver_.get()) : replay_ticks_;
}
GbaNetplayMatch::Step GbaNetplayMatch::poll(bool allow_simulation) {
    try {
        rnet_session_pump(session_);
        connection_=gba_netplay_connection_status(session_);
        if (phase_==Phase::Failed || phase_==Phase::CheckpointReady) return Step::Idle;
        if (connection_.phase==GbaConnectionPhase::TimedOut || connection_.phase==GbaConnectionPhase::PeerLeft)
            throw std::runtime_error("GBA match peer unavailable: "+gba_netplay_seat_names(connection_.seats)+
                (connection_.phase==GbaConnectionPhase::PeerLeft ? " left" : " timed out"));
        if (phase_==Phase::Starting) {
            bool ready=false;
            if (startup_) {
                const auto status=startup_->poll(rbe_mono_ms());
                if (status==GbaNetplayStartup::Status::Failed) throw std::runtime_error(startup_->error());
                ready=status==GbaNetplayStartup::Status::Ready;
            } else {
                const auto status=agreement_->poll(rbe_mono_ms());
                if (status==GbaNetplayCheckpointAgreement::Status::Failed) throw std::runtime_error(agreement_->error());
                ready=status==GbaNetplayCheckpointAgreement::Status::Ready;
            }
            if (ready) start_driver();
            return Step::Idle;
        }
        if (phase_==Phase::AgreeingCheckpoint) {
            const auto status=agreement_->poll(rbe_mono_ms());
            if (status==GbaNetplayCheckpointAgreement::Status::Failed) throw std::runtime_error(agreement_->error());
            if (status==GbaNetplayCheckpointAgreement::Status::Ready) phase_=Phase::CheckpointReady;
            return Step::Idle;
        }
        if (host_.return_to_lobby_requested()) throw std::runtime_error(host_.error());
        const auto through=driver_ ? rnet_rb_driver_confirmed_through(driver_.get()) : host_.next_tick()-1;
        if (!finish_tick_ && (!driver_ || through!=0)) {
            if (const auto request=host_.checkpoint_request(through)) {
                // Leave time for every peer to observe the confirmed control
                // row (D<=20, prediction<=16) before simulating the boundary.
                // The host pins that boundary's snapshot, so agreement latency
                // does not depend on the rollback ring's reach.
                // A pathological late observation fails closed: never save a
                // different boundary or a speculative cartridge independently.
                if (*request>UINT32_MAX-64) throw std::runtime_error("checkpoint tick overflow");
                finish_at(*request+64);
            }
        }
        const bool confirmed=finish_tick_ && host_.next_tick()>=finish_tick_ && through!=UINT32_MAX &&
            (!driver_ || through!=0) && through>=finish_tick_-1;
        if (driver_ && finish_tick_ && (agreement_ || confirmed)) {
            // QUIESCE also stops the other peer opening corrections. First
            // agree the full candidate while both drivers can still correct;
            // one peer's watermark is not permission to stop the other.
            if (!agreement_) agreement_=std::make_unique<GbaNetplayCheckpointAgreement>(
                host_,session_,seat_,options_.identity,through,finish_tick_,true);
            agreement_->update_confirmation(through);
            if (!host_.replaying()) {
                const auto status=agreement_->poll(rbe_mono_ms());
                if (status==GbaNetplayCheckpointAgreement::Status::Failed)
                    throw std::runtime_error(agreement_->error());
                checkpoint_agreed_=status==GbaNetplayCheckpointAgreement::Status::Ready;
            }
            if (checkpoint_agreed_) rnet_rb_driver_request_quiesce(driver_.get());
        }
        if (driver_ && rnet_rb_driver_quiesce_state(driver_.get())==RNET_RB_QUIESCE_TIMED_OUT)
            throw std::runtime_error("GBA checkpoint drain timed out");
        if ((driver_ && checkpoint_agreed_ && rnet_rb_driver_quiesce_state(driver_.get())==RNET_RB_QUIESCE_DRAINED) ||
            (!driver_ && confirmed)) {
            std::fprintf(stderr,"gba checkpoint seat=%d target=%u sim=%u confirmed=%u\n",seat_,finish_tick_,host_.next_tick(),through);
            if (driver_) {
                if (!host_.checkpoint_unchanged(agreement_->checkpoint()))
                    throw std::runtime_error("agreed checkpoint changed during rollback drain");
                replay_ticks_=replay_ticks(); driver_.reset();
                phase_=Phase::CheckpointReady;
                return Step::Idle;
            }
            replay_ticks_=replay_ticks(); driver_.reset();
            agreement_=std::make_unique<GbaNetplayCheckpointAgreement>(host_,session_,seat_,options_.identity,through,finish_tick_);
            phase_=Phase::AgreeingCheckpoint;
            return Step::Idle;
        }
        bool ran=false, replay=false;
        if (!allow_simulation) return Step::Idle;
        if (driver_) {
            const auto admit=rnet_rb_driver_poll_admit(driver_.get());
            replay=admit==RNET_RB_ADMIT_REPLAY;
            if (admit!=RNET_RB_ADMIT_STALL) {
                ran=host_.run_published_tick();
                if (!ran) throw std::runtime_error(host_.error());
                rnet_rb_driver_finish_frame(driver_.get());
            }
        } else ran=host_.try_delay_frame(session_);
        if (host_.return_to_lobby_requested()) throw std::runtime_error(host_.error());
        return ran ? (replay ? Step::Replay : Step::Forward) : Step::Idle;
    } catch (const std::exception& e) { fail(e.what()); return Step::Idle; }
}
bool GbaNetplayMatch::take_output(GbaNetplayOutput& output) {
    return phase_==Phase::Running && host_.take_output(seat_,output);
}
void GbaNetplayMatch::finish_at(std::uint32_t tick) {
    if (!tick || tick<host_.next_tick() || finish_tick_ ||
        (phase_!=Phase::Starting && phase_!=Phase::Running))
        throw std::invalid_argument("checkpoint requires one agreed future input boundary");
    finish_tick_=tick;
    host_.pin_checkpoint(tick);
    std::fprintf(stderr,"gba checkpoint planned seat=%d target=%u sim=%u\n",seat_,tick,host_.next_tick());
}
void GbaNetplayMatch::request_checkpoint() {
    if (phase_==Phase::Running && !finish_tick_) checkpoint_requested_=true;
}
const GbaConfirmedCheckpoint& GbaNetplayMatch::checkpoint() const {
    if (phase_!=Phase::CheckpointReady) throw std::logic_error("GBA checkpoint has not been agreed");
    return agreement_->checkpoint();
}
bool GbaNetplayMatch::store_checkpoint(const std::filesystem::path& path,std::string* error) const {
    if (phase_!=Phase::CheckpointReady) {
        if (error) *error="GBA checkpoint has not been agreed";
        return false;
    }
    return gba_store_agreed_checkpoint(*agreement_,path,error);
}
}
