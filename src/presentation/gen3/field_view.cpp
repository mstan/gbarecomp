#include "field_view.h"

#include <algorithm>

namespace gen3 {
namespace {
// Layouts are provided by the hash-gated game profile.
constexpr int kMapPadding = 7;

int floor8(int value) { return value >= 0 ? value / 8 : -((-value + 7) / 8); }
int floor2(int value) { return value >= 0 ? value / 2 : -((-value + 1) / 2); }
int wrap(int value, int modulus) { return (value % modulus + modulus) % modulus; }
int signed_ring_delta(int value) { return wrap(value + 128, 256) - 128; }
unsigned read16(const std::uint8_t* p) { return p[0] | (unsigned(p[1]) << 8); }

struct Map {
    const ViewMemory& m;
    std::uint32_t data = 0, border = 0;
    std::uint32_t metatiles[2]{}, attributes[2]{};
    int width = 0, height = 0, border_width = 2, border_height = 2;
    struct ConnectedLayout {
        std::uint32_t layout = 0, header = 0, data = 0;
        int x = 0, y = 0, width = 0, height = 0;
        std::uint8_t group = 0, number = 0;
    };
    std::array<ConnectedLayout, 32> connected{};
    int connected_count = 0;

    bool layout_at(std::uint32_t header, ConnectedLayout& out) const {
        if (!m.bytes(header, 16)) return false;
        const auto layout = m.u32(header);
        if (!m.bytes(layout, m.profile->frlg ? 28 : 24)) return false;
        const int w = static_cast<int>(m.u32(layout));
        const int h = static_cast<int>(m.u32(layout + 4));
        const auto data = m.u32(layout + 12);
        if (w < 1 || h < 1 || w > 10240 || h > 10240 || w * h > 10240 ||
            !m.bytes(data, w * h * 2)) return false;
        out = {layout, header, data, 0, 0, w, h};
        return true;
    }

    // The guest only copies seven metatiles of each neighboring map into
    // its padded grid (eight to the east). Taller/wider views can see past
    // that strip. Walk the visible connection graph without changing the
    // guest map, loading graphics, or carrying state across map transitions.
    void extend_connections(int min_x, int min_y, int max_x, int max_y) {
        ConnectedLayout root;
        if (!layout_at(m.profile->map_header, root)) return;
        root.x = root.y = kMapPadding;
        const auto* location = m.bytes(m.save() + 4, 2);
        if (location) { root.group = location[0]; root.number = location[1]; }
        connected[connected_count++] = root;
        for (int next = 0; next < connected_count; ++next) {
            const auto current = connected[next];
            const auto list = m.u32(current.header + 12);
            if (!list || !m.bytes(list, 8)) continue;
            const auto count = m.u32(list), entries = m.u32(list + 4);
            if (count > 64 || !m.bytes(entries, count * 12)) continue;
            for (unsigned i = 0; i < count && connected_count < int(connected.size()); ++i) {
                const auto* entry = m.bytes(entries + i * 12, 12);
                const unsigned direction = entry[0];
                if (direction < 1 || direction > 4) continue; // no dive/emerge/warps
                const int offset = static_cast<std::int32_t>(m.u32(entries + i * 12 + 4));
                if (offset < -10240 || offset > 10240) continue;
                const auto group = m.u32(m.profile->map_groups + entry[8] * 4);
                ConnectedLayout neighbor;
                if (!layout_at(m.u32(group + entry[9] * 4), neighbor)) continue;
                neighbor.group = entry[8]; neighbor.number = entry[9];
                // Tile entries can only address graphics already resident in
                // VRAM. Never interpret an unrelated tileset using those tiles.
                if (m.u32(neighbor.layout + 16) != m.u32(root.layout + 16) ||
                    m.u32(neighbor.layout + 20) != m.u32(root.layout + 20)) continue;
                neighbor.x = current.x;
                neighbor.y = current.y;
                switch (direction) {
                case 1: neighbor.x += offset; neighbor.y += current.height; break;
                case 2: neighbor.x += offset; neighbor.y -= neighbor.height; break;
                case 3: neighbor.x -= neighbor.width; neighbor.y += offset; break;
                case 4: neighbor.x += current.width; neighbor.y += offset; break;
                }
                if (neighbor.x > max_x || neighbor.y > max_y ||
                    neighbor.x + neighbor.width <= min_x ||
                    neighbor.y + neighbor.height <= min_y) continue;
                const auto duplicate = std::find_if(connected.begin(), connected.begin() + connected_count,
                    [&](const ConnectedLayout& seen) {
                        return seen.layout == neighbor.layout && seen.x == neighbor.x && seen.y == neighbor.y;
                    });
                if (duplicate == connected.begin() + connected_count)
                    connected[connected_count++] = neighbor;
            }
        }
    }

