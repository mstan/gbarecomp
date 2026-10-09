// file_replace.cpp — see file_replace.h.

#include "file_replace.h"

#include <filesystem>
#include <system_error>

namespace gbarecomp::debug {

bool replace_file_preserving(const std::string& tmp, const std::string& dest,
                             std::string* err) {
    std::error_code ec;
    std::filesystem::rename(tmp, dest, ec);  // replaces on POSIX and NTFS
    if (!ec) return true;
    // Rename-over refused (e.g. dest held open without delete sharing). Park
    // the old copy instead of deleting it, so no step can lose both.
    const std::string bak = dest + ".bak";
    std::error_code ec2;
    std::filesystem::remove(bak, ec2);
    if (std::filesystem::exists(dest, ec2)) {
        std::filesystem::rename(dest, bak, ec2);
        if (ec2) {
            if (err) *err = "cannot replace " + dest + " (" + ec.message() +
                            "); new data kept in " + tmp;
            return false;
        }
    }
    std::filesystem::rename(tmp, dest, ec2);
    if (ec2) {
        std::error_code ec3;
        std::filesystem::rename(bak, dest, ec3);  // restore the previous copy
        if (err) *err = "cannot replace " + dest + " (" + ec2.message() +
                        "); new data kept in " + tmp +
                        (ec3 ? ", previous copy in " + bak : std::string());
        return false;
    }
    std::filesystem::remove(bak, ec2);
    return true;
}

}  // namespace gbarecomp::debug
