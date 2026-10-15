// SPDX-License-Identifier: MIT
#include "ograf/serverapi.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>

#include "ograf/files.hpp"
#include "util/logging.hpp"

namespace mbs::ograf
{
    namespace
    {
        constexpr std::string_view kBase = "/ograf/v1";
        constexpr std::string_view kRenderer = "main";

        void json(ops::HttpResponse& res, int status, picojson::value const& body)
        {
            res.status = status;
            res.contentType = "application/json; charset=utf-8";
            res.body = body.serialize();
        }

        char const* titleOf(int status)
        {
            switch (status)
            {
            case 401: return "Unauthorized";
            case 404: return "Not Found";
            case 405: return "Method Not Allowed";
            case 409: return "Conflict";
            default: return "Bad Request";
            }
        }

        void problem(ops::HttpResponse& res, int status, std::string const& detail)
        {
            picojson::object o;
            o["status"] = picojson::value(static_cast<double>(status));
            o["title"] = picojson::value(titleOf(status));
            o["detail"] = picojson::value(detail);
            json(res, status, picojson::value(o));
        }

        void outcome(ops::HttpResponse& res, Outcome const& o)
        {
            json(res, o.status, o.body);
        }

        std::vector<std::string> split(std::string_view path)
        {
            std::vector<std::string> out;
            std::size_t start = 0;
            while (start < path.size())
            {
                auto end = path.find('/', start);
                if (end == std::string_view::npos)
                {
                    end = path.size();
                }
                if (end > start)
                {
                    out.push_back(ops::urlDecode(path.substr(start, end - start)));
                }
                start = end + 1;
            }
            return out;
        }

        /// {"layer": n} → n; nullopt when it is not one of the 8 targets.
        std::optional<int> target(picojson::value const& v)
        {
            if (!v.is<picojson::object>() || !v.get("layer").is<double>())
            {
                return std::nullopt;
            }
            double const n = v.get("layer").get<double>();
            if (n != static_cast<int>(n) || n < 1 || n > kLayers)
            {
                return std::nullopt;
            }
            return static_cast<int>(n);
        }

        picojson::value targetJson(int layer)
        {
            return picojson::value(picojson::object{{"layer", picojson::value(static_cast<double>(layer))}});
        }

        picojson::value targetInfo(ApiContext& ctx, Layer const& l)
        {
            picojson::array instances;
            if (l.state != "empty")
            {
                auto const g = ctx.library.findInPackage(l.package, l.graphicId);
                picojson::object item;
                item["graphicInstanceId"] = picojson::value(l.instanceId);
                item["graphic"] = g ? graphicListInfo(*g) : picojson::value(picojson::object{{"id", picojson::value(l.graphicId)}, {"name", picojson::value(l.graphicName)}});
                instances.emplace_back(item);
            }
            picojson::object o;
            o["renderTarget"] = targetJson(l.number);
            o["name"] = picojson::value("Layer " + std::to_string(l.number));
            o["graphicInstances"] = picojson::value(instances);
            o["v_layer"] = layerJson(l);
            return picojson::value(o);
        }

        picojson::value rendererInfo(ApiContext& ctx, bool full)
        {
            picojson::object o;
            o["id"] = picojson::value(std::string(kRenderer));
            o["name"] = picojson::value(ctx.name);
            o["description"] = picojson::value("mxl-browser-source: 8 layers into one MXL flow (fill and key)");
            if (!full)
            {
                return picojson::value(o);
            }
            auto const failed = ctx.rendererError ? ctx.rendererError() : std::string{};
            auto const why = ctx.rendererProblem ? ctx.rendererProblem() : std::string{};
            picojson::object status;
            status["status"] = picojson::value(!failed.empty() ? "ERROR" : why.empty() ? "OK" : "WARNING");
            status["message"] = picojson::value(!failed.empty() ? failed : why.empty() ? std::string("Renderer is running") : why);
            o["status"] = picojson::value(status);
            o["customActions"] = picojson::value(picojson::array{});
            auto const r = ctx.library.render();
            picojson::object res;
            res["width"] = picojson::value(static_cast<double>(r.width));
            res["height"] = picojson::value(static_cast<double>(r.height));
            o["renderCharacteristics"] = picojson::value(picojson::object{
                {"resolution", picojson::value(res)}, {"frameRate", picojson::value(r.frameRate)}, {"accessToPublicInternet", picojson::value(r.internet)}});
            // A shallow GDD object (server-api.yaml RenderTargetSchema). The defaults let a GDD form
            // (e.g. ograf-form's getDefaultDataFromSchema in SuperFlyTV's controller) start at layer 1.
            picojson::object layerSchema;
            layerSchema["type"] = picojson::value("integer");
            layerSchema["title"] = picojson::value("Layer");
            layerSchema["default"] = picojson::value(1.0);
            layerSchema["minimum"] = picojson::value(1.0);
            layerSchema["maximum"] = picojson::value(static_cast<double>(kLayers));
            o["renderTargetSchema"] = picojson::value(picojson::object{{"type", picojson::value("object")},
                {"properties", picojson::value(picojson::object{{"layer", picojson::value(layerSchema)}})},
                {"required", picojson::value(picojson::array{picojson::value("layer")})}, {"default", targetJson(1)}});
            picojson::array targets;
            for (auto const& l : ctx.layers.snapshot())
            {
                targets.push_back(targetInfo(ctx, l));
            }
            o["renderTargets"] = picojson::value(targets);
            return picojson::value(o);
        }

