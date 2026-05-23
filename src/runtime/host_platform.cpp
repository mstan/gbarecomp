#include "host_platform.h"

#include <cstdio>

namespace gbarecomp {

std::vector<unsigned char> read_file(const std::string& path, std::string* error) {
    std::vector<unsigned char> out;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (error) *error = "open failed: " + path;
        return out;
    }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz < 0) {
        if (error) *error = "ftell failed: " + path;
        std::fclose(f);
        return out;
    }
    out.resize(static_cast<std::size_t>(sz));
    std::size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    if (got != out.size()) {
        out.clear();
        if (error) *error = "short read: " + path;
    }
    return out;
}

}  // namespace gbarecomp
