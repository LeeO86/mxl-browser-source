// SPDX-License-Identifier: MIT
#include "ograf/library.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <stdexcept>

#include "ograf/files.hpp"
#include "util/logging.hpp"

namespace fs = std::filesystem;

namespace mbs::ograf
{
    namespace
    {
        constexpr std::string_view kManifestSuffix = ".ograf.json";

        picojson::value strings(std::vector<std::string> const& items)
        {
            picojson::array out;
            for (auto const& s : items)
            {
                out.emplace_back(s);
            }
            return picojson::value(out);
        }

        std::string str(picojson::object const& o, char const* key)
        {
            auto const it = o.find(key);
            return it != o.end() && it->second.is<std::string>() ? it->second.get<std::string>() : std::string{};
        }

        bool isRegularInside(fs::path const& root, std::string const& rel)
        {
            if (rel.empty())
            {
                return false;
            }
            std::error_code ec;
            auto const p = root / rel;
            return fs::is_regular_file(fs::symlink_status(p, ec)) && !ec;
        }
    }

    std::string Graphic::manifestDir() const
    {
        auto const slash = manifestPath.rfind('/');
        return slash == std::string::npos ? std::string{} : manifestPath.substr(0, slash);
    }

    std::string Graphic::mainPath() const
    {
        return manifest.is<picojson::object>() ? packagePath(manifestDir(), str(manifest.get<picojson::object>(), "main")) : std::string{};
    }

    int Graphic::stepCount() const
    {
        if (manifest.is<picojson::object>())
        {
            auto const& m = manifest.get<picojson::object>();
            if (auto const it = m.find("stepCount"); it != m.end() && it->second.is<double>())
            {
                return static_cast<int>(it->second.get<double>());
            }
        }
        return 1;
    }

    Library::Library(std::string dir, RenderInfo render, ZipLimits limits)
        : _dir(std::move(dir))
        , _render(render)
        , _limits(limits)
    {
    }

    void Library::load()
    {
        std::error_code ec;
        fs::create_directories(packagesDir(), ec);
        fs::remove_all(_dir + "/tmp", ec);
        fs::create_directories(_dir + "/tmp", ec);
        if (!fs::is_directory(packagesDir()))
        {
            throw std::runtime_error("cannot create the OGraf library at " + _dir);
        }
        std::lock_guard lock{_mu};
        _graphics.clear();
        auto const text = readFile(_dir + "/index.json");
        if (!text)
        {
            return;
        }
        picojson::value doc;
        if (!parseJson(doc, *text).empty() || !doc.is<picojson::object>() || !doc.get("graphics").is<picojson::array>())
        {
            log::error("ograf_index_invalid", {{"path", _dir + "/index.json"}});
            return;
        }
        for (auto const& item : doc.get("graphics").get<picojson::array>())
        {
            if (!item.is<picojson::object>())
            {
                continue;
            }
            auto const& o = item.get<picojson::object>();
            Graphic g;
            g.id = str(o, "id");
            g.name = str(o, "name");
            g.package = str(o, "package");
            g.manifestPath = str(o, "manifest");
            g.createdAt = str(o, "createdAt");
            g.updatedAt = str(o, "updatedAt");
            g.listed = !item.get("listed").is<bool>() || item.get("listed").get<bool>();
            if (item.get("warnings").is<picojson::array>())
            {
                for (auto const& w : item.get("warnings").get<picojson::array>())
                {
                    g.warnings.push_back(w.to_str());
                }
            }
            auto const manifest = readFile(packagesDir() + "/" + g.package + "/" + g.manifestPath);
            if (g.id.empty() || !manifest || !parseJson(g.manifest, *manifest).empty())
            {
                log::warn("ograf_graphic_missing", {{"id", g.id}, {"package", g.package}});
                continue;
            }
            _graphics.push_back(std::move(g));
        }
    }

