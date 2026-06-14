// cluster_test.cpp — unit test for self_heal_cluster_misses (the jump-table-
// aware grouping of self-heal dispatch misses). Pure logic, no runtime deps:
// links only src/runtime/self_heal_cluster.cpp.

#include "self_heal.h"

#include <cstdio>
#include <vector>

using gbarecomp::SelfHealMiss;
using gbarecomp::SelfHealCluster;
using gbarecomp::self_heal_cluster_misses;

namespace {
int g_failures = 0;
void check(bool cond, const char* msg) {
    if (!cond) { std::printf("FAIL: %s\n", msg); ++g_failures; }
}
}  // namespace

int main() {
    // 1. Minish Cap Kid_Head's healed fragments (tight, all THUMB, with one
    //    70-byte gap where an un-hit case sits) → ONE jump-table cluster.
    std::vector<SelfHealMiss> kh = {
        {0x08062894u, true, 32}, {0x080628A6u, true, 30}, {0x080628B2u, true, 28},
        {0x080628BCu, true, 28}, {0x080628C6u, true, 30}, {0x080628D0u, true, 28},
        {0x08062916u, true, 28}, {0x0806291Au, true, 28}, {0x08062920u, true, 28},
    };
    auto c = self_heal_cluster_misses(kh, 0x80, 3);
    check(c.size() == 1, "Kid_Head: single cluster");
    check(!c.empty() && c[0].jump_table_candidate, "Kid_Head: flagged JT candidate");
    check(!c.empty() && c[0].begin == 0 && c[0].end == 9, "Kid_Head: spans all 9");

    // 2. Two isolated misses far apart → two singletons, neither a JT.
    std::vector<SelfHealMiss> iso = {{0x08001000u, true, 1}, {0x08050000u, true, 1}};
    auto c2 = self_heal_cluster_misses(iso, 0x80, 3);
    check(c2.size() == 2, "isolated: two clusters");
    check(c2.size() == 2 && !c2[0].jump_table_candidate &&
          !c2[1].jump_table_candidate, "isolated: no JT candidates");

    // 3. A mode break splits a run even when addresses are tight.
    std::vector<SelfHealMiss> mix = {
        {0x08010000u, true, 1}, {0x08010008u, true, 1}, {0x08010010u, false, 1},
        {0x08010018u, false, 1}, {0x08010020u, false, 1},
    };
    auto c3 = self_heal_cluster_misses(mix, 0x80, 3);
    check(c3.size() == 2, "mode split: two clusters");
    check(c3.size() == 2 && !c3[0].jump_table_candidate,
          "mode split: thumb run of 2 not JT");
    check(c3.size() == 2 && c3[1].jump_table_candidate,
          "mode split: arm run of 3 is JT");

    // 4. A gap larger than max_gap splits the run.
    std::vector<SelfHealMiss> gap = {
        {0x08020000u, true, 1}, {0x08020008u, true, 1}, {0x08020200u, true, 1},
    };
    auto c4 = self_heal_cluster_misses(gap, 0x80, 3);
    check(c4.size() == 2, "gap split: two clusters");

    // 5. Run exactly at min_run is flagged; below is not.
    std::vector<SelfHealMiss> three = {
        {0x08030000u, true, 1}, {0x08030004u, true, 1}, {0x08030008u, true, 1}};
    check(self_heal_cluster_misses(three, 0x80, 3)[0].jump_table_candidate,
          "exactly min_run flagged");
    std::vector<SelfHealMiss> two = {{0x08030000u, true, 1}, {0x08030004u, true, 1}};
    check(!self_heal_cluster_misses(two, 0x80, 3)[0].jump_table_candidate,
          "below min_run not flagged");

    // 6. Empty input → no clusters (no crash).
    check(self_heal_cluster_misses({}, 0x80, 3).empty(), "empty: no clusters");

    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("selfheal_cluster_test: all checks passed\n");
    return 0;
}
