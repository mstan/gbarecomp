#include "host_capture.h"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2 && argc != 3)
        return 2;

    namespace capture = gbarecomp::capture;
    const std::filesystem::path output = argv[1];
    const bool bounded = argc == 3 && std::string(argv[2]) == "1";
    const bool invalid = argc == 3 && std::string(argv[2]) == "invalid";
    std::array<std::string, 5> options = {
        "host_capture_tests", "--capture-wav", argv[1], "--capture-to", "1",
    };
    std::array<char*, 5> args = {
        options[0].data(), options[1].data(), options[2].data(),
        options[3].data(), options[4].data(),
    };
    int next = 1;
    if (!capture::parse_arg(bounded ? 5 : 3, args.data(), &next))
        return 2;
    ++next;
    if (bounded && !capture::parse_arg(5, args.data(), &next))
        return 2;

    constexpr std::array<int16_t, 4> stereo = {256, -256, 512, -512};
    for (int frame = 0; frame < 3; ++frame) {
        capture::on_frame(nullptr, 0, 0);
        capture::on_audio(stereo.data(), 2, 48000);
    }
    capture::finish();
    if (invalid)
        return capture::failed() && capture::should_exit() ? 0 : 1;

    std::ifstream wav(output, std::ios::binary);
    std::array<unsigned char, 44> header{};
    wav.read(reinterpret_cast<char*>(header.data()), header.size());
    // Two stereo samples per frame. The bounded range keeps frames 0 and 1.
    const unsigned expected = (bounded ? 2 : 3) * stereo.size() * sizeof(int16_t);
    const bool valid = !capture::failed() && wav && header[40] == expected && !header[41] &&
                       !header[42] && !header[43] &&
                       std::filesystem::file_size(output) == header.size() + expected;
    wav.close();
    std::filesystem::remove(output);
    if (!valid)
        std::fprintf(stderr, "Expected %u captured PCM bytes\n", expected);
    return valid ? 0 : 1;
}
