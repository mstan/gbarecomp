#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gbarecomp {

struct Function;

struct DispatchTableEntrySpec {
    std::uint32_t addr;
    bool thumb;
    bool resume;
    std::size_t function_index;
};

// Flatten function roots and resume aliases into the global ordering required
// by runtime_arm's binary search. ARM and Thumb bodies may overlap at the same
// numeric addresses, so sorting each host followed by its aliases is not
// sufficient when an alias crosses the next host.
std::vector<DispatchTableEntrySpec> build_dispatch_table_entries(
    const std::vector<Function>& functions);

}  // namespace gbarecomp
