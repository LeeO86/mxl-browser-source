// SPDX-License-Identifier: MIT
// Unpacking an uploaded OGraf ZIP (docs/ograf.md §4) with miniz: regular files and folders only,
// no absolute paths, no "..", no symlinks, no encryption, limited size and entry count.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mbs::ograf
{
    struct ZipLimits
    {
        std::uint64_t maxUnpackedBytes = 1024ull << 20;
        std::size_t maxEntries = 10000;
    };

    /// Unpacks `zipPath` into the existing folder `dest`. `__MACOSX/` and dot-files are skipped.
    /// Returns the package-relative paths of the files written, or nullopt with `error` set.
    std::optional<std::vector<std::string>> unpackZip(std::string const& zipPath, std::string const& dest, ZipLimits const& limits, std::string& error);
}
