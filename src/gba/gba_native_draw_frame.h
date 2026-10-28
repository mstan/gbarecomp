#pragma once
#include "gba_draw_memory.h"
#include <memory>

namespace gba {

// One frame's historical raster inputs, independent of mutable bus memory or
// GPU resources. IO contains the effective affine references for this row.
// The CPU materializer and the GPU consumer see the same ordered writes.
class GbaNativeDrawFrame {
public:
    static constexpr unsigned kLines = 160;
    struct Line {
        std::uint16_t dispcnt = 0;
        std::array<std::uint8_t, 0x60> io{};
        std::array<std::uint8_t, 1024> oam{}, palette{};
        GbaDrawMemory::Delta memory;
    };
    std::span<const Line> lines() const { return lines_; }
    std::size_t payload_bytes() const {
        std::size_t result = 0;
        for (const auto& line : lines_) result += line.memory.payload.size();
        return result;
    }

private:
    friend class GbaNativeDrawBuilder;
    GbaNativeDrawFrame() = default;
    std::vector<Line> lines_;
};

class GbaNativeDrawBuilder {
public:
    // This seam accepts only the already-admitted hardware raster route.
    // Providers/foreign presentation and escaped memory aliases must decline
    // before capture. It never runs game callbacks or advances device time.
    bool capture(unsigned y, std::uint16_t dispcnt,
                 std::span<const std::uint8_t> io,
                 std::span<const std::uint8_t> oam,
                 std::span<const std::uint8_t> palette,
                 std::span<const std::uint8_t> vram,
                 GbaDrawMemory& memory) {
        if (y >= GbaNativeDrawFrame::kLines || (y && y != lines_.size()) ||
            io.size() < 0x60 || oam.size() != 1024 || palette.size() != 1024 ||
            vram.size() != GbaDrawMemory::kVramBytes) return false;
        if (!y) {
            std::vector<GbaNativeDrawFrame::Line> fresh;
            fresh.reserve(GbaNativeDrawFrame::kLines);
            lines_.swap(fresh);
            memory.invalidate(); // Every retained frame is self-contained.
        }
        GbaNativeDrawFrame::Line line;
        line.dispcnt = dispcnt;
        std::copy_n(io.begin(), line.io.size(), line.io.begin());
        std::copy(oam.begin(), oam.end(), line.oam.begin());
        std::copy(palette.begin(), palette.end(), line.palette.begin());
        line.memory = memory.publish(vram);
        lines_.push_back(std::move(line)); // Capacity is owned before publishing.
        return true;
    }

    std::shared_ptr<const GbaNativeDrawFrame> finish() {
        if (lines_.size() != GbaNativeDrawFrame::kLines) return {};
        auto frame = std::shared_ptr<GbaNativeDrawFrame>(new GbaNativeDrawFrame);
        frame->lines_ = std::move(lines_);
        return frame;
    }

    void reset() { lines_.clear(); }
    std::span<const GbaNativeDrawFrame::Line> lines() const { return lines_; }
private:
    std::vector<GbaNativeDrawFrame::Line> lines_;
};
} // namespace gba
