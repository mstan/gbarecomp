#include "simulation_archive.h"
#include <cstdlib>
#include <initializer_list>
#include <iostream>

static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

template<class T>
static void roundtrip(T original, std::initializer_list<std::uint8_t> expected) {
    gba::SimulationArchive<false> writer;
    writer.value(original);
    require(writer.bytes() == std::vector<std::uint8_t>(expected),
            "integral state wire representation changed");
    T restored{};
    gba::SimulationArchive<true> reader(writer.bytes());
    reader.value(restored);
    require(restored == original && reader.remaining() == 0,
            "integral state did not roundtrip");
    for (std::size_t length = 0; length < sizeof(T); ++length) {
        bool rejected = false;
        try {
            gba::SimulationArchive<true> truncated(
                std::span<const std::uint8_t>(writer.bytes().data(), length));
            truncated.value(restored);
        } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "truncated integral state was accepted");
    }
}

int main() {
    roundtrip<std::int8_t>(-128, {0x80});
    roundtrip<std::uint8_t>(255, {0xff});
    roundtrip<std::int16_t>(-2, {0xfe, 0xff});
    roundtrip<std::uint16_t>(0x1234, {0x34, 0x12});
    roundtrip<std::int32_t>(std::numeric_limits<std::int32_t>::min(), {0,0,0,0x80});
    roundtrip<std::uint32_t>(0x89abcdefu, {0xef,0xcd,0xab,0x89});
    roundtrip<std::int64_t>(std::numeric_limits<std::int64_t>::min(), {0,0,0,0,0,0,0,0x80});
    roundtrip<std::int64_t>(-1, {0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff});
    roundtrip<std::uint64_t>(0xfedcba9876543210ull, {0x10,0x32,0x54,0x76,0x98,0xba,0xdc,0xfe});
    roundtrip<std::int32_t>(0, {0,0,0,0});
    std::cout << "simulation archive integral wire and bounds passed\n";
}
