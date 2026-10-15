// SPDX-License-Identifier: MIT
// Small file and id helpers of the OGraf library and layer state.
#pragma once

#include <optional>
#include <string>

#include "picojson/picojson.h"

namespace mbs::ograf
{
    /// A random UUID (version 4) for packages and graphic instances.
    std::string randomId();
    /// The current UTC time as ISO 8601 (2026-10-13T20:15:00Z).
    std::string isoNow();
    /// The whole file; nullopt when it cannot be read.
    std::optional<std::string> readFile(std::string const& path);
    /// tmp + rename, as the config document (SPEC §11). False when it failed.
    bool writeFileAtomic(std::string const& path, std::string const& text);
    /// picojson::parse in one place; returns the error text (empty when it parsed).
    std::string parseJson(picojson::value& out, std::string const& text);
}
