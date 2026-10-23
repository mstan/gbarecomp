// Explicit host-cost probe; never launches a window or writes cartridge saves.
// Allocation counts cover ordinary C++ new/new[] on this single simulation thread.
#include "../../tests/link/netplay_fixture.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <new>
#include <string>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {
bool counting = false;
std::uint64_t allocations = 0, allocated_bytes = 0, sink = 0;
std::uint64_t thread_cycles() {
#if defined(_WIN32)
    ULONG64 cycles = 0;
    if (!QueryThreadCycleTime(GetCurrentThread(), &cycles)) std::abort();
    return cycles;
#else
    return 0; // CPU milliseconds remain available on every supported host.
#endif
}
double thread_cpu_ms() {
#if defined(_WIN32)
    FILETIME created{}, exited{}, kernel_time{}, user_time{};
    if (!GetThreadTimes(GetCurrentThread(),&created,&exited,&kernel_time,&user_time)) std::abort();
    const auto ticks=[](const FILETIME& t) {
        return (std::uint64_t(t.dwHighDateTime)<<32) | t.dwLowDateTime;
    };
    return (ticks(kernel_time)+ticks(user_time))/10000.0;
#else
    return 1000.0*std::clock()/CLOCKS_PER_SEC;
#endif
}
void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "%s\n", what); std::exit(1); }
}
template<class Work> void measure(const char* name, unsigned iterations, Work work) {
    work(); // warm code, pages and allocator outside the measured interval
    allocations = allocated_bytes = 0;
    const auto cpu = thread_cpu_ms();
    const auto cycles = thread_cycles();
    const auto start = std::chrono::steady_clock::now();
    counting = true;
    for (unsigned i = 0; i < iterations; ++i) work();
    counting = false;
    const auto wall = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    const auto used_cycles = thread_cycles() - cycles;
    const auto cpu_ms = thread_cpu_ms() - cpu;
    std::printf("{\"phase\":\"%s\",\"iterations\":%u,\"wall_ms\":%.6f,"
        "\"cpu_ms\":%.6f,\"thread_cycles\":%llu,\"allocations\":%llu,\"allocated_bytes\":%llu}\n",
        name, iterations, wall, cpu_ms, (unsigned long long)used_cycles,
        (unsigned long long)allocations, (unsigned long long)allocated_bytes);
}
void configure(gbarecomp::GbaMultiplayerSession& session) {
    for (unsigned seat = 0; seat < session.input_count(); ++seat) {
        auto& m = session.machine(seat);
        for (unsigned i = 0; i < 0x18000; ++i) m.bus.vram_ptr()[i] = (i * 37 + i / 19) & 255;
        for (unsigned i = 0; i < 0x400; ++i) {
            m.bus.pal_ptr()[i] = (i * 13 + seat) & 255;
            m.bus.oam_ptr()[i] = (i * 7 + seat) & 255;
        }
        m.bus.write16(0x04000000, 0x1740); // text BGs + OBJ, 1D tiles
        m.bus.write16(0x04000008, 0x1c00);
        m.bus.write16(0x0400000a, 0x1d81);
        m.bus.write16(0x0400000c, 0x1e02);
        m.bus.write16(0x04000082, 0x000e);
        m.bus.write16(0x04000084, 0x0080);
        m.bus.write16(0x04000080, 0x2277);
        m.bus.write16(0x04000068, 0xf080);
        m.bus.write16(0x0400006c, 0x8400);
        m.bus.write32(0x040000a0, 0x7f408000); // real sound FIFO contents
    }
}
void emit(std::ofstream& file, std::span<const std::uint8_t> bytes) {
    const auto size = static_cast<std::uint32_t>(bytes.size());
    for (unsigned i = 0; i < 4; ++i) file.put(char(size >> (8 * i)));
    file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
void qualify(const char* path, unsigned players) {
    auto session = link_fixture::create(true, false, players);
    configure(*session);
    std::ofstream file(path, std::ios::binary);
    check(bool(file), "cannot open explicit qualification output");
    std::string error;
    const auto reset = session->save_state();
    std::array<std::uint16_t,4> keys{1,2,4,8};
    std::vector<std::vector<std::uint8_t>> history;
    for (unsigned frame = 0; frame < 24; ++frame) {
        for (unsigned seat = 0; seat < players; ++seat) {
            auto& m = session->machine(seat);
            m.bus.write16(0x04000000, 0x1f40 | (frame % 6) | ((frame % 8) << 13));
            m.bus.write16(0x04000008, 0x1c40);
            m.bus.write16(0x04000040, 0x20b0);
            m.bus.write16(0x04000042, 0x1070);
            m.bus.write16(0x04000044, 0x00a0);
            m.bus.write16(0x04000046, 0x2080);
            m.bus.write16(0x04000048, 0x373f);
            m.bus.write16(0x0400004a, 0x2f15);
            m.bus.write16(0x04000020, 0x100 + frame * 3);
            m.bus.write16(0x04000022, frame * 11);
            m.bus.write16(0x04000024, frame * 5);
            m.bus.write16(0x04000026, 0x100 - frame);
            m.bus.write16(0x04000028, frame * 0x171);
            m.bus.write16(0x0400002c, frame * 0x83);
            m.bus.write16(0x04000050, (frame % 4) * 0x40 | 0x3f3f);
            m.bus.write16(0x04000052, 0x0808);
            m.bus.write16(0x04000054, frame % 17);
            m.bus.write16(0x0400004c, frame * 0x101);
            // Direct native/mod writes bypass ordinary bus hooks.
            m.bus.vram_ptr()[(frame * 131 + seat) % 0x18000] ^= 0xa5;
            m.bus.oam_ptr()[(frame * 17) % 0x400] ^= 0x10;
            m.bus.pal_ptr()[(frame * 7) % 0x400] ^= 0x1f;
        }
        // Snapshot at a changing mid-frame phase, then restore/replay exactly.
        session->run_until(session->cycle() + 1031 + frame * 97);
        const auto before = session->save_state();
        session->discard_audio_output();
        session->run_frame(std::span(keys.data(), players));
        const auto expected = session->save_state();
        std::array<std::vector<std::int16_t>,4> audio;
        for (unsigned seat = 0; seat < players; ++seat) {
            std::array<std::int16_t,8192> samples{};
            auto& apu = session->machine(seat).bus.audio();
            const auto count = apu.drain_samples(samples.data(), samples.size());
            audio[seat].assign(samples.begin(),samples.begin()+count);
        }
        check(session->load_state(before, &error), error.c_str());
        session->discard_audio_output();
        session->run_frame(std::span(keys.data(), players));
        check(session->save_state() == expected, "mid-frame restore replay diverged");
        emit(file, expected);
        for (unsigned seat = 0; seat < players; ++seat) {
            auto& m = session->machine(seat);
            std::array<std::int16_t,8192> samples{};
            const auto count = m.bus.audio().drain_samples(samples.data(), samples.size());
            check(std::vector<std::int16_t>(samples.begin(),samples.begin()+count) == audio[seat],
                "audio changed after mid-frame restore");
            emit(file, std::span(reinterpret_cast<const std::uint8_t*>(samples.data()), count * sizeof(samples[0])));
            std::array<std::uint8_t,240*160*3> image{};
            const auto stable = session->save_state();
            for (unsigned repeat = 0; repeat < 3; ++repeat)
                m.ppu.render(image.data(), m.bus.io().read16(0), m.bus.io().raw(),
                    m.bus.vram_ptr(), m.bus.oam_ptr(), m.bus.pal_ptr());
            check(session->save_state() == stable, "repeated presentation mutated guest state");
            emit(file, image);
        }
        history.push_back(expected);
        if (history.size() > 3) {
            const auto current = session->save_state();
            check(session->load_state(history[history.size()-3], &error), "rewind restore failed");
            check(session->save_state() == history[history.size()-3], "rewind bytes changed");
            check(session->load_state(current, &error), "rewind forward restore failed");
        }
        const auto stable = session->save_state();
        for (auto length : {std::size_t(0), std::size_t(11), stable.size()/2, stable.size()-1}) {
            check(!session->load_state(std::span(stable.data(),length), &error), "truncated state accepted");
            check(session->save_state() == stable, "invalid state changed live session");
        }
        auto invalid = stable; invalid[0] ^= 1;
        check(!session->load_state(invalid, &error), "bad identity accepted");
        check(session->save_state() == stable, "bad identity changed session");
    }
    check(session->load_state(reset, &error), "cold restore failed");
    check(session->save_state() == reset, "cold restore bytes differ");
    emit(file, reset);
    check(bool(file), "qualification output failed");
    std::printf("qualification passed players=%u frames=24 modes=0..5 mid-frame/direct-write/audio/re-present/rewind/invalid-input\n", players);
}
void raster(unsigned count) {
    auto session = link_fixture::create(true);
    configure(*session);
    auto& m = session->machine(0);
    std::array<std::uint8_t,240*160*3> image{};
    struct Case { const char* name; unsigned mode, effect, window, mosaic; };
    const Case cases[] = {{"text_obj",0,0,0,0},{"affine_obj",2,0,0,0},
        {"bitmap3_obj",3,0,0,0},{"bitmap4_obj",4,0,0,0},{"bitmap5_obj",5,0,0,0},
        {"text_obj_blend",0,1,0,0},{"text_obj_windows",0,0,1,0},
        {"text_obj_mosaic_registers",0,0,0,1}};
    for (const auto& c : cases) {
        m.bus.write16(0x04000000,0x1f40 | c.mode | (c.window ? 0xe000 : 0));
        m.bus.write16(0x04000008,0x1c00 | (c.mosaic ? 0x40 : 0));
        m.bus.write16(0x0400000c,0x3e00);
        m.bus.write16(0x04000020,0x100); m.bus.write16(0x04000026,0x100);
        m.bus.write16(0x04000040,0x20b0); m.bus.write16(0x04000044,0x00a0);
        m.bus.write16(0x04000048,0x3f3f); m.bus.write16(0x0400004a,0x152f);
        m.bus.write16(0x04000050,c.effect ? 0x3f7f : 0);
        m.bus.write16(0x04000052,0x0808); m.bus.write16(0x0400004c,c.mosaic ? 0x3333 : 0);
        measure(c.name,count,[&] {
            m.ppu.render(image.data(),m.bus.io().read16(0),m.bus.io().raw(),
                m.bus.vram_ptr(),m.bus.oam_ptr(),m.bus.pal_ptr());
            sink ^= image[123];
        });
    }
    measure("audio_tick_1024",count*1000,[&] { m.bus.audio().tick(1024); });
}
}
void* operator new(std::size_t size) {
    if (counting) { ++allocations; allocated_bytes += size; }
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

int main(int argc, char** argv) {
    const unsigned players = argc > 3 ? std::stoul(argv[3]) : 2;
    check(players >= 2 && players <= 4, "players must be 2..4");
    if (argc > 2 && std::string(argv[1]) == "--qualify") { qualify(argv[2], players); return 0; }
    const unsigned count = argc > 2 ? std::stoul(argv[2]) : 128;
    check(count > 0 && count <= 10000, "iterations must be 1..10000");
    if (argc > 1 && std::string(argv[1]) == "--raster") { raster(count); return 0; }
    auto session = link_fixture::create(true, false, players);
    configure(*session);
    const std::array<std::uint16_t,4> keys{1,2,4,8};
    session->run_frame(std::span(keys.data(),players)); session->discard_audio_output();
    const auto state = session->save_state();
    std::printf("{\"snapshot_bytes\":%zu,\"players\":%u}\n", state.size(), players);
    measure("save", count, [&] { auto s=session->save_state(); sink ^= s.back(); });
    measure("digest", count, [&] { sink ^= session->state_digest(state); });
    measure("hash_parts", count, [&] { sink ^= session->state_hash_parts()[0]; });
    std::string error;
    measure("load", std::max(1u,count/8), [&] { check(session->load_state(state,&error),error.c_str()); });
    measure("run_frame", std::max(1u,count/8), [&] { session->run_frame(std::span(keys.data(),players)); session->discard_audio_output(); });
    std::printf("{\"sink\":%llu}\n", (unsigned long long)sink);
}
