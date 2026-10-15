// SPDX-License-Identifier: MIT
// OGraf v1 manifest check (docs/ograf.md §4): the EBU JSON schema (draft 2020-12, embedded,
// no network) plus the rules this renderer needs. No CEF here, so the unit tests reach it.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "picojson/picojson.h"

namespace mbs::ograf
{
    /// What the renderer offers a graphic (load()'s renderCharacteristics).
    struct RenderInfo
    {
        int width = 1920;
        int height = 1080;
        double frameRate = 50;
        bool internet = false; // accessToPublicInternet
    };

    struct ManifestCheck
    {
        bool ok = false;
        std::string id;
        std::string name;
        picojson::value manifest;
        std::vector<std::string> errors;   // refuse the upload
        std::vector<std::string> warnings; // keep, show in the library
    };

    /// `fileExists(path)`: whether `path` (relative to the manifest's folder, as written in the
    /// manifest) names a regular file inside the package. Paths that leave the package are errors.
    ManifestCheck checkManifest(std::string const& text, std::function<bool(std::string const&)> const& fileExists, RenderInfo const& render);

    /// `path` relative to the manifest's folder joined to `manifestDir` (a package-relative folder,
    /// "" for the root); empty when it is absolute or leaves the package.
    std::string packagePath(std::string const& manifestDir, std::string const& path);
}