    bool load() {
        const auto layout = m.u32(m.profile->map_header);
        if (!m.bytes(layout, m.profile->frlg ? 28 : 24)) return false;
        width = static_cast<int>(m.u32(m.profile->backup_map));
        height = static_cast<int>(m.u32(m.profile->backup_map + 4));
        data = m.u32(m.profile->backup_map + 8);
        border = m.u32(layout + 8);
        if (m.profile->frlg) {
            const auto* dims = m.bytes(layout + 24, 2);
            if (!dims || !dims[0] || !dims[1] || dims[0] > 32 || dims[1] > 32) return false;
            border_width = dims[0]; border_height = dims[1];
        }
        if (width < 1 || height < 1 || width > 10240 || height > 10240 ||
            width * height > 10240 || !m.bytes(data, width * height * 2) ||
            !m.bytes(border, border_width * border_height * 2)) return false;
        for (int i = 0; i < 2; ++i) {
            const auto tileset = m.u32(layout + 16 + i * 4);
            if (!m.bytes(tileset, 24)) return false;
            metatiles[i] = m.u32(tileset + 12);
            attributes[i] = m.u32(tileset + (m.profile->frlg ? 20 : 16));
            if (!m.bytes(metatiles[i], 16) || !m.bytes(attributes[i], 2))
                return false;
        }
        return true;
    }

    unsigned metatile(int x, int y, bool extended = false) const {
        unsigned block = 0x3ff;
        if (x >= 0 && y >= 0 && x < width && y < height)
            block = m.u16(data + 2 * (x + y * width));
        // Live map edits and copied connection strips always win over ROM.
        // Only expand missing cells; native camera validation keeps the exact
        // guest border behavior, including deliberately undefined live cells.
        if (block == 0x3ff && extended && connected_count) {
            const auto& root = connected[0];
            if (x < root.x || y < root.y || x >= root.x + root.width || y >= root.y + root.height) {
                for (int i = 1; i < connected_count; ++i) {
                    const auto& map = connected[i];
                    const int mx = x - map.x, my = y - map.y;
                    if (mx >= 0 && my >= 0 && mx < map.width && my < map.height) {
                        block = m.u16(map.data + 2 * (mx + my * map.width));
                        break;
                    }
                }
            }
        }
        if (block == 0x3ff)
            block = m.u16(border + 2 * (wrap(x - kMapPadding, border_width) + border_width * wrap(y - kMapPadding, border_height)));
        return block & 0x3ff;
    }

    bool entries(int tile_x, int tile_y, std::uint16_t out[3], bool extended = false) const {
        const unsigned id = metatile(floor2(tile_x), floor2(tile_y), extended);
        const unsigned primary = m.profile->frlg ? 640 : 512;
        const unsigned set = id >= primary, index = set ? id - primary : id;
        const auto tiles = metatiles[set] + index * 16;
        const unsigned attr_size = m.profile->frlg ? 4 : 2;
        const auto attr = attributes[set] + index * attr_size;
        if (!m.bytes(tiles, 16) || !m.bytes(attr, attr_size)) return false;
        const unsigned layer = m.profile->frlg ? ((m.u32(attr) >> 29) & 3) : m.u16(attr) >> 12;
        const int quadrant = wrap(tile_x, 2) + 2 * wrap(tile_y, 2);
        const auto bottom = m.u16(tiles + quadrant * 2);
        const auto top = m.u16(tiles + 8 + quadrant * 2);
        switch (layer) {
        case 0: out[0] = top; out[1] = bottom; out[2] = 0x3014; break;
        case 1: out[0] = 0; out[1] = top; out[2] = bottom; break;
        case 2: out[0] = top; out[1] = 0; out[2] = bottom; break;
        default: return false;
        }
        return true;
    }

