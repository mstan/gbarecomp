#include "multiplayer_session.h"
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); std::exit(1); } } while (0)
namespace {
void return_yield_continuation() {
    using namespace gbarecomp;
    RuntimeArmContext original, guest;
    RuntimeTimingContext timing;
    runtime_capture_arm_context(original); runtime_capture_timing_context(timing);
    guest.cpu.cpsr=0xd3;
    guest.cpu.R[15]=0x08000000;
    guest.returns={0x08000004,0x08000008};
    runtime_restore_arm_context(guest); runtime_set_resumable_context(&guest);
    runtime_session_execution(true); runtime_session_begin_instruction();
    runtime_set_return_yield(true);
    CHECK(!runtime_should_yield());
    runtime_tick(1); g_cpu.R[15]=0x08000004;
    const auto cycles=g_runtime_cycles;
    CHECK(runtime_should_yield());
    // Nested generated callers cancel only their native frames on a suspended
    // return. A matching caller return PC must not accidentally resume it.
    runtime_call_cancel_return(0x08000008);
    runtime_call_cancel_return(0x08000004);
    CHECK(runtime_call_stack_depth()==2);
    g_cpu.R[15]=0x08000100; // another caller's instruction prologue
    CHECK(runtime_should_yield());
    CHECK(g_cpu.R[15]==0x08000004 && g_runtime_cycles==cycles);
    // Neither fall-through dispatch nor its custom RAM callback may execute.
    guest.ram_dispatch=[](std::uint32_t,int) { CHECK(false); return 0; };
    g_runtime_ram_dispatch_hook=guest.ram_dispatch;
    runtime_dispatch(0x03000000);
    CHECK(g_cpu.R[15]==0x08000004 && runtime_call_stack_depth()==2);
    runtime_set_return_yield(false); // next dispatch consumes real guest returns
    CHECK(runtime_call_should_return(0x08000008));
    CHECK(runtime_call_should_return(0x08000004));
    runtime_session_execution(false); runtime_set_resumable_context(nullptr);
    runtime_restore_arm_context(original); runtime_restore_timing_context(timing);
}
gba::GbaLinkHub* callback_cable=nullptr;
unsigned callback_visits=0;
int ram_callback(std::uint32_t pc,int) {
    CHECK(pc==0x03000000);
    // Validation reads are already a callback side effect: the scheduler must
    // reach this boundary before entering it, not wait for its first prologue.
    CHECK(g_runtime_cycles==3);
    CHECK(callback_cable->cycle(0)==3 && callback_cable->cycle(1)==3);
    ++callback_visits;
    CHECK(!runtime_should_yield());
    runtime_tick(1); g_cpu.R[15]=0x08000004;
    return 1;
}
int ram_caller(std::uint32_t pc,int) {
    if (pc==0x08000000) {
        CHECK(!runtime_should_yield());
        runtime_tick(3);
        runtime_dispatch(0x03000000);
        return 1;
    }
    if (pc==0x08000004) {
        CHECK(!runtime_should_yield());
        runtime_tick(1); bus_write_u8(0x04000301,0); return 1;
    }
    return 0;
}
// Small native instruction fixture obeying the generated ABI. It performs an
// actual MMIO cable exchange and stores the result into each machine's RAM.
int program(std::uint32_t pc, int) {
    if (pc < 0x08000000 || pc > 0x08000018) return 0;
    if (runtime_should_yield()) return 1;
    const auto instruction = (pc - 0x08000000) / 4;
    g_cpu.R[15] = pc + 4;
    switch (instruction) {
    case 0: bus_write_u16(0x04000134, 0); break;
    case 1: bus_write_u16(0x04000128, 0x2003); break;
    case 2: bus_write_u16(0x0400012a, static_cast<std::uint16_t>(g_cpu.R[0])); break;
    case 3: if (g_cpu.R[0] == 0x1111) bus_write_u16(0x04000128, 0x2083); break;
    case 4:
        if (bus_read_u16(0x04000128) & 0x80) g_cpu.R[15] = pc;
        break;
    case 5: bus_write_u32(0x02000000, bus_read_u32(0x04000120)); break;
    case 6: bus_write_u8(0x04000301, 0); g_cpu.R[15] = pc; break;
    }
    runtime_tick(1);
    return 1;
}
gbarecomp::GbaSessionConfig config() {
    using namespace gbarecomp;
    GbaSessionConfig c;
    c.machines = {{10,"fixture",std::string(40,'a')}, {30,"fixture",std::string(40,'a')}};
    c.input_machines = {30,10}; // seats and cable ports are independent
    c.links = {{GbaLinkMedium::Cable,{10,30}}};
    return c;
}
void ram_dispatch_rendezvous() {
    using namespace gbarecomp;
    auto session=std::make_unique<GbaMultiplayerSession>(config());
    callback_cable=&session->cable(); callback_visits=0;
    for (auto id:{10,30}) {
        auto& execution=session->machine(id).execution;
        execution.cpu.R[15]=0x08000000;
        execution.program_dispatch=ram_caller;
        execution.ram_dispatch=ram_callback;
    }
    session->run_until(3);
    CHECK(callback_visits==0);
    const auto before=session->save_state();
    session->run_until(8); CHECK(callback_visits==2);
    const auto after=session->save_state();
    std::string error; CHECK(session->load_state(before,&error));
    callback_cable=&session->cable(); callback_visits=0;
    session->set_native_slices(true);
    session->run_until(8); CHECK(callback_visits==2);
    CHECK(session->save_state()==after);
    callback_cable=nullptr;
}
void wireless_session_restore() {
    using namespace gbarecomp;
    auto cfg=config(); cfg.links[0].medium=GbaLinkMedium::Wireless;
    auto session=std::make_unique<GbaMultiplayerSession>(cfg);
    for (auto id:{10,30}) {
        auto& io=session->machine(id).bus.io();
        io.write8(0x301,0); // HALT: only the shared deterministic devices run.
        io.write16(0x134,0);
        io.write32(0x120,0x7fff494e);
        io.write16(0x128,0x5081);
    }
    session->run_until(1024);
    CHECK(session->machine(10).bus.io().read16(0x128)&0x80);
    const auto mid=session->save_state();
    session->run_until(2048);
    for (auto id:{10,30}) {
        auto& io=session->machine(id).bus.io();
        CHECK(!(io.read16(0x128)&0x80));
        CHECK(io.read16(0x202)&0x80);
        CHECK(io.read32(0x120)==0);
    }
    const auto expected=session->save_state();
    std::string error;
    CHECK(session->load_state(mid,&error));
    session->run_until(2048);
    CHECK(session->save_state()==expected);
    auto cable=std::make_unique<GbaMultiplayerSession>(config());
    const auto before=cable->save_state();
    CHECK(!cable->load_state(expected,&error));
    CHECK(cable->save_state()==before);
    CHECK(!session->load_state(before,&error));
    CHECK(session->save_state()==expected);
}
void native_exchange() {
    using namespace gbarecomp;
    g_cpu.R[4] = 0xabcdef01;
    auto session = std::make_unique<GbaMultiplayerSession>(config());
    auto& a = session->machine(10);
    auto& b = session->machine(30);
    for (auto* m : {&a,&b}) {
        m->execution.cpu.R[15] = 0x08000000;
        m->execution.program_dispatch = program;
    }
    a.execution.cpu.R[0] = 0x1111;
    b.execution.cpu.R[0] = 0x2222;
    a.bus.write32(0x02000100, 0x55555555);
    b.bus.write32(0x02000100, 0xaaaaaaaa);
    session->run_until(100);
    CHECK(session->cable().transfer_active());
    CHECK(a.bus.read32(0x02000000) == 0);
    const auto baseline = session->save_state();
    const auto baseline_hash = session->state_hash();
    std::printf("two-machine fixture snapshot: %zu bytes\n", baseline.size());
    std::string error;
    auto bad = baseline;
    bad[0] ^= 1;
    CHECK(!session->load_state(bad,&error));
    CHECK(session->state_hash() == baseline_hash);
    for (auto size : {std::size_t{0}, baseline.size()/2, baseline.size()-1}) {
        CHECK(!session->load_state(std::span(baseline.data(),size),&error));
        CHECK(session->state_hash() == baseline_hash);
    }
    session->run_until(6000);
    CHECK(!session->cable().transfer_active());
    CHECK(a.bus.read32(0x02000000) == 0x22221111);
    CHECK(b.bus.read32(0x02000000) == 0x22221111);
    CHECK(a.bus.read32(0x02000100) == 0x55555555);
    CHECK(b.bus.read32(0x02000100) == 0xaaaaaaaa);
    CHECK(g_cpu.R[4] == 0xabcdef01);
    CHECK(!runtime_has_resumable_context());
    CHECK(session->cable().cycle(0) == session->cycle());
    CHECK(session->cable().cycle(1) == session->cycle());
    const auto expected = session->save_state();
    CHECK(session->load_state(baseline,&error));
    CHECK(session->state_hash() == baseline_hash);
    session->run_until(6000);
    CHECK(session->save_state() == expected);
    const std::uint16_t inputs[] = {1, 2};
    session->run_frame(inputs);
    CHECK((session->machine(10).bus.io().read16(0x130) & 0x3ff) == 0x3fd);
    CHECK((session->machine(30).bus.io().read16(0x130) & 0x3ff) == 0x3fe);
}
// Same exchange with every SIOMULTI word captured (ports 2/3 included).
int program_all(std::uint32_t pc, int) {
    if (pc < 0x08000000 || pc > 0x0800001c) return 0;
    if (runtime_should_yield()) return 1;
    const auto instruction = (pc - 0x08000000) / 4;
    g_cpu.R[15] = pc + 4;
    switch (instruction) {
    case 0: bus_write_u16(0x04000134, 0); break;
    case 1: bus_write_u16(0x04000128, 0x2003); break;
    case 2: bus_write_u16(0x0400012a, static_cast<std::uint16_t>(g_cpu.R[0])); break;
    case 3: if (g_cpu.R[0] == 0x1111) bus_write_u16(0x04000128, 0x2083); break;
    case 4:
        if (bus_read_u16(0x04000128) & 0x80) g_cpu.R[15] = pc;
        break;
    case 5: bus_write_u32(0x02000000, bus_read_u32(0x04000120)); break;
    case 6: bus_write_u32(0x02000004, bus_read_u32(0x04000124)); break;
    case 7: bus_write_u8(0x04000301, 0); g_cpu.R[15] = pc; break;
    }
    runtime_tick(1);
    return 1;
}
// Three- and four-console cables: stable non-contiguous machine IDs, seats in
// a different order from the ports, all words delivered to every console,
// the 3/4-player 115200-baud durations, and exact snapshot replay.
void native_exchange_n(unsigned players) {
    using namespace gbarecomp;
    GbaSessionConfig c;
    std::vector<GbaMachineId> ports;
    for (unsigned port = 0; port < players; ++port) {
        c.machines.push_back({10*(port+1),"fixture",std::string(40,'a')});
        ports.push_back(10*(port+1));
    }
    for (unsigned seat = 0; seat < players; ++seat) c.input_machines.push_back(10*(players-seat));
    c.links = {{GbaLinkMedium::Cable,ports}};
    auto session = std::make_unique<GbaMultiplayerSession>(c);
    CHECK(session->machine_count() == players && session->cable().port_count() == players);
    for (unsigned port = 0; port < players; ++port) {
        auto& m = session->machine(ports[port]);
        m.execution.cpu.R[15] = 0x08000000;
        m.execution.program_dispatch = program_all;
        m.execution.cpu.R[0] = (port+1)*0x1111;
    }
    session->run_until(100);
    CHECK(session->cable().transfer_active());
    const auto started = session->cable().completion_cycle() - (players == 3 ? 8376u : 10486u);
    const auto baseline = session->save_state();
    std::printf("%u-machine fixture snapshot: %zu bytes\n", players, baseline.size());
    session->run_until(started + (players == 3 ? 8376u : 10486u) - 1);
    CHECK(session->cable().transfer_active());
    session->run_until(15000);
    CHECK(!session->cable().transfer_active());
    const std::uint32_t high = players == 4 ? 0x44443333u : 0xffff3333u;
    for (unsigned port = 0; port < players; ++port) {
        auto& bus = session->machine(ports[port]).bus;
        CHECK(bus.read32(0x02000000) == 0x22221111);
        CHECK(bus.read32(0x02000004) == high);
        CHECK(session->cable().cycle(port) == session->cycle());
    }
    const auto expected = session->save_state();
    std::string error;
    CHECK(session->load_state(baseline,&error));
    session->set_native_slices(true);
    session->run_until(15000);
    CHECK(session->save_state() == expected);
    // Seat s drives machine input_machines[s]; a short row is refused.
    std::vector<std::uint16_t> inputs;
    for (unsigned seat = 0; seat < players; ++seat) inputs.push_back(static_cast<std::uint16_t>(1u << seat));
    session->run_frame(inputs);
    for (unsigned seat = 0; seat < players; ++seat)
        CHECK((session->input_machine(seat).bus.io().read16(0x130) & 0x3ff) == (0x3ff & ~(1u << seat)));
    CHECK(session->input_machine(0).descriptor.id == 10*players);
    inputs.pop_back();
    bool refused = false;
    try { session->run_frame(inputs); } catch (const std::invalid_argument&) { refused = true; }
    CHECK(refused);
    // A snapshot names its exact wiring: another cable size cannot load it.
    auto other = c; other.machines.pop_back(); other.input_machines.clear(); other.links[0].machines.pop_back();
    for (const auto& m : other.machines) other.input_machines.push_back(m.id);
    auto smaller = std::make_unique<GbaMultiplayerSession>(other);
    const auto untouched = smaller->save_state();
    CHECK(!smaller->load_state(expected,&error));
    CHECK(smaller->save_state() == untouched);
}
void exception_continuations(bool return_yield) {
    using namespace gbarecomp;
    const auto suspend=[&](auto operation) {
        runtime_set_return_yield(return_yield);
        if (return_yield) {
            operation(); CHECK(runtime_dispatch_suspended());
            runtime_set_return_yield(false);
        } else {
            try { operation(); CHECK(false); } catch (const RuntimeDispatchYield&) {}
        }
    };
    RuntimeArmContext original;
    runtime_capture_arm_context(original);
    RuntimeArmContext a, b;
    a.cpu.cpsr = 0x1f;
    a.cpu.R[0] = 123;
    a.cpu.R[12] = 456;
    a.returns = {0x08001234};
    runtime_restore_arm_context(a);
    runtime_set_resumable_context(&a);
    suspend([] { runtime_irq(0x08000040); });
    CHECK(g_cpu.R[15] == 0x18);
    CHECK(a.interrupts.size() == 1);
    runtime_capture_arm_context(a);
    b.cpu.cpsr = 0x1f;
    b.cpu.R[0] = 999;
    runtime_restore_arm_context(b);
    runtime_set_resumable_context(&b);
    CHECK(g_cpu.R[0] == 999 && runtime_call_stack_depth() == 0);
    runtime_restore_arm_context(a);
    runtime_set_resumable_context(&a);
    CHECK(g_cpu.R[0] == 123 && runtime_call_stack_depth() == 1);
    g_cpu.R[0] = 777;
    const auto outer_spsr = g_cpu.banked_spsr[ARM_BANK_IRQ];
    // Model a nested IRQ after the handler unmasks IRQs.
    g_cpu.cpsr &= ~CPSR_I_BIT;
    suspend([] { runtime_irq(0x00000140); });
    CHECK(a.interrupts.size() == 2);
    g_cpu.R[0] = 888;
    suspend([] { runtime_exception_return(0x140); });
    CHECK(g_cpu.R[0] == 777 && a.interrupts.size() == 1);
    // A real nested IRQ wrapper saves/restores this banked SPSR in guest RAM.
    g_cpu.banked_spsr[ARM_BANK_IRQ] = outer_spsr;
    suspend([] { runtime_exception_return(0x08000040); });
    CHECK(g_cpu.R[0] == 123 && g_cpu.R[12] == 456);
    CHECK(g_cpu.R[15] == 0x08000040 && a.interrupts.empty());
    CHECK(g_cpu.cpsr == 0x1f);
    runtime_set_resumable_context(nullptr);
    runtime_restore_arm_context(original);
}
}
int main() {
    return_yield_continuation();
    exception_continuations(false); exception_continuations(true);
    native_exchange(); native_exchange_n(3); native_exchange_n(4); ram_dispatch_rendezvous();
    wireless_session_restore();
    std::puts("native multiplayer session tests passed");
}