    bool Library::saveLocked() const
    {
        picojson::array items;
        for (auto const& g : _graphics)
        {
            picojson::object o;
            o["id"] = picojson::value(g.id);
            o["name"] = picojson::value(g.name);
            o["package"] = picojson::value(g.package);
            o["manifest"] = picojson::value(g.manifestPath);
            o["createdAt"] = picojson::value(g.createdAt);
            o["updatedAt"] = picojson::value(g.updatedAt);
            o["listed"] = picojson::value(g.listed);
            o["warnings"] = strings(g.warnings);
            items.emplace_back(o);
        }
        picojson::object doc;
        doc["version"] = picojson::value(1.0);
        doc["graphics"] = picojson::value(items);
        return writeFileAtomic(_dir + "/index.json", picojson::value(doc).serialize(true));
    }

    UploadResult Library::addZip(std::string const& zipPath, InUse const& inUse)
    {
        auto const package = randomId();
        auto const tmp = fs::path(_dir) / "tmp" / package;
        std::error_code ec;
        fs::create_directories(tmp, ec);
        auto const fail = [&](int status, std::string const& message, picojson::array manifests = {}) {
            fs::remove_all(tmp, ec);
            picojson::object body;
            body["error"] = picojson::value(message);
            if (!manifests.empty())
            {
                body["manifests"] = picojson::value(manifests);
            }
            return UploadResult{status, picojson::value(body)};
        };

        std::string error;
        auto const files = unpackZip(zipPath, tmp.string(), _limits, error);
        if (!files)
        {
            return fail(error.starts_with("unpacked size") || error.starts_with("too many") ? 413 : 422, error);
        }
        std::vector<std::string> manifests;
        for (auto const& f : *files)
        {
            if (f.size() > kManifestSuffix.size() && f.ends_with(kManifestSuffix))
            {
                manifests.push_back(f);
            }
        }
        std::sort(manifests.begin(), manifests.end());
        if (manifests.empty())
        {
            return fail(422, "no *.ograf.json manifest in the ZIP");
        }

        std::vector<Graphic> added;
        picojson::array reports;
        bool bad = false;
        std::map<std::string, std::string> seen; // id → manifest path
        for (auto const& path : manifests)
        {
            Graphic g;
            g.manifestPath = path;
            g.package = package;
            auto const dir = g.manifestDir();
            auto const text = readFile((tmp / path).string()).value_or("");
            auto check = checkManifest(text, [&](std::string const& rel) { return isRegularInside(tmp, packagePath(dir, rel)); }, _render);
            if (!check.id.empty())
            {
                if (auto const it = seen.find(check.id); it != seen.end())
                {
                    check.errors.push_back("/id: '" + check.id + "' is also used by " + it->second);
                    check.ok = false;
                }
                seen.emplace(check.id, path);
            }
            picojson::object report;
            report["path"] = picojson::value(path);
            report["id"] = picojson::value(check.id);
            report["errors"] = strings(check.errors);
            report["warnings"] = strings(check.warnings);
            reports.emplace_back(report);
            bad = bad || !check.ok;
            g.id = check.id;
            g.name = check.name;
            g.manifest = check.manifest;
            g.warnings = check.warnings;
            added.push_back(std::move(g));
        }
        if (bad)
        {
            return fail(422, "the ZIP was refused: see the manifests' errors", reports);
        }

        std::lock_guard lock{_mu};
        fs::rename(tmp, fs::path(packagesDir()) / package, ec);
        if (ec)
        {
            return fail(500, "cannot store the package: " + ec.message());
        }
        auto const now = isoNow();
        picojson::array graphics;
        for (auto& g : added)
        {
            bool replaced = false;
            g.createdAt = g.updatedAt = now;
            for (auto& old : _graphics)
            {
                if (old.id == g.id && old.listed)
                {
                    old.listed = false; // kept while a layer still runs it (collect)
                    g.createdAt = old.createdAt;
                    replaced = true;
                }
            }
            picojson::object o;
            o["id"] = picojson::value(g.id);
            o["name"] = picojson::value(g.name);
            o["replaced"] = picojson::value(replaced);
            o["warnings"] = strings(g.warnings);
            graphics.emplace_back(o);
            log::info("ograf_graphic_added", {{"id", g.id}, {"package", package}, {"replaced", replaced}});
            _graphics.push_back(std::move(g));
        }
        collectLocked(inUse);
        if (!saveLocked())
        {
            return UploadResult{500, picojson::value(picojson::object{{"error", picojson::value("cannot write index.json")}})};
        }
        picojson::object body;
        body["package"] = picojson::value(package);
        body["graphics"] = picojson::value(graphics);
        return UploadResult{200, picojson::value(body)};
    }

