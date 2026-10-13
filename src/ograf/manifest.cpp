// SPDX-License-Identifier: MIT
#include "ograf/manifest.hpp"
#include "ograf/files.hpp"

#include <cmath>
#include <mutex>
#include <optional>

#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>

#include "ograf_schemas.hpp" // generated from third_party/ograf/json-schemas (CMakeLists.txt)

namespace mbs::ograf
{
    namespace
    {
        namespace js = jsoncons::jsonschema;
        using jsoncons::json;

        constexpr std::string_view kSchemaBase = "https://ograf.ebu.io/v1/specification/json-schemas/";

        json embedded(std::string_view rel)
        {
            auto const text = schemaFile(rel);
            return text.empty() ? json::null() : json::parse(text);
        }

        // Compiled once; jsoncons' json_schema is safe to use from several threads after that.
        js::json_schema<json> const& schema()
        {
            static js::json_schema<json> const compiled = js::make_json_schema(embedded("graphics/schema.json"), [](jsoncons::uri const& uri) {
                auto const s = uri.string();
                return s.starts_with(kSchemaBase) ? embedded(std::string_view(s).substr(kSchemaBase.size())) : json::null();
            });
            return compiled;
        }

        // A NumberConstraint (exact, min, max; ideal is only a wish).
        bool meets(picojson::value const& constraint, double value)
        {
            if (!constraint.is<picojson::object>())
            {
                return true;
            }
            auto const& c = constraint.get<picojson::object>();
            auto const num = [&](char const* key) -> std::optional<double> {
                auto const it = c.find(key);
                return it != c.end() && it->second.is<double>() ? std::optional(it->second.get<double>()) : std::nullopt;
            };
            if (auto const exact = num("exact"); exact && std::abs(*exact - value) > 0.01)
            {
                return false;
            }
            if (auto const lo = num("min"); lo && value < *lo - 0.01)
            {
                return false;
            }
            if (auto const hi = num("max"); hi && value > *hi + 0.01)
            {
                return false;
            }
            return true;
        }

        bool meetsRequirement(picojson::object const& req, RenderInfo const& render)
        {
            if (auto const it = req.find("resolution"); it != req.end() && it->second.is<picojson::object>())
            {
                auto const& res = it->second.get<picojson::object>();
                if (auto const w = res.find("width"); w != res.end() && !meets(w->second, render.width))
                {
                    return false;
                }
                if (auto const h = res.find("height"); h != res.end() && !meets(h->second, render.height))
                {
                    return false;
                }
            }
            if (auto const it = req.find("frameRate"); it != req.end() && !meets(it->second, render.frameRate))
            {
                return false;
            }
            if (auto const it = req.find("accessToPublicInternet"); it != req.end() && it->second.is<picojson::object>())
            {
                auto const& c = it->second.get<picojson::object>();
                if (auto const exact = c.find("exact"); exact != c.end() && exact->second.is<bool>() && exact->second.get<bool>() != render.internet)
                {
                    return false;
                }
            }
            return true;
        }

        std::string str(picojson::object const& o, char const* key)
        {
            auto const it = o.find(key);
            return it != o.end() && it->second.is<std::string>() ? it->second.get<std::string>() : std::string{};
        }
    }

    std::string packagePath(std::string const& manifestDir, std::string const& path)
    {
        if (path.empty() || path.front() == '/' || path.find('\\') != std::string::npos || path.find("://") != std::string::npos)
        {
            return {};
        }
        std::vector<std::string> parts;
        auto const push = [&](std::string const& text) {
            std::size_t start = 0;
            while (start <= text.size())
            {
                auto const end = text.find('/', start);
                auto const part = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
                if (part == "..")
                {
                    if (parts.empty())
                    {
                        return false;
                    }
                    parts.pop_back();
                }
                else if (!part.empty() && part != ".")
                {
                    parts.push_back(part);
                }
                if (end == std::string::npos)
                {
                    break;
                }
                start = end + 1;
            }
            return true;
        };
        if (!push(manifestDir) || !push(path) || parts.empty())
        {
            return {};
        }
        std::string out;
        for (auto const& part : parts)
        {
            out += (out.empty() ? "" : "/") + part;
        }
        return out;
    }

    ManifestCheck checkManifest(std::string const& text, std::function<bool(std::string const&)> const& fileExists, RenderInfo const& render)
    {
        ManifestCheck out;
        if (auto const err = parseJson(out.manifest, text); !err.empty() || !out.manifest.is<picojson::object>())
        {
            out.errors.push_back("not a JSON object: " + err);
            return out;
        }
        try
        {
            schema().validate(json::parse(text), [&](js::validation_message const& msg) -> js::walk_result {
                auto const where = msg.instance_location().string();
                auto const line = (where.empty() ? std::string("/") : where) + ": " + msg.message();
                // Fields this schema version does not know (newer manifests, the EBU l3rd-name
                // example's thumbnail `format`) are kept with a warning, not refused.
                (msg.message().starts_with("Additional property") ? out.warnings : out.errors).push_back(line);
                return js::walk_result::advance;
            });
        }
        catch (std::exception const& e)
        {
            out.errors.push_back(std::string("schema check failed: ") + e.what());
        }

        auto const& m = out.manifest.get<picojson::object>();
        out.id = str(m, "id");
        out.name = str(m, "name");
        if (out.id.empty() || out.id.find('/') != std::string::npos)
        {
            out.errors.push_back("/id: must be a non-empty string without '/'");
        }
        if (auto const it = m.find("supportsRealTime"); it == m.end() || !it->second.is<bool>() || !it->second.get<bool>())
        {
            out.errors.push_back("/supportsRealTime: this renderer is real-time only");
        }
        if (auto const main = str(m, "main"); main.empty() || !fileExists(main))
        {
            out.errors.push_back("/main: '" + main + "' is not a file in the package");
        }
        if (auto const it = m.find("thumbnails"); it != m.end() && it->second.is<picojson::array>())
        {
            for (auto const& t : it->second.get<picojson::array>())
            {
                auto const file = t.is<picojson::object>() ? str(t.get<picojson::object>(), "file") : std::string{};
                if (!file.empty() && !fileExists(file))
                {
                    out.warnings.push_back("/thumbnails: '" + file + "' is not a file in the package");
                }
            }
        }
        if (auto const it = m.find("renderRequirements"); it != m.end() && it->second.is<picojson::array>() && !it->second.get<picojson::array>().empty())
        {
            bool any = false;
            for (auto const& r : it->second.get<picojson::array>())
            {
                any = any || (r.is<picojson::object>() && meetsRequirement(r.get<picojson::object>(), render));
            }
            if (!any)
            {
                out.warnings.push_back("/renderRequirements: this output (" + std::to_string(render.width) + "x" + std::to_string(render.height) + " at " +
                                       picojson::value(render.frameRate).serialize() + " fps, internet " + (render.internet ? "yes" : "no") +
                                       ") meets none of them");
            }
        }
        out.ok = out.errors.empty();
        return out;
    }
}