        picojson::value withUse(ApiContext& ctx, Graphic const& g)
        {
            auto info = graphicListInfo(g).get<picojson::object>();
            picojson::array use;
            for (auto const& l : ctx.layers.snapshot())
            {
                if (l.state != "empty" && l.graphicId == g.id)
                {
                    picojson::object u;
                    u["layer"] = picojson::value(static_cast<double>(l.number));
                    u["current"] = picojson::value(l.package == g.package); // false: the layer runs the replaced version
                    use.emplace_back(u);
                }
            }
            info["v_inUse"] = picojson::value(use);
            return picojson::value(info);
        }

        std::string contentTypeOf(std::string const& path)
        {
            auto ext = std::filesystem::path(path).extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext == ".png")
            {
                return "image/png";
            }
            if (ext == ".jpg" || ext == ".jpeg")
            {
                return "image/jpeg";
            }
            if (ext == ".gif")
            {
                return "image/gif";
            }
            if (ext == ".webp")
            {
                return "image/webp";
            }
            return "application/octet-stream";
        }

        void cors(ApiContext const& ctx, ops::HttpRequest const& req, ops::HttpResponse& res)
        {
            auto const origin = req.header("Origin");
            if (!origin.empty() && std::find(ctx.corsOrigins.begin(), ctx.corsOrigins.end(), origin) != ctx.corsOrigins.end())
            {
                res.headers.emplace_back("Access-Control-Allow-Origin", origin);
                res.headers.emplace_back("Access-Control-Allow-Methods", "GET, PUT, POST, HEAD, OPTIONS, DELETE");
                res.headers.emplace_back("Access-Control-Allow-Headers", "Content-Type, Accept, Authorization");
                res.headers.emplace_back("Access-Control-Max-Age", "3600");
            }
            res.headers.emplace_back("Vary", "Origin");
        }