    std::vector<Graphic> Library::list() const
    {
        std::lock_guard lock{_mu};
        std::vector<Graphic> out;
        std::copy_if(_graphics.begin(), _graphics.end(), std::back_inserter(out), [](Graphic const& g) { return g.listed; });
        std::sort(out.begin(), out.end(), [](Graphic const& a, Graphic const& b) { return a.name == b.name ? a.id < b.id : a.name < b.name; });
        return out;
    }

    std::optional<Graphic> Library::find(std::string const& id) const
    {
        std::lock_guard lock{_mu};
        for (auto const& g : _graphics)
        {
            if (g.listed && g.id == id)
            {
                return g;
            }
        }
        return std::nullopt;
    }

    std::optional<Graphic> Library::findInPackage(std::string const& package, std::string const& id) const
    {
        std::lock_guard lock{_mu};
        for (auto const& g : _graphics)
        {
            if (g.package == package && g.id == id)
            {
                return g;
            }
        }
        return std::nullopt;
    }

    bool Library::remove(std::string const& id, InUse const& inUse)
    {
        std::lock_guard lock{_mu};
        bool found = false;
        for (auto& g : _graphics)
        {
            if (g.listed && g.id == id)
            {
                g.listed = false;
                found = true;
            }
        }
        if (found)
        {
            log::info("ograf_graphic_removed", {{"id", id}});
            collectLocked(inUse);
            saveLocked();
        }
        return found;
    }

    void Library::collect(InUse const& inUse)
    {
        std::lock_guard lock{_mu};
        collectLocked(inUse);
        saveLocked();
    }

    void Library::collectLocked(InUse const& inUse)
    {
        auto const used = inUse ? inUse() : std::set<std::string>{};
        std::erase_if(_graphics, [&](Graphic const& g) { return !g.listed && !used.contains(g.package); });
        std::set<std::string> keep = used;
        for (auto const& g : _graphics)
        {
            keep.insert(g.package);
        }
        std::error_code ec;
        for (auto const& entry : fs::directory_iterator(packagesDir(), ec))
        {
            if (!keep.contains(entry.path().filename().string()))
            {
                fs::remove_all(entry.path(), ec);
                log::info("ograf_package_deleted", {{"package", entry.path().filename().string()}});
            }
        }
    }

    std::string Library::filePath(std::string const& package, std::string const& rel) const
    {
        if (package.empty() || package.find('/') != std::string::npos || package.front() == '.')
        {
            return {};
        }
        auto const clean = packagePath("", rel);
        auto const root = fs::path(packagesDir()) / package;
        return isRegularInside(root, clean) ? (root / clean).string() : std::string{};
    }

    picojson::value graphicListInfo(Graphic const& g)
    {
        picojson::object o;
        o["id"] = picojson::value(g.id);
        o["name"] = picojson::value(g.name);
        if (g.manifest.is<picojson::object>())
        {
            auto const& m = g.manifest.get<picojson::object>();
            for (char const* key : {"description", "version", "thumbnails", "stepCount", "author"})
            {
                if (auto const it = m.find(key); it != m.end())
                {
                    o[key] = it->second;
                }
            }
        }
        o["v_warnings"] = strings(g.warnings);
        o["v_updatedAt"] = picojson::value(g.updatedAt);
        return picojson::value(o);
    }
}
