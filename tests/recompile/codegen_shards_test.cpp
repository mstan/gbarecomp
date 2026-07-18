#include "codegen_shards.h"

#include <cstdint>
#include <cstdio>

namespace {

bool expect(std::size_t functions, std::uint32_t want) {
    const auto got = gbarecomp::choose_auto_codegen_shards(functions);
    if (got == want) return true;
    std::fprintf(stderr,
                 "choose_auto_codegen_shards(%zu): got %u, want %u\n",
                 functions, got, want);
    return false;
}

}  // namespace

int main() {
    bool ok = true;
    ok &= expect(0, 2);
    ok &= expect(1, 2);
    ok &= expect(6000, 2);
    ok &= expect(6001, 4);
    ok &= expect(12000, 4);
    ok &= expect(44584, 16);  // Current Minish Cap corpus.
    ok &= expect(192001, 64);
    ok &= expect(1000000, 64);
    return ok ? 0 : 1;
}
