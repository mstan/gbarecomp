#include "gba_audio.h"
#include "gba_io.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" unsigned long long g_runtime_cycles = 123456ull;
extern "C" uint32_t runtime_current_pc(void) { return 0x08000100u; }

namespace {

int failures = 0;

void check(bool condition, const char* name) {
    if (!condition) {
        std::printf("FAIL: %s\n", name);
        ++failures;
    }
}

void set_env_value(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    if (value && *value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

bool env_enabled_arg(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--enabled") == 0) return true;
    if (argc == 2 && std::strcmp(argv[1], "--disabled") == 0) return false;
    std::printf("usage: diagnostic_capture_tests (--enabled|--disabled)\n");
    std::exit(2);
}

void test_mmio_capture(bool enabled) {
    set_env_value("GBARECOMP_MMIO_CAP", enabled ? "1" : "");
    set_env_value("GBARECOMP_MMIO_DUMP", "");
    check(gba::gba_mmio_cap_enabled() == enabled, "mmio enabled state");

    gba::GbaIo io;
    io.write16(gba::IoReg::WAITCNT, 0x1234u);
    check(io.read16(gba::IoReg::WAITCNT) == 0x1234u,
          "mmio guest write still commits");

    gba::MmioCapEntry entry{};
    uint64_t first = 99;
    const std::size_t n = gba::gba_mmio_cap_query(0, 1, &entry, first);
    if (enabled) {
        check(gba::gba_mmio_cap_total() == 1, "mmio enabled total");
        check(n == 1, "mmio enabled query count");
        check(first == 0, "mmio enabled first");
        check(entry.cycle == g_runtime_cycles, "mmio cycle stamp");
        check(entry.addr == 0x04000000u + gba::IoReg::WAITCNT,
              "mmio addr stamp");
        check(entry.value == 0x1234u, "mmio value stamp");
        check(entry.size == 2, "mmio size stamp");
        check(entry.pc == runtime_current_pc(), "mmio pc stamp");
    } else {
        check(gba::gba_mmio_cap_total() == 0, "mmio disabled total");
        check(n == 0, "mmio disabled query count");
    }
}

void test_fifo_trace(bool enabled) {
    set_env_value("GBARECOMP_AUDIO_FIFO_TRACE", enabled ? "1" : "");
    check(gba::GbaAudio::debug_fifo_trace_enabled() == enabled,
          "fifo enabled state");

    gba::GbaAudio audio;
    audio.write_io32(0x0A0u, 0x04030201u);
    audio.timer_overflow(0);
    const auto fifo = audio.debug_fifo_state(0);
    check(fifo.count == 0, "fifo guest count after timer step");
    check(fifo.bytes_remaining == 3, "fifo guest shift state");
    check(fifo.samples[1] == 1, "fifo guest sample output");

    if (enabled) {
        check(audio.debug_trace_count() == 2, "fifo enabled trace count");
        const auto tr = audio.debug_trace_entry(0);
        check(tr.fifo_id == 0, "fifo trace id");
        check(tr.sample == 1, "fifo trace sample");
        const auto tr_b = audio.debug_trace_entry(1);
        check(tr_b.fifo_id == 1, "fifo trace B id");
        check(tr_b.sample == 0, "fifo trace B idle sample");

        gba::GbaAudio copied = audio;
        audio.reset();
        check(audio.debug_trace_count() == 0, "fifo reset clears trace count");
        check(copied.debug_trace_count() == 2, "fifo copied trace count");
        check(copied.debug_trace_entry(0).sample == 1,
              "fifo copied trace sample");
    } else {
        check(audio.debug_trace_count() == 0, "fifo disabled trace count");
    }
}

}  // namespace

int main(int argc, char** argv) {
    const bool enabled = env_enabled_arg(argc, argv);
    test_mmio_capture(enabled);
    test_fifo_trace(enabled);
    if (failures) {
        std::printf("diagnostic_capture_tests: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("diagnostic_capture_tests: %s PASS\n",
                enabled ? "enabled" : "disabled");
    return 0;
}
