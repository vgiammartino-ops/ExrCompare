// Finds image sequences on disk (name.####.exr, name_1001.exr, name.exr ...).
#pragma once

#include "exrc/Types.h"

namespace exrc {

struct ParsedName {
    bool hasFrame = false;
    std::string prefix;  // everything before the frame digits
    std::string digits;  // "1001" (empty if no frame number)
    std::string suffix;  // everything after (".exr")
};

// Splits a filename around its LAST run of digits before the extension.
ParsedName parseFrameName(const std::string& filename);

bool hasExrExtension(const fs::path& p);

// Accepts any one frame of a sequence or a directory. For a directory, the sequence with
// the most frames is chosen (as RV does when a folder is dropped).
std::optional<SequenceInfo> scanSequence(const fs::path& fileOrDirectory, std::string* error = nullptr);

// Version token in a sequence path, e.g. "v003" in /renders/v003/shot_v003.####.exr.
// Returns "" if the path has no version token.
std::string versionToken(const SequenceInfo& seq);

// Finds the nearest existing version in direction `delta` (+1 newer, -1 older), replacing every
// occurrence of the version token in the directory and file name. Returns one frame of that
// version, or nullopt if none exists.
std::optional<fs::path> findVersionSibling(const SequenceInfo& seq, int delta);

}  // namespace exrc
