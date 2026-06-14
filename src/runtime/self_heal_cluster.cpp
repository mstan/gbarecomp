// self_heal_cluster.cpp — pure clustering of self-heal dispatch misses.
//
// Split out from runtime_arm_default_aborts.cpp (and free of any runtime
// dependency) so it can be unit-tested in isolation: see
// tests/selfheal/cluster_test.cpp. The runtime's proposal writer
// (self_heal_write_report) uses it to recognize when a tight run of
// consecutive same-mode misses is the case targets of a computed-jump
// switch the finder could not size — and to propose ONE sized
// [[jump_table]] over N scattered [[extra_func]] (the MC-HP-006 / Kid_Head
// lesson, where 9 healed fragments in 0x08062894..0x08062920 were really a
// single jump table at 0x08062870).

#include "self_heal.h"

namespace gbarecomp {

std::vector<SelfHealCluster> self_heal_cluster_misses(
    const std::vector<SelfHealMiss>& m,
    std::uint32_t max_gap, std::size_t min_run) {
    std::vector<SelfHealCluster> out;
    std::size_t i = 0;
    while (i < m.size()) {
        // Extend the run while the next miss is the same instruction-set
        // mode and within max_gap bytes of its predecessor. The `pc >=`
        // guard keeps a non-sorted input from underflowing the gap.
        std::size_t j = i + 1;
        while (j < m.size() &&
               m[j].thumb == m[i].thumb &&
               m[j].pc >= m[j - 1].pc &&
               (m[j].pc - m[j - 1].pc) <= max_gap) {
            ++j;
        }
        SelfHealCluster c;
        c.begin = i;
        c.end   = j;
        c.jump_table_candidate = (j - i) >= min_run;
        out.push_back(c);
        i = j;
    }
    return out;
}

}  // namespace gbarecomp
