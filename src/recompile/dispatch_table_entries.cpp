#include "dispatch_table_entries.h"

#include <algorithm>

#include "function_finder.h"

namespace gbarecomp {

std::vector<DispatchTableEntrySpec> build_dispatch_table_entries(
    const std::vector<Function>& functions) {
    std::vector<DispatchTableEntrySpec> entries;
    std::size_t count = functions.size();
    for (const Function& function : functions) {
        count += function.alias_entries.size();
    }
    entries.reserve(count);

    for (std::size_t i = 0; i < functions.size(); ++i) {
        const Function& function = functions[i];
        const bool thumb = function.mode == CpuMode::Thumb;
        entries.push_back({function.addr, thumb, false, i});
        for (std::uint32_t addr : function.alias_entries) {
            entries.push_back({addr, thumb, true, i});
        }
    }

    std::stable_sort(
        entries.begin(), entries.end(),
        [](const DispatchTableEntrySpec& lhs,
           const DispatchTableEntrySpec& rhs) {
            if (lhs.addr != rhs.addr) return lhs.addr < rhs.addr;
            if (lhs.thumb != rhs.thumb) return lhs.thumb < rhs.thumb;
            // Prefer a real function root when overlapping discovery also
            // produced a same-mode resume alias at the identical address.
            if (lhs.resume != rhs.resume) return lhs.resume < rhs.resume;
            return lhs.function_index < rhs.function_index;
        });
    return entries;
}

}  // namespace gbarecomp
