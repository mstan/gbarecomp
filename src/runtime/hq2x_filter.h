#pragma once
#include <cstdint>
#include <vector>

namespace gbarecomp {
// 0 keeps normal pixels; 2, 3 and 4 select the genuine upstream HQx scaler.
unsigned hqx_scale() noexcept;
bool set_hqx_scale(unsigned scale) noexcept;

// Retained for existing HQ2x plugins and callers.
bool hq2x_enabled() noexcept;
void set_hq2x_enabled(bool enabled) noexcept;

// Presentation-only RGB888 adapter. The input is never modified. A failed
// optional module load returns nullptr so ordinary presentation can continue.
class Hq2xFilter {
public:
    const std::uint8_t* apply(const std::uint8_t* rgb, unsigned width, unsigned height,
                              unsigned scale = 2);
private:
    std::vector<std::uint32_t> input_, output_;
    std::vector<std::uint8_t> rgb_;
};
}
