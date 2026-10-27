#pragma once
#include <cstdint>

namespace gen3 {
// Reviewed, ROM-specific data layouts supplied by the title. The renderer
// never executes reference-decomp code or writes to guest memory.
struct Profile {
    const char* name;
    bool frlg;
    std::uint32_t main, overworld, map_header, backup_map, save_block, camera, map_groups;
    bool indirect_save;
    std::uint32_t objects, sprites, offset_x, offset_y, windows;
    std::uint32_t doors;
    unsigned door_count;
    std::uint32_t graphics, facing, face_anims, priorities, palette_sets, reflection_type, special_palette;
    unsigned graphics_count, movement_count;
    unsigned templates_offset, flags_offset, vars_offset, flag_count;
    std::uint32_t special_flags;
    // RS uses published text frames rather than the newer Window array.
    std::uint32_t standard_frame, dialogue_frame, cursor_sprites;
};
const Profile& game_profile(); // implemented and hash-gated by the title
} // namespace gen3