        void graphics(ApiContext& ctx, ops::HttpRequest const& req, std::vector<std::string> const& p, ops::HttpResponse& res)
        {
            if (p.size() == 1)
            {
                if (req.method != "GET")
                {
                    return problem(res, 405, "GET only");
                }
                picojson::array list;
                for (auto const& g : ctx.library.list())
                {
                    list.push_back(withUse(ctx, g));
                }
                return json(res, 200, picojson::value(picojson::object{{"graphics", picojson::value(list)}}));
            }
            auto const graphic = ctx.library.find(p[1]);
            if (!graphic)
            {
                return problem(res, 404, "no graphic '" + p[1] + "'");
            }
            if (p.size() == 2 && req.method == "GET")
            {
                picojson::object metadata;
                metadata["createdAt"] = picojson::value(graphic->createdAt);
                metadata["updatedAt"] = picojson::value(graphic->updatedAt);
                auto out = picojson::value(picojson::object{{"graphic", graphic->manifest}, {"metadata", picojson::value(metadata)}, {"v_info", withUse(ctx, *graphic)}});
                json(res, 200, out);
                // `graphic` keeps the manifest file's own key order (picojson sorts keys; a form shows
                // fields without `order` in the manifest's order): the file's text replaces it, when it
                // still parses to the same manifest.
                auto const raw = readFile(ctx.library.filePath(graphic->package, graphic->manifestPath));
                picojson::value check;
                if (raw && parseJson(check, *raw).empty() && check == graphic->manifest)
                {
                    out.get<picojson::object>().erase("graphic");
                    auto const rest = out.serialize(); // {"metadata":…,"v_info":…}
                    res.body = "{\"graphic\":" + *raw + "," + rest.substr(1);
                }
                return;
            }
            if (p.size() == 2 && req.method == "DELETE")
            {
                if (ops::queryValue(req.query, "force") == "true")
                {
                    ctx.layers.clearWhere(std::nullopt, graphic->id, "");
                }
                ctx.library.remove(graphic->id, [&ctx] { return ctx.layers.packagesInUse(); });
                log::info("ograf_delete", {{"id", graphic->id}, {"client", req.peer}});
                return json(res, 200, picojson::value(picojson::object{}));
            }
            if (p.size() == 3 && p[2] == "thumbnail" && req.method == "GET")
            {
                auto const file = ops::queryValue(req.query, "file");
                if (file.empty())
                {
                    return problem(res, 400, "file is required");
                }
                bool listed = false;
                if (auto const& t = graphic->manifest.get("thumbnails"); t.is<picojson::array>())
                {
                    for (auto const& item : t.get<picojson::array>())
                    {
                        listed = listed || item.get("file").to_str() == file;
                    }
                }
                auto const path = listed ? ctx.library.filePath(graphic->package, packagePath(graphic->manifestDir(), file)) : std::string{};
                auto const bytes = path.empty() ? std::nullopt : readFile(path);
                if (!bytes)
                {
                    return problem(res, 404, "no thumbnail '" + file + "'");
                }
                res.status = 200;
                res.contentType = contentTypeOf(path);
                res.body = *bytes;
                res.headers.emplace_back("Cache-Control", "max-age=3600");
                return;
            }
            problem(res, 404, "not found");
        }

