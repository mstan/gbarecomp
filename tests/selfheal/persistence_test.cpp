#include "self_heal.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cstdio>

int main() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("gba-miss-persist-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    const auto frag = (root / "misses.frag").string();
    const auto cov = (root / "coverage.json").string();
    { std::ofstream old(frag); old << "stale previous-session miss"; }
    auto read = [](const std::string& path) {
        std::ifstream f(path);
        return std::string(std::istreambuf_iterator<char>(f), {});
    };
    gbarecomp::self_heal_reset();
    gbarecomp::self_heal_set_program_identity("Fixture", "TEST", "abc");
    gbarecomp::self_heal_set_persist_paths(frag.c_str(), cov.c_str());
    bool ok = read(frag).find("stale previous-session") == std::string::npos &&
        read(frag).find("TEST") != std::string::npos &&
        read(cov).find("\"phase\":\"running\"") != std::string::npos;
    gbarecomp::self_heal_persist("abort");
    ok = ok && read(cov).find("\"phase\":\"abort\"") != std::string::npos;
    gbarecomp::self_heal_set_persist_paths(nullptr, nullptr);
    for (const auto& entry : fs::directory_iterator(root))
        ok = ok && entry.path().extension() != ".tmp";
    fs::remove(frag); fs::remove(cov); fs::remove(root);
    if (!ok) std::fprintf(stderr, "self-heal persistence regression\n");
    return ok ? 0 : 1;
}
