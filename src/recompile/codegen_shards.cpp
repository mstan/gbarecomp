#include "codegen_shards.h"

#include <algorithm>

namespace gbarecomp {

std::uint32_t choose_auto_codegen_shards(std::size_t function_count) {
    const std::size_t needed = std::max<std::size_t>(
        kMinCartCodegenShards,
        (function_count + kTargetFunctionsPerShard - 1) /
            kTargetFunctionsPerShard);

    std::uint32_t shards = kMinCartCodegenShards;
    while (shards < needed && shards < kMaxAutoCodegenShards) {
        shards *= 2;
    }
    return std::min(shards, kMaxAutoCodegenShards);
}

}  // namespace gbarecomp
