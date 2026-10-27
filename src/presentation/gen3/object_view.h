#pragma once
#include "field_view.h"
#include "gba_ppu.h"
#include <map>
#include <vector>

namespace gen3 {
// Read-only presentation of live and dormant overworld objects. Dormant
// actors keep their last observed position/facing; guest simulation owns AI.
class ObjectView {
public:
    bool prepare(const ViewMemory& memory, const FieldView& field, int width, int height = 160);
    const gba::WsMarginObjPixel* row(int y, int* left, int* width) const;
    int objects() const { return objects_; }
    int verified_parts() const { return verified_parts_; }
    int dormant_objects() const { return dormant_objects_; }
    const std::array<unsigned, 7>& mismatch() const { return mismatch_; }
private:
    std::vector<gba::WsMarginObjPixel> pixels_;
    int left_ = 0, top_ = 0, width_ = 0, height_ = 160, objects_ = 0, verified_parts_ = 0;
    bool ready_ = false;
    std::array<unsigned, 7> mismatch_{}; // object, expected attrs, same-tile HW attrs
    struct Pose { int x, y, template_x, template_y; unsigned graphics, direction; bool hidden; };
    std::map<unsigned, Pose> poses_;
    std::uint32_t last_tick_ = 0;
    int dormant_objects_ = 0;
    void dormant(const ViewMemory& memory, const FieldView& field);
};
} // namespace gen3
