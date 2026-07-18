#pragma once

#include <cstddef>
#include <cstdint>

namespace gbarecomp {

// Cartridge output is intentionally never monolithic.  The C++ frontend is
// the expensive stage for large games, so even tiny carts receive at least two
// deterministic translation units and large corpora scale to a bounded power
// of two.  A stable power-of-two count also minimizes shard churn as discovery
// grows.
constexpr std::uint32_t kMinCartCodegenShards = 2;
constexpr std::uint32_t kMaxAutoCodegenShards = 64;
constexpr std::size_t kTargetFunctionsPerShard = 3000;

std::uint32_t choose_auto_codegen_shards(std::size_t function_count);

}  // namespace gbarecomp
