#include "multiplayer_session.h"
#include "gba_simulation_state.h"
#include "simulation_archive.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace gbarecomp {
namespace {
template<class Archive, class Context> void execution_state(Archive& a, Context& state) {
    auto& c = state.cpu;
    a(c.R,c.cpsr,c.banked_sp,c.banked_lr,c.banked_spsr,c.r8_12_user,c.r8_12_fiq);
    const auto mode = c.cpsr & 31;
    if (mode != 0x10 && mode != 0x11 && mode != 0x12 && mode != 0x13 &&
        mode != 0x17 && mode != 0x1b && mode != 0x1f)
        throw std::invalid_argument("invalid CPU mode");
    std::uint32_t count = static_cast<std::uint32_t>(state.returns.size());
    a(count);
    if (count > 1024) throw std::invalid_argument("excessive guest return depth");
    if constexpr (!std::is_const_v<Context>) state.returns.resize(count);
    for (auto& pc : state.returns) a(pc);
    count = static_cast<std::uint32_t>(state.interrupts.size());
    a(count);
    if (count > 1024) throw std::invalid_argument("excessive IRQ depth");
    if constexpr (!std::is_const_v<Context>) state.interrupts.resize(count);
    for (auto& f : state.interrupts) a(f.preserved,f.return_floor,f.return_depth,f.iret_depth);
    a(state.return_floor,state.irq_depth,state.iret_depth,state.resume_pc,state.irq_from_halt);
    if (state.return_floor > state.returns.size() || state.irq_depth != state.interrupts.size())
        throw std::invalid_argument("inconsistent execution continuation");
    for (const auto& f : state.interrupts)
        if (f.return_floor > f.return_depth || f.return_depth > state.returns.size())
            throw std::invalid_argument("inconsistent IRQ return floor");
}
template<class Archive> void manifest(Archive& a, const GbaSessionConfig& config) {
    a.identity(std::uint32_t{0x53534247}); // GBSS
    a.identity(std::uint32_t{1});
    a.identity(static_cast<std::uint32_t>(config.machines.size()));
    for (const auto& m : config.machines) {
        a.identity(m.id); a.text_identity(m.program_id); a.text_identity(m.rom_sha1); a.identity(m.boot);
    }
    a.identity(static_cast<std::uint32_t>(config.input_machines.size()));
    for (auto id : config.input_machines) a.identity(id);
    a.identity(static_cast<std::uint32_t>(config.links.size()));
    for (const auto& group : config.links) {
        a.identity(group.medium); a.identity(static_cast<std::uint32_t>(group.machines.size()));
        for (auto id : group.machines) a.identity(id);
    }
}
// Restore the legacy caller's execution window even when a guest fails.
class ExecutionScope {
public:
    ExecutionScope() : bus_(active_bus()), ppu_(active_ppu()) {
        if (runtime_has_resumable_context())
            throw std::logic_error("nested multiplayer session execution");
        runtime_capture_arm_context(arm_);
        if (arm_.irq_depth)
            throw std::logic_error("session execution from a legacy IRQ handler");
        runtime_capture_timing_context(timing_);
        runtime_session_execution(true);
    }
    ~ExecutionScope() {
        runtime_set_resumable_context(nullptr);
        runtime_session_execution(false);
        runtime_restore_arm_context(arm_);
        runtime_restore_timing_context(timing_);
        set_active_bus(bus_); set_active_ppu(ppu_);
    }
private:
    RuntimeArmContext arm_;
    RuntimeTimingContext timing_;
    gba::GbaBus* bus_;
    gba::GbaPpu* ppu_;
};
}
GbaInstance::GbaInstance(GbaMachineDescriptor value) : descriptor(std::move(value)) {
    execution.cpu.cpsr = 0xd3; // hardware reset: SVC, ARM, IRQ/FIQ masked
    execution.ram_dispatch_boundary=runtime_session_ram_dispatch_boundary;
    bus.io().set_bus(&bus);
    bus.io().set_ppu(&ppu);
    bus.rtc().set_emulated_clock(0); // startup replaces with agreed per-machine seed
}
GbaMultiplayerSession::GbaMultiplayerSession(GbaSessionConfig config) : config_(std::move(config)) {
    std::string error;
    if (!validate_multiplayer_mvp(config_, &error)) throw std::invalid_argument(error);
    for (const auto& descriptor : config_.machines)
        machines_.push_back(std::make_unique<GbaInstance>(descriptor));
    if (config_.links[0].medium==GbaLinkMedium::Wireless)
        wireless_=std::make_unique<gba::GbaWirelessDomain>(config_.links[0].machines.size());
    else cable_ = std::make_unique<gba::GbaLinkHub>(config_.links[0].machines.size());
    for (std::size_t port = 0; port < config_.links[0].machines.size(); ++port)
        machine(config_.links[0].machines[port]).bus.io().set_serial_device(
            wireless_ ? &wireless_->endpoint(port) : &cable_->endpoint(port));
}
GbaMultiplayerSession::~GbaMultiplayerSession() = default;
gba::GbaLinkHub& GbaMultiplayerSession::cable() {
    if (!cable_) throw std::logic_error("wireless session has no cable");
    return *cable_;
}
std::vector<std::uint8_t> GbaMultiplayerSession::link_state() const {
    return wireless_ ? wireless_->save_state() : cable_->save_state();
}
GbaInstance& GbaMultiplayerSession::machine(GbaMachineId id) {
    for (auto& instance : machines_) if (instance->descriptor.id == id) return *instance;
    throw std::out_of_range("unknown GBA machine ID");
}
const GbaInstance& GbaMultiplayerSession::machine(GbaMachineId id) const {
    for (const auto& instance : machines_) if (instance->descriptor.id == id) return *instance;
    throw std::out_of_range("unknown GBA machine ID");
}
void GbaMultiplayerSession::bind(GbaInstance& instance) {
    runtime_restore_arm_context(instance.execution);
    runtime_restore_timing_context(instance.timing);
    runtime_set_resumable_context(&instance.execution);
    set_active_bus(&instance.bus); set_active_ppu(&instance.ppu);
}
void GbaMultiplayerSession::capture(GbaInstance& instance) {
    runtime_capture_arm_context(instance.execution);
    runtime_capture_timing_context(instance.timing);
}
void GbaMultiplayerSession::run_frame(std::span<const std::uint16_t> buttons) {
    if (buttons.size() != config_.input_machines.size())
        throw std::invalid_argument("input row does not cover every session seat");
    if (cycle_ > std::numeric_limits<std::uint64_t>::max() - gba::GbaPpu::kCyclesPerFrame)
        throw std::overflow_error("GBA session cycle overflow");
    for (std::size_t seat = 0; seat < buttons.size(); ++seat) {
        auto& io = machine(config_.input_machines[seat]).bus.io();
        io.set_synthesized_keyinput(0x3ff);
        io.set_keyinput(static_cast<std::uint16_t>(~buttons[seat] & 0x3ff));
    }
    run_until(cycle_ + gba::GbaPpu::kCyclesPerFrame);
}
void GbaMultiplayerSession::run_until(std::uint64_t target) {
    if (target < cycle_) throw std::invalid_argument("session clock cannot run backwards");
    ExecutionScope scope;
    // Sample one in 60 session frames when profiling is requested. Ordinary
    // sessions take no clock reads inside the scheduler loop.
    using ProfileClock = std::chrono::steady_clock;
    static const bool profile_enabled = std::getenv("GBA_SESSION_PROFILE") != nullptr;
    static std::uint64_t profile_frames = 0;
    const bool profile = profile_enabled && (++profile_frames % 60 == 0);
    const auto profile_start = profile ? ProfileClock::now() : ProfileClock::time_point{};
    std::chrono::nanoseconds cpu_time{0}, schedule_time{0}, device_time{0};
    std::uint64_t dispatches = 0, passes = 0;
    unsigned zero_cycle_passes = 0;
    while (cycle_ < target) {
        const auto cpu_start = profile ? ProfileClock::now() : ProfileClock::time_point{};
        // Simultaneous CPU edges have stable manifest order. Every device is
        // already at cycle_, so even a transfer begun by either CPU sees the
        // peer's state at that same time, never a host scheduling artifact.
        for (auto& ptr : machines_) {
            auto& instance = *ptr;
            auto& io = instance.bus.io();
            if (instance.timing.cycles > cycle_) continue;
            if (io.halted()) {
                if (!(io.ie() & io.if_reg())) continue;
                io.clear_halt();
                instance.timing.cycles = cycle_ + 4; // ARM7 HALT wake latency
                continue;
            }
            instance.timing.cycles = cycle_;
            bind(instance);
            const auto& e=instance.execution;
            const bool ram_callback=e.ram_dispatch && e.cpu.R[15]>=0x02000000 && e.cpu.R[15]<0x04000000 &&
                (!e.ram_dispatch_filter || e.ram_dispatch_filter(e.cpu.R[15],(e.cpu.cpsr&CPSR_T_BIT) ? 1 : 0));
            const bool generated = !e.program_dispatch && !e.immediate_override && !e.read_override &&
                !e.force_interp && !e.entry_hook && !e.bios_hook &&
                !gba::g_rom_read16_override && !gba::g_rom_read32_override;
            if (native_slices_ && generated && !ram_callback) {
                // No newly started cable transfer can complete within this
                // lookahead (normal 8-bit fast clock takes 64 cycles).
                // Existing device events and the caller's boundary shorten it.
                auto deadline=cycle_+std::min<std::uint64_t>(target-cycle_,63);
                for (const auto& machine : machines_) {
                    const auto& device_io=machine->bus.io();
                    auto distance=std::min({machine->ppu.cycles_until_next_event(),
                        machine->bus.audio().cycles_until_next_sample(),
                        device_io.cycles_until_next_timer_event(),device_io.cycles_until_next_sio_event()});
                    deadline=std::min(deadline,cycle_+distance);
                }
                runtime_session_begin_slice(deadline);
            } else runtime_session_begin_instruction();
            runtime_set_return_yield(native_slices_ && generated && return_yields_);
            if (profile) ++dispatches;
            try { runtime_dispatch(g_cpu.R[15]); }
            catch (const RuntimeDispatchYield&) { /* all guest residue is explicit */ }
            catch (...) { capture(instance); throw; }
            capture(instance);
            instance.timing.cycles += io.take_dma_steal_cycles();
        }
        if (profile) cpu_time += ProfileClock::now() - cpu_start;
        const auto schedule_start = profile ? ProfileClock::now() : ProfileClock::time_point{};
        std::uint64_t next = target;
        for (auto& ptr : machines_) {
            auto& instance = *ptr;
            auto& io = instance.bus.io();
            if (!io.halted()) next = std::min(next, instance.timing.cycles);
            const auto event = std::min({instance.ppu.cycles_until_next_event(),
                instance.bus.audio().cycles_until_next_sample(),
                io.cycles_until_next_timer_event(), io.cycles_until_next_sio_event()});
            next = std::min(next, cycle_ + event);
        }
        if (profile) schedule_time += ProfileClock::now() - schedule_start;
        if (next == cycle_) {
            // An IRQ entry/return consumed no emulated cycles; the next
            // iteration executes its continuation at this same timestamp.
            // No arbitrary host-time timeout changes guest behavior.
            if (++zero_cycle_passes > 1024)
                throw std::logic_error("GBA dispatch made no cycle progress");
            continue;
        }
        zero_cycle_passes = 0;
        if (next < cycle_) throw std::logic_error("GBA session lost cycle debt");
        const auto delta = static_cast<std::uint32_t>(next - cycle_);
        const auto device_start = profile ? ProfileClock::now() : ProfileClock::time_point{};
        for (auto& ptr : machines_) {
            auto& instance = *ptr;
            bind(instance);
            runtime_session_tick_devices(delta);
            instance.bus.rtc().advance_emulated_clock(delta);
            capture(instance);
            instance.timing.cycles += instance.bus.io().take_dma_steal_cycles();
        }
        if (profile) {
            device_time += ProfileClock::now() - device_start;
            ++passes;
        }
        cycle_ = next;
    }
    if (profile) {
        const auto ms = [](auto duration) {
            return std::chrono::duration<double, std::milli>(duration).count();
        };
        std::fprintf(stderr, "[session:profile] machines=%zu frame=%llu total_ms=%.3f cpu_ms=%.3f schedule_ms=%.3f devices_ms=%.3f dispatches=%llu passes=%llu\n",
            machines_.size(), static_cast<unsigned long long>(profile_frames), ms(ProfileClock::now() - profile_start),
            ms(cpu_time), ms(schedule_time), ms(device_time),
            static_cast<unsigned long long>(dispatches), static_cast<unsigned long long>(passes));
    }
}

