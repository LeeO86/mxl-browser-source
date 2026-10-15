// SPDX-License-Identifier: MIT
// The OGraf graphics library (docs/ograf.md §4): uploaded ZIPs unpacked as packages under
// <dir>/packages/<uuid>/, an index of graphic ids, replace and delete while a layer still uses
// an older package. Thread-safe. No CEF here.
#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "ograf/manifest.hpp"
#include "ograf/zip.hpp"
#include "picojson/picojson.h"

namespace mbs::ograf
{
    struct Graphic
    {
        std::string id;
        std::string name;
        std::string package;      // package uuid
        std::string manifestPath; // package-relative path of the *.ograf.json
        std::string createdAt;
        std::string updatedAt;
        std::vector<std::string> warnings;
        bool listed = true;       // false after a delete while a layer still used it
        picojson::value manifest; // the manifest as uploaded

        /// The manifest's folder inside the package ("" at the root).
        [[nodiscard]] std::string manifestDir() const;
        /// `main` as a package-relative path.
        [[nodiscard]] std::string mainPath() const;
        [[nodiscard]] int stepCount() const; // manifest stepCount, default 1
    };

    struct UploadResult
    {
        int status = 200;   // 200, 413, 422, 500
        picojson::value body;
    };

    /// The packages layers use now. Asked with the library's lock held, right before packages are
    /// deleted, so a layer that loaded a graphic meanwhile is seen (LayerSet::load checks again
    /// after recording its package). Empty: none.
    using InUse = std::function<std::set<std::string>()>;

    class Library
    {
    public:
        Library(std::string dir, RenderInfo render, ZipLimits limits);

        /// Reads index.json, clears tmp/. Throws std::runtime_error when the folder cannot be made.
        void load();

        /// Adds every graphic of one ZIP, or none (docs/ograf.md §4). Packages in `inUse` are kept
        /// when a graphic moves to a new package.
        UploadResult addZip(std::string const& zipPath, InUse const& inUse);

        [[nodiscard]] std::vector<Graphic> list() const; // listed graphics, by name
        [[nodiscard]] std::optional<Graphic> find(std::string const& id) const;
        /// The graphic a layer loaded, also after it was replaced or unlisted.
        [[nodiscard]] std::optional<Graphic> findInPackage(std::string const& package, std::string const& id) const;

        /// Unlists `id`; its files stay while a package in `inUse` needs them. False when unknown.
        bool remove(std::string const& id, InUse const& inUse);
        /// Deletes packages that neither a listed graphic nor `inUse` refers to.
        void collect(InUse const& inUse);

        /// Absolute path of a package file, empty when it does not exist or leaves the package.
        [[nodiscard]] std::string filePath(std::string const& package, std::string const& rel) const;
        [[nodiscard]] std::string packagesDir() const { return _dir + "/packages"; }
        [[nodiscard]] RenderInfo render() const { return _render; }

    private:
        bool saveLocked() const;
        void collectLocked(InUse const& inUse);

        std::string _dir;
        RenderInfo _render;
        ZipLimits _limits;
        mutable std::mutex _mu;
        std::vector<Graphic> _graphics; // listed and unlisted
    };

    /// GraphicListInfo of the Server API (id, name, description, thumbnails) plus our fields.
    picojson::value graphicListInfo(Graphic const& g);
}
