#pragma once

#include <filesystem>
#include "runtime.h"

namespace gbarecomp {

// Player-owned state can live outside a signed/read-only application bundle.
// Resource discovery continues to use the executable directory separately.
inline std::filesystem::path player_config_directory(const RunOptions& opts,
                                                     const char* executable) {
    if (opts.config_dir && *opts.config_dir)
        return std::filesystem::absolute(opts.config_dir).lexically_normal();
    if (executable && *executable)
        return std::filesystem::absolute(executable).parent_path().lexically_normal();
    return std::filesystem::current_path();
}

inline std::filesystem::path player_config_file(const std::filesystem::path& root,
                                                const char* filename,
                                                const char* fallback) {
    const std::filesystem::path path(filename && *filename ? filename : fallback);
    return path.is_absolute() ? path : root / path;
}

} // namespace gbarecomp
