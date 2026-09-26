// file_replace.h — crash-safe replacement of a file by a written temp file.
//
// Shared by the battery-save flush and save-state slots so both follow the
// same rule: no step may destroy the only complete copy of the data.
#pragma once

#include <string>

namespace gbarecomp::debug {

// Move a fully written sibling temp file over `dest`. The normal path is one
// atomic rename-over (POSIX rename / NTFS MoveFileEx REPLACE_EXISTING). When
// that is refused (e.g. `dest` held open without delete sharing), the
// previous `dest` is parked as `dest.bak` and restored if the second rename
// also fails, and the temp file is kept, its path named in *err, so the new
// data survives too.
bool replace_file_preserving(const std::string& tmp, const std::string& dest,
                             std::string* err);

}  // namespace gbarecomp::debug
