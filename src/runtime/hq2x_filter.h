#pragma once
#include <cstdint>
#include <vector>

namespace gbarecomp {
bool hq2x_enabled() noexcept;
void set_hq2x_enabled(bool enabled) noexcept;

// Presentation-only RGB888 adapter. The input is never modified. A failed
// optional module load returns nullptr so ordinary presentation can continue.
class Hq2xFilter {
public:
    const std::uint8_t* apply(const std::uint8_t* rgb, unsigned width, unsigned height);
private:
    std::vector<std::uint32_t> input_, output_;
    std::vector<std::uint8_t> rgb_;
};
}
