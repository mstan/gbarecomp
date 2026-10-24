#include "suspend_guard.h"
#include <chrono>
#include <cstdio>

int main() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("gba-suspend-guard-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    const auto save = (root / "game.sav").string();
    const auto marker = (root / "suspend.pending").string();
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
    };
    check(gbarecomp::write_suspend_marker(marker, save, 1), "missing battery can be recorded");
    check(gbarecomp::suspend_marker_matches_battery(marker, save), "unchanged missing battery matches");
    { std::ofstream file(save); file << "old progress"; }
    check(!gbarecomp::suspend_marker_matches_battery(marker, save), "new battery rejects older suspend");
    check(gbarecomp::write_suspend_marker(marker, save, 2), "existing battery can be recorded");
    check(gbarecomp::suspend_marker_matches_battery(marker, save), "unchanged battery resumes");
    // A battery flush succeeds, then the suspend refresh fails or the process
    // dies before it: the old marker must never roll back the new .sav.
    { std::ofstream file(save); file << "new progress"; }
    check(!gbarecomp::suspend_marker_matches_battery(marker, save), "failed refresh preserves newer save");
    check(!gbarecomp::write_suspend_marker(root.string(), save, 3), "marker write errors are reported");
    { std::ofstream file(marker); file << "2\n"; }
    check(!gbarecomp::suspend_marker_matches_battery(marker, save), "incomplete marker cannot resume");
    fs::remove(marker); fs::remove(save); fs::remove(root);
    return failures ? 1 : 0;
}
