#include "multiplayer_match.h"
#include <retcomm_rbengine/mono_ms.h>
#include <stdexcept>

namespace gbarecomp {
GbaNetplayMatch::GbaNetplayMatch(GbaMultiplayerSession& simulation, GbaNetplayMatchOptions options)
    : simulation_(simulation), options_(std::move(options)), host_(simulation) {
    const auto& n=options_.network;
    if (simulation.input_count()!=2 || n.slot_count!=2 || n.local_slot>=2 ||
        n.wire_slot || (n.occupied_mask && n.occupied_mask!=3) || !n.session_id ||
        n.input_delay<2 || n.input_delay>20 || options_.prediction<6 || options_.prediction>16 ||
        !options_.build_fingerprint || options_.identity.empty())
        throw std::invalid_argument("invalid two-player cable match configuration");
    if (!options_.restored_pair && simulation.cycle())
        throw std::invalid_argument("an advanced session requires paired checkpoint startup");
    seat_=n.local_slot; delay_=n.input_delay; prediction_=options_.prediction;
    // Refuse mismatched admission policies before either peer can simulate.
    const auto handshake_identity=options_.identity+"|gba-match/1:"+std::to_string(options_.build_fingerprint)+":"+
        std::to_string(n.protocol_magic)+":"+std::to_string(delay_)+":"+
        std::to_string(prediction_)+":"+std::to_string(options_.rollback);
    host_.sample_local=[this](std::uint32_t tick) {
        return sample_local ? sample_local(tick) : std::uint16_t(0);
    };
    const auto callbacks=host_.delay_callbacks();
    network_.reset(rnet_session_create(&n,&callbacks)); session_=network_.get();
    if (!session_) throw std::runtime_error("cannot create GBA match transport");
    if (options_.restored_pair)
        agreement_=std::make_unique<GbaNetplayCheckpointAgreement>(host_,session_,seat_,handshake_identity,UINT32_MAX,0);
    else
        startup_=std::make_unique<GbaNetplayStartup>(simulation_,session_,seat_,handshake_identity,options_.rtc_seed_seconds);
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
        cfg.replay_mode=RNET_RB_REPLAY_INCREMENTAL; cfg.snap_depth=GbaNetplayHost::kSnapshotDepth;
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
GbaNetplayMatch::Step GbaNetplayMatch::poll() {
    try {
        rnet_session_pump(session_);
        connection_=gba_netplay_connection_status(session_);
        if (phase_==Phase::Failed || phase_==Phase::CheckpointReady) return Step::Idle;
        if (connection_.phase==GbaConnectionPhase::TimedOut || connection_.phase==GbaConnectionPhase::PeerLeft)
            throw std::runtime_error("GBA match peer unavailable");
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
        const bool confirmed=finish_tick_ && host_.next_tick()>=finish_tick_ && through!=UINT32_MAX &&
            (!driver_ || through!=0) && through>=finish_tick_-1;
        if (driver_ && confirmed) rnet_rb_driver_request_quiesce(driver_.get());
        if (driver_ && rnet_rb_driver_quiesce_state(driver_.get())==RNET_RB_QUIESCE_TIMED_OUT)
            throw std::runtime_error("GBA checkpoint drain timed out");
        if (confirmed && (!driver_ || rnet_rb_driver_quiesce_state(driver_.get())==RNET_RB_QUIESCE_DRAINED)) {
            replay_ticks_=replay_ticks(); driver_.reset();
            agreement_=std::make_unique<GbaNetplayCheckpointAgreement>(host_,session_,seat_,options_.identity,through,finish_tick_);
            phase_=Phase::AgreeingCheckpoint;
            return Step::Idle;
        }
        bool ran=false, replay=false;
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
