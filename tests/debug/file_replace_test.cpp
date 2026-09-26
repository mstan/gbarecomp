// file_replace_test.cpp — replace_file_preserving never loses the only copy.
//
// Cases: a plain replace; a replace with no previous file; and (Windows) a
// destination held open without delete sharing, where every rename touching
// it is refused — the previous contents must stay in place and the new data
// must survive in the temp file.

#include "file_replace.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;
using gbarecomp::debug::replace_file_preserving;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

void write(const fs::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
}

std::string read(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "gbarecomp_file_replace_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const fs::path dest = dir / "slot.state";
    const fs::path tmp = dir / "slot.state.tmp";
    const fs::path bak = dir / "slot.state.bak";
    std::string err;

    write(dest, "old");
    write(tmp, "new");
    check(replace_file_preserving(tmp.string(), dest.string(), &err),
          "plain replace succeeds");
    check(read(dest) == "new", "plain replace installs the new data");
    check(!fs::exists(tmp) && !fs::exists(bak), "plain replace leaves no temp/bak");

    fs::remove(dest);
    write(tmp, "first");
    check(replace_file_preserving(tmp.string(), dest.string(), &err),
          "replace without a previous file succeeds");
    check(read(dest) == "first", "first write installs the data");

#if defined(_WIN32)
    write(dest, "old");
    write(tmp, "new");
    // Open without FILE_SHARE_DELETE: MoveFileEx over it and renaming it
    // away are both refused while this handle lives.
    HANDLE h = CreateFileW(dest.wstring().c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(h != INVALID_HANDLE_VALUE, "hold destination open");
    err.clear();
    const bool ok = replace_file_preserving(tmp.string(), dest.string(), &err);
    CloseHandle(h);
    check(!ok, "locked destination reports failure");
    check(read(dest) == "old", "locked destination keeps the previous data");
    check(fs::exists(tmp) && read(tmp) == "new", "new data survives in temp file");
    check(err.find(tmp.string()) != std::string::npos, "error names the temp file");
    // Once the lock is gone the same call completes normally.
    check(replace_file_preserving(tmp.string(), dest.string(), &err),
          "retry after unlock succeeds");
    check(read(dest) == "new" && !fs::exists(bak), "retry installs the new data");
#endif

    fs::remove_all(dir, ec);
    if (g_failures) return 1;
    std::printf("file_replace_tests: all passed\n");
    return 0;
}