std::vector<std::uint8_t> GbaMultiplayerSession::save_state() const {
    if (runtime_has_resumable_context()) throw std::logic_error("snapshot inside native dispatch");
    gba::SimulationArchive<false> a;
    manifest(a, config_);
    a(cycle_);
    for (const auto& ptr : machines_) {
        const auto& m = *ptr;
        execution_state(a,m.execution);
        a(m.timing.cycles,m.timing.vblank_starts);
        auto bytes = gba::save_device_state(m.bus,m.ppu);
        a.vector(bytes,4*1024*1024);
    }
    auto link = link_state();
    a.vector(link,1024*1024);
    return a.take();
}
bool GbaMultiplayerSession::load_state(std::span<const std::uint8_t> bytes, std::string* error) {
    if (runtime_has_resumable_context()) {
        if (error) *error = "snapshot load inside native dispatch";
        return false;
    }
    try {
        gba::SimulationArchive<true> a(bytes);
        manifest(a,config_);
        auto staged = std::make_unique<GbaMultiplayerSession>(config_);
        a(staged->cycle_);
        for (std::size_t i = 0; i < machines_.size(); ++i) {
            auto& to = *staged->machines_[i];
            const auto& from = *machines_[i];
            to.bus.set_bios(from.bus.bios());
            if (from.bus.rom_ptr()) to.bus.set_rom(from.bus.rom_ptr(),from.bus.rom_size());
            to.execution = from.execution; // retain trusted program callbacks
            execution_state(a,to.execution);
            a(to.timing.cycles,to.timing.vblank_starts);
            std::vector<std::uint8_t> device;
            a.vector(device,4*1024*1024);
            if (!gba::load_device_state(to.bus,to.ppu,device))
                throw std::invalid_argument("invalid GBA device state");
            if (!to.bus.io().halted() && to.timing.cycles < staged->cycle_)
                throw std::invalid_argument("CPU is behind session timeline");
        }
        std::vector<std::uint8_t> link;
        a.vector(link,1024*1024);
        if (a.remaining() || !(staged->wireless_ ? staged->wireless_->load_state(link,error) : staged->cable_->load_state(link,error)))
            throw std::invalid_argument("invalid session link state");
        for (std::size_t i = 0; i < staged->machines_.size(); ++i)
            if ((staged->wireless_ ? staged->wireless_->cycle(i) : staged->cable_->cycle(i)) != staged->cycle_)
                throw std::invalid_argument("link and session clocks differ");
        machines_.swap(staged->machines_);
        cable_.swap(staged->cable_);
        wireless_.swap(staged->wireless_);
        cycle_ = staged->cycle_;
        // Host mirrors survive rollback, but their cached pixels do not.
        // Rewire only after the complete replacement has passed validation.
        for (std::size_t i = 0; i < machines_.size(); ++i) {
            auto* observer = staged->machines_[i]->ppu.presentation_observer();
            machines_[i]->ppu.set_presentation_observer(observer);
            if (observer) observer->restored();
        }
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}
std::uint32_t GbaMultiplayerSession::state_digest(std::span<const std::uint8_t> bytes) {
    std::uint32_t hash = 2166136261u;
    for (auto byte : bytes) hash = (hash ^ byte) * 16777619u;
    return hash;
}
std::uint32_t GbaMultiplayerSession::state_hash() const {
    return state_digest(save_state());
}
std::array<std::uint32_t,3> GbaMultiplayerSession::state_hash_parts() const {
    auto hash = [](std::span<const std::uint8_t> bytes) {
        std::uint32_t result = 2166136261u;
        for (auto b : bytes) result = (result ^ b) * 16777619u;
        return result;
    };
    std::array<std::uint32_t,3> parts{};
    gba::SimulationArchive<false> first, rest, link;
    for (std::size_t i = 0; i < machines_.size(); ++i) {
        auto& a = i ? rest : first;
        const auto& m = *machines_[i];
        execution_state(a,m.execution);
        a(m.timing.cycles,m.timing.vblank_starts);
        auto bytes = gba::save_device_state(m.bus,m.ppu);
        a.vector(bytes,4*1024*1024);
    }
    manifest(link,config_); link(cycle_);
    auto bytes = link_state(); link.vector(bytes,1024*1024);
    parts[0] = hash(first.bytes()); parts[1] = hash(rest.bytes()); parts[2] = hash(link.bytes());
    return parts;
}
void GbaMultiplayerSession::discard_audio_output() {
    for (auto& m : machines_) m->bus.audio().discard_output();
}
} // namespace gbarecomp