        void renderers(ApiContext& ctx, ops::HttpRequest const& req, std::vector<std::string> const& p, ops::HttpResponse& res)
        {
            if (p.size() == 1)
            {
                return json(res, 200, picojson::value(picojson::object{{"renderers", picojson::value(picojson::array{rendererInfo(ctx, false)})}}));
            }
            if (p[1] != kRenderer)
            {
                return problem(res, 404, "no renderer '" + p[1] + "' (this one is 'main')");
            }
            if (p.size() == 2)
            {
                return json(res, 200, picojson::value(picojson::object{{"renderer", rendererInfo(ctx, true)}}));
            }
            if (p.size() == 4 && p[2] == "customActions")
            {
                return problem(res, 404, "the renderer has no custom actions");
            }
            if (p[2] != "target")
            {
                return problem(res, 404, "not found");
            }
            if (p.size() == 3)
            {
                picojson::value t;
                parseJson(t, ops::queryValue(req.query, "renderTarget"));
                auto const layer = target(t);
                if (!layer)
                {
                    return problem(res, 404, "renderTarget must be {\"layer\": 1…8}");
                }
                return json(res, 200, targetInfo(ctx, ctx.layers.snapshot()[static_cast<std::size_t>(*layer - 1)]));
            }
            if (p.size() < 5 || p[3] != "graphicInstance")
            {
                return problem(res, 404, "not found");
            }
            picojson::value body;
            if (auto const err = parseJson(body, req.body); !err.empty() || !body.is<picojson::object>())
            {
                return problem(res, 400, "the body must be a JSON object");
            }
            auto const& verb = p[4];
            if (verb == "clear")
            {
                if (req.method != "PUT")
                {
                    return problem(res, 405, "PUT only");
                }
                picojson::array cleared;
                auto const& filters = body.get("filters");
                std::vector<picojson::value> list = filters.is<picojson::array>() ? filters.get<picojson::array>() : std::vector<picojson::value>{};
                if (list.empty())
                {
                    list.emplace_back(picojson::object{}); // no filter: every instance
                }
                for (auto const& f : list)
                {
                    if (!f.is<picojson::object>())
                    {
                        continue;
                    }
                    auto const text = [&](char const* key) { return f.get(key).is<std::string>() ? f.get(key).to_str() : std::string{}; };
                    // A part that is set but not a string matches nothing (instead of anything).
                    if ((f.contains("graphicId") && !f.get("graphicId").is<std::string>()) ||
                        (f.contains("graphicInstanceId") && !f.get("graphicInstanceId").is<std::string>()))
                    {
                        continue;
                    }
                    std::optional<int> layer;
                    if (f.contains("renderTarget"))
                    {
                        layer = target(f.get("renderTarget"));
                        if (!layer)
                        {
                            continue;
                        }
                    }
                    for (auto const& l : ctx.layers.clearWhere(layer, text("graphicId"), text("graphicInstanceId")))
                    {
                        cleared.push_back(picojson::value(picojson::object{{"renderTarget", targetJson(l.number)}, {"graphicInstanceId", picojson::value(l.instanceId)}}));
                    }
                }
                return json(res, 200, picojson::value(picojson::object{{"graphicInstances", picojson::value(cleared)}}));
            }
            if (req.method != "POST")
            {
                return problem(res, 405, "POST only");
            }
            auto const layer = target(body.get("renderTarget"));
            if (!layer)
            {
                return problem(res, 404, "renderTarget must be {\"layer\": 1…8}");
            }
            if (auto const why = ctx.rendererProblem ? ctx.rendererProblem() : std::string{}; !why.empty())
            {
                return problem(res, 409, why);
            }
            auto const& params = body.get("params");
            auto const instance = body.get("graphicInstanceId").is<std::string>() ? body.get("graphicInstanceId").to_str() : std::string{};
            if (verb == "load")
            {
                if (!body.get("graphicId").is<std::string>())
                {
                    return problem(res, 400, "graphicId is required");
                }
                log::info("ograf_load", {{"layer", *layer}, {"graphic", body.get("graphicId").to_str()}, {"client", req.peer}});
                return outcome(res, ctx.layers.load(*layer, body.get("graphicId").to_str(), params.is<picojson::object>() ? params.get("data") : picojson::value()));
            }
            if (instance.empty())
            {
                return problem(res, 400, "graphicInstanceId is required");
            }
            if (verb == "playAction" || verb == "stopAction" || verb == "updateAction")
            {
                return outcome(res, ctx.layers.action(*layer, instance, verb, params));
            }
            if (verb == "customActions" && p.size() == 6)
            {
                return outcome(res, ctx.layers.action(*layer, instance, "customAction", params, p[5]));
            }
            problem(res, 404, "not found");
        }
    }

    bool isOgrafPath(std::string_view path)
    {
        return path == kBase || path.starts_with("/ograf/v1/") || path.starts_with("/api/v1/ograf/");
    }

    bool isUploadRequest(ops::HttpRequest const& req)
    {
        return req.method == "POST" && req.path == "/api/v1/ograf/graphics";
    }

    void unauthorized(ApiContext const& ctx, ops::HttpRequest const& req, ops::HttpResponse& res)
    {
        if (!req.path.starts_with("/api/v1/"))
        {
            cors(ctx, req, res);
        }
        problem(res, 401, "token required (Authorization: Bearer …)");
    }

    void handleOgraf(ApiContext& ctx, ops::HttpRequest const& req, ops::HttpResponse& res)
    {
        if (req.path.starts_with("/api/v1/ograf/"))
        {
            if (req.path == "/api/v1/ograf/layers" && req.method == "GET")
            {
                picojson::array layers;
                for (auto const& l : ctx.layers.snapshot())
                {
                    layers.push_back(layerJson(l));
                }
                return json(res, 200, picojson::value(picojson::object{{"layers", picojson::value(layers)}}));
            }
            if (isUploadRequest(req))
            {
                if (req.bodyFile.empty())
                {
                    return problem(res, 400, "send the ZIP as the request body");
                }
                auto const result = ctx.library.addZip(req.bodyFile, [&ctx] { return ctx.layers.packagesInUse(); });
                log::info("ograf_upload", {{"status", result.status}, {"client", req.peer}});
                if (ctx.metrics)
                {
                    ctx.metrics->inc("ograf_uploads_total", {{"result", result.status == 200 ? "ok" : "rejected"}});
                }
                return json(res, result.status, result.body);
            }
            return problem(res, 404, "not found");
        }

        cors(ctx, req, res);
        if (req.method == "OPTIONS")
        {
            res.status = 204;
            return;
        }
        auto const p = split(std::string_view(req.path).substr(kBase.size()));
        if (p.empty())
        {
            if (req.method != "GET")
            {
                return problem(res, 405, "GET only");
            }
            return json(res, 200, picojson::value(picojson::object{{"name", picojson::value(ctx.name)}, {"description", picojson::value("mxl-browser-source OGraf renderer")},
                                      {"version", picojson::value(ctx.version)}}));
        }
        if (p[0] == "graphics")
        {
            return graphics(ctx, req, p, res);
        }
        if (p[0] == "renderers")
        {
            return renderers(ctx, req, p, res);
        }
        problem(res, 404, "not found");
    }
}
