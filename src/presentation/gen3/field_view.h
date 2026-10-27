#pragma once

#include "profile.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace gbarecomp { struct RunOptions; struct ExtendedViewFrameInfo; }

namespace gen3 {

constexpr int kMaxViewWidth = 569; // round(160 * 32 / 9)
constexpr int kMaxViewHeight = 854; // includes phone portrait and 9:32

// Borrowed, read-only guest regions. Reads never touch the emulated bus, its
// prefetch state, CPU cycles, or guest memory. Also used by capture tests.
struct ViewMemory {
    const std::uint8_t* ewram = nullptr;
    const std::uint8_t* iwram = nullptr;
    const std::uint8_t* rom = nullptr;
    std::size_t rom_size = 0;
    const std::uint8_t* vram = nullptr;
    const std::uint8_t* io = nullptr;
    const std::uint8_t* oam = nullptr;
    const std::uint8_t* pal = nullptr;
    const Profile* profile = nullptr;
    std::uint32_t save() const { return profile ? (profile->indirect_save ? u32(profile->save_block) : profile->save_block) : 0; }
    const std::uint8_t* bytes(std::uint32_t address, std::size_t size) const;
    std::uint16_t u16(std::uint32_t address) const;
    std::uint32_t u32(std::uint32_t address) const;
};

enum class ViewStatus { Native, NonField, Unsupported, Unverified, Ready };
const char* view_status_name(ViewStatus status);

struct FieldMapRegion {
    std::uint32_t header = 0;
    int x = 0, y = 0; // metatiles in the current padded map's coordinates
    std::uint8_t group = 0, number = 0;
};

class FieldView {
public:
    ViewStatus prepare(const ViewMemory& memory, int width, int height = 160);
    bool tile(int bg, int hardware_x, int screen_y, std::uint16_t* entry) const;
    ViewStatus status() const { return status_; }
    int compared() const { return compared_; }
    int matched() const { return matched_; }
    int origin_x() const { return origin_x_; }
    int origin_y() const { return origin_y_; }
    int map_count() const { return map_count_; }
    const FieldMapRegion& map(int index) const { return maps_[index]; }
private:
    static constexpr int kColumns = 73; // 569px plus a partial tile at either end
    static constexpr int kRows = 108;
    std::array<std::array<std::uint16_t, kColumns * kRows>, 3> tiles_{};
    ViewStatus status_ = ViewStatus::Native;
    int left_ = 0, top_ = 0, width_ = 240, height_ = 160, phase_x_ = 0, phase_y_ = 0;
    int compared_ = 0, matched_ = 0;
    int origin_x_ = 0, origin_y_ = 0, map_count_ = 0;
    std::array<FieldMapRegion, 32> maps_{};
};

void configure_view(gbarecomp::RunOptions& options, const Profile& profile);
void install_extended_view(std::uint32_t left, std::uint32_t right);
void update_extended_view(const gbarecomp::ExtendedViewFrameInfo* frame);
void reset_extended_view();

} // namespace gen3