    // Doors replace map tiles directly in the published ring while leaving
    // the map grid unchanged. Accept only the exact covered-layer pattern
    // described by this ROM's door graphics table and palette assignments.
    bool door_entries(int tile_x, int tile_y, const std::uint16_t actual[3]) const {
        const auto& p = *m.profile;
        // FRLG's buffer names are reversed, but their physical bindings are
        // BG1=top, BG2=middle, BG3=bottom, just like Ruby/Sapphire/Emerald.
        const int top_bg = 0, middle_bg = 1;
        if (actual[top_bg] != 0 || (actual[middle_bg] & 0xFFF) != 0 ||
            (actual[2] & 1023) < 1016 || !m.bytes(p.doors, p.door_count * 12)) return false;
        const int mx = floor2(tile_x), my = floor2(tile_y);
        for (int down = 0; down <= 1; ++down) {
            const auto id = metatile(mx, my + down);
            for (unsigned i = 0; i < p.door_count; ++i) {
                const auto* gfx = m.bytes(p.doors + i * 12, 12);
                if (!gfx || read16(gfx) != id) continue;
                const bool tall = !p.frlg || gfx[3] == 1;
                if (down && !tall) continue;
                const int part = tall ? (1 - down) * 4 : 0;
                const int q = wrap(tile_x, 2) + wrap(tile_y, 2) * 2;
                const auto* palettes = m.bytes(m.u32(p.doors + i * 12 + 8), 12);
                if (!palettes) continue;
                std::uint16_t expected[3]{};
                expected[middle_bg] = palettes[part + 4 + q] << 12;
                expected[2] = (palettes[part + q] << 12) | (1016 + part + q);
                if (std::equal(expected, expected + 3, actual)) return true;
            }
        }
        return false;
    }
};
} // namespace

const std::uint8_t* ViewMemory::bytes(std::uint32_t address, std::size_t size) const {
    const std::uint8_t* base = nullptr;
    std::size_t offset = 0, capacity = 0;
    if (address >= 0x02000000 && address < 0x02040000) {
        base = ewram; offset = address - 0x02000000; capacity = 0x40000;
    } else if (address >= 0x03000000 && address < 0x03008000) {
        base = iwram; offset = address - 0x03000000; capacity = 0x8000;
    } else if (address >= 0x08000000 && address < 0x0A000000) {
        base = rom; offset = address - 0x08000000; capacity = rom_size;
    }
    return base && offset <= capacity && size <= capacity - offset ? base + offset : nullptr;
}
std::uint16_t ViewMemory::u16(std::uint32_t address) const {
    const auto* p = bytes(address, 2);
    return p ? static_cast<std::uint16_t>(read16(p)) : 0;
}
std::uint32_t ViewMemory::u32(std::uint32_t address) const {
    const auto* p = bytes(address, 4);
    return p ? read16(p) | (read16(p + 2) << 16) : 0;
}

const char* view_status_name(ViewStatus status) {
    switch (status) {
    case ViewStatus::Native: return "native";
    case ViewStatus::NonField: return "non-field";
    case ViewStatus::Unsupported: return "unsupported-field";
    case ViewStatus::Unverified: return "unverified-camera";
    case ViewStatus::Ready: return "verified-field";
    }
    return "unknown";
}

ViewStatus FieldView::prepare(const ViewMemory& m, int width, int height) {
    status_ = ViewStatus::Native;
    compared_ = matched_ = 0;
    map_count_ = 0;
    width_ = std::clamp(width, 240, kMaxViewWidth);
    height_ = std::clamp(height, 160, kMaxViewHeight);
    left_ = (width_ - 240) / 2;
    top_ = (height_ - 160) / 2;
    if (width_ == 240 && height_ == 160) return status_;
    if (!m.profile) return status_ = ViewStatus::Unsupported;
    status_ = ViewStatus::NonField;
    if ((m.u32(m.profile->main + 4) & ~1u) != m.profile->overworld) return status_;
    status_ = ViewStatus::Unsupported;
    if (!m.io || !m.vram || !m.iwram) return status_;
    const unsigned dispcnt = read16(m.io);
    // Mode 0, all three field backgrounds, no forced blank. These fields keep
    // rectangular hardware windows enabled during ordinary field rendering;
    // these remain native UI masks. BG0 never extends into the margins.
    if ((dispcnt & 0x87) || (dispcnt & 0xE00) != 0xE00) return status_;
    const int hofs = read16(m.io + 0x14) & 255;
    const int vofs = read16(m.io + 0x16) & 255;
    unsigned screen_base[3]{};
    for (int bg = 1; bg <= 3; ++bg) {
        const auto cnt = read16(m.io + 8 + bg * 2);
        if ((cnt & 0xC080) || ((cnt & 0x40) && (m.io[0x4C] != 0)) ||
            int(read16(m.io + 0x10 + bg * 4) & 255) != hofs ||
            int(read16(m.io + 0x12 + bg * 4) & 255) != vofs) return status_;
        screen_base[bg - 1] = ((cnt >> 8) & 31) * 0x800;
    }
    Map map{m};
    const auto save = m.save();
    if (!map.load() || !m.bytes(save, 4)) return status_;
    const auto* camera = m.bytes(m.profile->camera, 4);
    if (!camera || camera[2] >= 32 || camera[3] >= 32) return status_;
    const int nominal_x = static_cast<std::int16_t>(m.u16(save)) * 16 +
        signed_ring_delta(hofs - camera[2] * 8);
    const int nominal_y = static_cast<std::int16_t>(m.u16(save + 2)) * 16 +
        signed_ring_delta(vofs - camera[3] * 8);
    const int origin_tile_x = floor8(nominal_x), origin_tile_y = floor8(nominal_y);
    phase_x_ = hofs & 7;
    phase_y_ = vofs & 7;

    // Camera RAM may have advanced one metatile before VBlank publishes the
    // ring. Resolve that bounded ambiguity against the displayed VRAM. Every
    // sampled native tile/layer must match, on every frame; never accumulate
    // camera deltas or retain a prior-map cache across warps/savestate loads.
    const auto score = [&](int dx, int dy) {
        int matches = 0, total = 0;
        for (int y = 0; y < 20; y += 3) for (int x = 0; x < 30; x += 3) {
            std::uint16_t expected[3];
            if (!map.entries(origin_tile_x + x + dx, origin_tile_y + y + dy, expected))
                return -1;
            const int ring = wrap((hofs >> 3) + x, 32) + 32 * wrap((vofs >> 3) + y, 32);
            std::uint16_t actual[3];
            for (int bg = 0; bg < 3; ++bg)
                actual[bg] = read16(m.vram + screen_base[bg] + ring * 2);
            const bool door = !std::equal(expected, expected + 3, actual) &&
                map.door_entries(origin_tile_x + x + dx, origin_tile_y + y + dy, actual);
            for (int bg = 0; bg < 3; ++bg) {
                ++total;
                matches += door || expected[bg] == actual[bg];
            }
        }
        compared_ = total;
        return matches;
    };
    status_ = ViewStatus::Unverified;
    int best_x = 0, best_y = 0;
    matched_ = score(0, 0);
    for (int dy : {0, -2, 2}) for (int dx : {0, -2, 2}) {
        if (matched_ == compared_ && compared_ > 0) break;
        const int matches = score(dx, dy);
        if (matches > matched_) { matched_ = matches; best_x = dx; best_y = dy; }
    }
    if (compared_ == 0 || matched_ != compared_) return status_;
    const int first_x = floor8(-left_ + phase_x_);
    const int last_x = floor8(width_ - left_ - 1 + phase_x_);
    const int first_y = floor8(-top_ + phase_y_);
    const int last_y = floor8(height_ - top_ - 1 + phase_y_);
    if (last_x - first_x >= kColumns || last_y - first_y >= kRows) return status_;
    map.extend_connections(floor2(origin_tile_x + best_x + first_x),
                           floor2(origin_tile_y + best_y + first_y),
                           floor2(origin_tile_x + best_x + last_x),
                           floor2(origin_tile_y + best_y + last_y));
    origin_x_ = (origin_tile_x + best_x) * 8 + phase_x_;
    origin_y_ = (origin_tile_y + best_y) * 8 + phase_y_;
    map_count_ = map.connected_count;
    for (int i = 0; i < map_count_; ++i) {
        const auto& region = map.connected[i];
        maps_[i] = {region.header, region.x, region.y, region.group, region.number};
    }
    for (int y = first_y; y <= last_y; ++y) for (int x = first_x; x <= last_x; ++x) {
        std::uint16_t entries[3];
        if (!map.entries(origin_tile_x + best_x + x, origin_tile_y + best_y + y, entries, true))
            return status_;
        // The live ring only represents a bounded 32x32 tile area; never
        // accept its wrapped aliases as doors elsewhere in the expanded map.
        if (x >= 0 && x < 30 && y >= 0 && y < 20) {
            const int ring = wrap((hofs >> 3) + x, 32) + 32 * wrap((vofs >> 3) + y, 32);
            std::uint16_t actual[3];
            for (int bg = 0; bg < 3; ++bg) actual[bg] = read16(m.vram + screen_base[bg] + ring * 2);
            if (map.door_entries(origin_tile_x + best_x + x, origin_tile_y + best_y + y, actual))
                std::copy_n(actual, 3, entries);
        }
        for (int bg = 0; bg < 3; ++bg)
            tiles_[bg][(y - first_y) * kColumns + x - first_x] = entries[bg];
    }
    status_ = ViewStatus::Ready;
    return status_;
}

bool FieldView::tile(int bg, int hardware_x, int screen_y, std::uint16_t* entry) const {
    if (status_ != ViewStatus::Ready || bg < 1 || bg > 3 || !entry ||
        hardware_x < -left_ || hardware_x >= width_ - left_ ||
        screen_y < -top_ || screen_y >= height_ - top_) return false;
    const int x = floor8(hardware_x + phase_x_) - floor8(-left_ + phase_x_);
    const int y = floor8(screen_y + phase_y_) - floor8(-top_ + phase_y_);
    *entry = tiles_[bg - 1][y * kColumns + x];
    return true;
}
} // namespace gen3
