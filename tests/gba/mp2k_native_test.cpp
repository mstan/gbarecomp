// Differential contract checks against the owner's original SDK instructions.
// No game/BIOS bytes are embedded. Supply the MKSC USA ROM explicitly.
#include "mp2k_native.h"
#include "arm_decode.h"
#include "thumb_decode.h"
#include "interpreter.h"
#include "cpu_state.h"
#include "bus.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <random>
#include <stdexcept>
#include <vector>

namespace {
constexpr uint32_t kInfo = 0x03000000, kStack = 0x03007000;
constexpr uint32_t kReturn = 0x08000000;

struct TestBus final : armv4t::Bus {
    std::vector<uint8_t> ewram = std::vector<uint8_t>(0x40000);
    std::vector<uint8_t> iwram = std::vector<uint8_t>(0x8000);
    std::vector<uint8_t>* rom = nullptr;
    bool bad = false;
    uint32_t access_cycles(uint32_t address, uint8_t width, bool) const override {
        if ((address >> 24) == 8) return width == 4 ? 18 : 9;
        if ((address >> 24) == 2) return width == 4 ? 6 : 3;
        return 1;
    }
    uint8_t* ptr(uint32_t address, unsigned bytes) {
        std::vector<uint8_t>* region = nullptr;
        unsigned offset = 0;
        switch (address >> 24) {
        case 2: region = &ewram; offset = address & 0x3ffff; break;
        case 3: region = &iwram; offset = address & 0x7fff; break;
        case 8: region = rom; offset = address & 0x1ffffff; break;
        default: bad = true; return nullptr;
        }
        if (!region || offset + bytes > region->size()) { bad = true; return nullptr; }
        return region->data() + offset;
    }
    uint8_t read8(uint32_t a) override {
        if (a == 0x04000006) return 200;
        auto* p = ptr(a, 1); return p ? *p : 0;
    }
    uint16_t read16(uint32_t a) override {
        auto* p = ptr(a, 2); return p ? uint16_t(p[0] | (p[1] << 8)) : 0;
    }
    uint32_t read32(uint32_t a) override {
        auto* p = ptr(a, 4);
        return p ? uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
            (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24) : 0;
    }
    void write8(uint32_t a, uint8_t value) override { auto* p = ptr(a, 1); if (p) *p = value; }
    void write16(uint32_t a, uint16_t value) override {
        auto* p = ptr(a, 2); if (p) { p[0] = uint8_t(value); p[1] = uint8_t(value >> 8); }
    }
    void write32(uint32_t a, uint32_t value) override {
        auto* p = ptr(a, 4); if (p) for (unsigned n = 0; n != 4; ++n) p[n] = uint8_t(value >> (8 * n));
    }
};

unsigned run_original(TestBus& bus, unsigned samples, unsigned counter, uint64_t* cycles) {
    armv4t::CPUState cpu{};
    cpu.cpsr.mode = static_cast<uint8_t>(armv4t::Mode::System);
    cpu.cpsr.t = cpu.thumb = true;
    cpu.R[0] = kInfo;
    cpu.R[4] = counter;
    cpu.R[5] = bus.read32(kStack + 8);
    cpu.R[6] = 0x630;
    cpu.R[8] = samples;
    cpu.R[13] = kStack;
    cpu.R[15] = 0x03002600;
    unsigned steps = 0;
    while (cpu.R[15] != kReturn) {
        const uint32_t pc = cpu.R[15];
        if (++steps > 1000000) throw std::runtime_error("original mixer instruction bound");
        const auto instruction = cpu.cpsr.t
            ? armv4t::ThumbDecoder::decode(bus.read16(pc), pc)
            : armv4t::ArmDecoder::decode(bus.read32(pc), pc);
        uint32_t used_cycles = 0;
        const auto result = armv4t::Interpreter::step(cpu, bus, instruction, &used_cycles);
        *cycles += used_cycles;
        if (result == armv4t::Interpreter::Result::Undefined ||
            result == armv4t::Interpreter::Result::NotImplemented ||
            result == armv4t::Interpreter::Result::Swi || bus.bad)
            throw std::runtime_error("original mixer invalid instruction/access");
    }
    if (cpu.R[13] != kStack + 64) throw std::runtime_error("original mixer ABI stack");
    return steps;
}

void require_equal(const TestBus& original, const TestBus& native, unsigned case_id,
                   uint32_t stack_address = kStack) {
    if (original.ewram != native.ewram) throw std::runtime_error("EWRAM differs");
    for (unsigned i = 0; i < original.iwram.size(); ++i) {
        // Dead guest stack scratch is not part of the native PCM contract.
        // Runtime adapter ABI/continuation is checked separately.
        if (i >= (stack_address & 0x7fff) - 8 && i < (stack_address & 0x7fff) + 64) continue;
        if (original.iwram[i] != native.iwram[i]) {
            std::fprintf(stderr, "case=%u address=%08x reference=%02x native=%02x\n",
                         case_id, 0x03000000 + i, original.iwram[i], native.iwram[i]);
            throw std::runtime_error("persistent IWRAM differs");
        }
    }
}

uint32_t raw32(const std::vector<uint8_t>& data, std::size_t offset) {
    if (offset + 4 > data.size()) throw std::runtime_error("short capture");
    return uint32_t(data[offset]) | (uint32_t(data[offset+1]) << 8) |
        (uint32_t(data[offset+2]) << 16) | (uint32_t(data[offset+3]) << 24);
}
std::vector<uint8_t> capture_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {(std::istreambuf_iterator<char>(file)), {}};
}
void decode_capture(const std::vector<uint8_t>& data, TestBus& bus) {
    if (raw32(data,0) != 0x4d324b43 || raw32(data,4) != 1 ||
        raw32(data,12) != 0x40000 || raw32(data,16) != 0x8000)
        throw std::runtime_error("capture format mismatch");
    const std::size_t offset = 28 + raw32(data,8);
    if (data.size() != offset + 0x48000) throw std::runtime_error("capture length mismatch");
    std::copy_n(data.begin()+offset, 0x40000, bus.ewram.begin());
    std::copy_n(data.begin()+offset+0x40000, 0x8000, bus.iwram.begin());
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) {
        std::fprintf(stderr, "Supply the owner's MKSC USA ROM for original-code differential testing.\n");
        return 77;
    }
    try {
        std::ifstream input(argv[1], std::ios::binary);
        std::vector<uint8_t> rom((std::istreambuf_iterator<char>(input)), {});
        if (rom.size() != 0x400000 || rom[0x5f2f4] != 0x43 || rom[0x5f2f5] != 0x79)
            throw std::runtime_error("MKSC USA input mismatch");
        const auto mixer = std::vector<uint8_t>(rom.begin() + 0x5f2f4, rom.begin() + 0x5f674);
        std::mt19937 random(0x4d50324b);
        uint64_t instructions = 0;
        uint64_t output_samples = 0;
        for (unsigned case_id = 0; case_id != 256; ++case_id) {
            TestBus original;
            original.rom = &rom;
            for (auto& value : original.iwram) value = uint8_t(random());
            for (auto& value : original.ewram) value = uint8_t(random());
            std::copy(mixer.begin(), mixer.end(), original.iwram.begin() + 0x2600);
            auto* info = original.iwram.data();
            original.write32(kInfo, 0x68736d54);
            static constexpr unsigned counts[] = {16, 32, 96, 132, 224};
            const unsigned samples = counts[case_id % 5];
            const unsigned counter = 1 + case_id % 3;
            const unsigned offset = counter > 1 ? (6 - counter + 1) * samples : 0;
            const unsigned channel_count = 1 + case_id % 12;
            info[5] = case_id % 3 ? uint8_t(random() & 0x7f) : 0;
            info[6] = uint8_t(channel_count);
            info[7] = uint8_t(random() & 15);
            info[11] = 6;
            original.write32(kInfo + 0x18, 0x1000);
            original.write32(kStack + 8, kInfo + 0x350 + offset);
            original.write32(kStack + 20, 0); // no scanline time-limit in this kernel contract
            original.write32(kStack + 24, kInfo);
            original.write32(kStack + 60, kReturn | 1);
            for (unsigned n = 0; n < channel_count; ++n) {
                auto* c = info + 0x50 + n * 0x40;
                static constexpr uint8_t states[] = {0, 0x80, 0xc0, 3, 2, 1, 0x41, 5};
                const bool loop = (random() & 1) != 0;
                c[0] = uint8_t(states[(case_id + n) % 8] | (loop ? 0x10 : 0));
                c[1] = (case_id + n) % 2 ? 8 : 0;
                c[13] = uint8_t((case_id + n) % 4);
                const uint32_t wave = n & 1 ? 0x02002000 + n * 0x1000 : 0x08001000 + n * 0x1000;
                const unsigned size = 8 + random() % 400;
                const unsigned loop_start = random() % size;
                const unsigned position = random() % size;
                original.write32(wave, loop ? 0xc0000000 : 0);
                original.write32(wave + 8, loop_start);
                original.write32(wave + 12, size);
                for (unsigned j = 0; j < size + 1; ++j) original.write8(wave + 16 + j, uint8_t(random()));
                original.write32(kInfo + 0x50 + n * 0x40 + 0x18, size - position);
                original.write32(kInfo + 0x50 + n * 0x40 + 0x1c, random() & 0x7fffff);
                original.write32(kInfo + 0x50 + n * 0x40 + 0x20, 1 + random() % 0x2800);
                original.write32(kInfo + 0x50 + n * 0x40 + 0x24, wave);
                original.write32(kInfo + 0x50 + n * 0x40 + 0x28, wave + 16 + position);
            }
            TestBus native = original;
            gba::Mp2kNativeStats stats;
            const gba::Mp2kNativeCall call{kInfo, kInfo + 0x350 + offset, samples, counter};
            const auto result = gba::mp2k_native_mix({rom, native.ewram, native.iwram}, call, &stats);
            if (result != gba::Mp2kNativeResult::Complete) throw std::runtime_error("supported fixture rejected");
            uint64_t guest_cycles = 0;
            instructions += run_original(original, samples, counter, &guest_cycles);
            if (guest_cycles > stats.guest_cycle_upper_bound) {
                std::fprintf(stderr, "case=%u reference_cycles=%llu bound=%u\n", case_id,
                    static_cast<unsigned long long>(guest_cycles), stats.guest_cycle_upper_bound);
                throw std::runtime_error("native service cost bound underestimated original");
            }
            require_equal(original, native, case_id);
            output_samples += uint64_t(stats.direct_samples) + stats.interpolated_samples;
            // A rejected input must be transactional, including corrupt voice pointers.
            const auto before = native.iwram;
            auto invalid = call;
            invalid.samples = 0;
            if (gba::mp2k_native_mix({rom, native.ewram, native.iwram}, invalid) !=
                    gba::Mp2kNativeResult::UnsupportedInput || native.iwram != before)
                throw std::runtime_error("unsupported input mutated memory");
            invalid = call;
            invalid.max_guest_cycles = 1;
            if (gba::mp2k_native_mix({rom, native.ewram, native.iwram}, invalid) !=
                    gba::Mp2kNativeResult::UnsupportedInput || native.iwram != before)
                throw std::runtime_error("insufficient service window mutated memory");
            if (case_id == 0) for (unsigned alias = 0; alias != 2; ++alias) {
                TestBus overlapping = native;
                overlapping.iwram[0x50] = alias ? 0x80 : 1;
                overlapping.iwram[0x51] = 8;
                overlapping.write32(kInfo + 0x50 + 0x18, 16);
                overlapping.write32(kInfo + 0x50 + 0x24, kInfo + 0x100);
                overlapping.write32(kInfo + 0x50 + 0x28, call.pcm_buffer);
                const auto saved_iwram = overlapping.iwram;
                const auto saved_ewram = overlapping.ewram;
                if (gba::mp2k_native_mix({rom, overlapping.ewram, overlapping.iwram}, call) !=
                        gba::Mp2kNativeResult::UnsupportedInput ||
                        overlapping.iwram != saved_iwram || overlapping.ewram != saved_ewram)
                    throw std::runtime_error("aliased service input was not rejected transactionally");
            }
        }
        unsigned live_cases = 0;
        if (argc == 3) for (unsigned index=1; index<=16; ++index) {
            char name[64];
            std::snprintf(name,sizeof(name),"call-%03u-before.bin",index);
            const auto before_path = std::filesystem::path(argv[2])/name;
            if (!std::filesystem::exists(before_path)) continue;
            const auto before = capture_file(before_path);
            std::snprintf(name,sizeof(name),"call-%03u-after.bin",index);
            const auto after = capture_file(std::filesystem::path(argv[2])/name);
            TestBus original, native;
            original.rom = native.rom = &rom;
            decode_capture(before,native);
            decode_capture(after,original);
            const uint32_t sp = raw32(before,28+13*4);
            const uint32_t si = raw32(before,28);
            const gba::Mp2kNativeCall call{si,raw32(before,28+5*4),raw32(before,28+8*4),raw32(before,28+4*4)};
            // Use the unmodified ROM's wave data, not the synthetic waves.
            std::ifstream input_real(argv[1],std::ios::binary);
            const std::vector<uint8_t> real_rom((std::istreambuf_iterator<char>(input_real)),{});
            gba::Mp2kNativeStats stats;
            if (gba::mp2k_native_mix({real_rom,native.ewram,native.iwram},call,&stats) != gba::Mp2kNativeResult::Complete)
                throw std::runtime_error("live SDK call rejected");
            require_equal(original,native,256+index,sp);
            // Independently validate the portable adapter's saved-register,
            // stack, return-PC and flags contract against the actual original.
            const auto* saved = native.ptr(sp+28,36);
            for (unsigned r=0;r<16;++r) {
                // LR is an ARM mixer scratch accumulator, not a preserved
                // return register. The audited caller at 0805ff1a pops its
                // saved return address; it never consumes this scratch LR.
                if (r==14) continue;
                uint32_t expected = raw32(before,28+r*4);
                if (r<8) expected = uint32_t(saved[r*4]) | (uint32_t(saved[r*4+1])<<8) |
                    (uint32_t(saved[r*4+2])<<16) | (uint32_t(saved[r*4+3])<<24);
                if (r>=8 && r<=11) expected = native.read32(sp+28+(r-8)*4);
                if (r==3) expected=native.read32(sp+60);
                if (r==12) expected=native.read32(si+0x18);
                if (r==13) expected=sp+64;
                if (r==15) expected=native.read32(sp+60)&~1u;
                if (raw32(after,28+r*4)!=expected) {
                    std::fprintf(stderr,"live=%u r%u expected=%08x actual=%08x\n",index,r,expected,raw32(after,28+r*4));
                    throw std::runtime_error("original live ABI differs from adapter contract");
                }
            }
            const uint32_t flags = (raw32(before,28+64)&~0xf0000020u) | 0x60000000u |
                ((native.read32(sp+60)&1u)?0x20u:0u);
            if (raw32(after,28+64)!=flags) throw std::runtime_error("original live flags differ from adapter contract");
            ++live_cases;
        }
        std::printf("mp2k_native_contract synthetic_cases=256 original_instructions=%llu synthetic_mixed_samples=%llu live_cases=%u live_ABI_checks=%u persistent_RAM=exact unsupported_input=transactional\n",
                    static_cast<unsigned long long>(instructions), static_cast<unsigned long long>(output_samples),live_cases,live_cases);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "mp2k_native_contract FAILED: %s\n", error.what());
        return 1;
    }
}
