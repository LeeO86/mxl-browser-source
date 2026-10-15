// SPDX-License-Identifier: MIT
#include "ograf/layers.hpp"

#include "ograf/files.hpp"
#include "util/logging.hpp"

namespace mbs::ograf
{
    namespace
    {
        picojson::value object(std::initializer_list<std::pair<char const*, picojson::value>> items)
        {
            picojson::object o;
            for (auto const& [k, v] : items)
            {
                o[k] = v;
            }
            return picojson::value(o);
        }

        // RFC 7807 problem details, as the Server API's ErrorResponse.
        Outcome problem(int status, std::string const& title, std::string const& detail)
        {
            return {status, object({{"status", picojson::value(static_cast<double>(status))}, {"title", picojson::value(title)}, {"detail", picojson::value(detail)}})};
        }

        Outcome fromCall(CallResult const& r, std::string const& what)
        {
            switch (r.kind)
            {
            case CallResult::Kind::Threw: return problem(550, "Graphic error", what + "() threw: " + r.error);
            case CallResult::Kind::Timeout: return problem(504, "Graphic timeout", what + "() did not settle in time");
            case CallResult::Kind::Unavailable: return problem(409, "No renderer", "the OGraf renderer page is not running (mode page, loading or recovering)");
            case CallResult::Kind::Ok: break;
            }
            return {};
        }

        int statusCode(picojson::value const& v)
        {
            return v.is<picojson::object>() && v.get("statusCode").is<double>() ? static_cast<int>(v.get("statusCode").get<double>()) : 200;
        }

        picojson::value merge(picojson::value const& base, picojson::value const& update)
        {
            if (!base.is<picojson::object>() || !update.is<picojson::object>())
            {
                return update;
            }
            auto out = base.get<picojson::object>();
            for (auto const& [k, v] : update.get<picojson::object>())
            {
                out[k] = v;
            }
            return picojson::value(out);
        }

        picojson::value characteristics(RenderInfo const& r)
        {
            return object({{"resolution", object({{"width", picojson::value(static_cast<double>(r.width))}, {"height", picojson::value(static_cast<double>(r.height))}})},
                {"frameRate", picojson::value(r.frameRate)},
                {"accessToPublicInternet", picojson::value(r.internet)}});
        }

        bool validLayer(int layer)
        {
            return layer >= 1 && layer <= kLayers;
        }
    }

    LayerSet::LayerSet(Renderer& renderer, Library& library, std::string statePath, RenderInfo render)
        : _renderer(renderer)
        , _library(library)
        , _statePath(std::move(statePath))
        , _render(render)
    {
        for (int i = 0; i < kLayers; ++i)
        {
            _layers[static_cast<std::size_t>(i)].number = i + 1;
        }
    }

    void LayerSet::onChange(std::function<void()> callback)
    {
        _onChange = std::move(callback);
    }

    void LayerSet::onRestore(std::function<void(bool ok)> callback)
    {
        _onRestore = std::move(callback);
    }

    void LayerSet::changed()
    {
        if (_onChange)
        {
            _onChange();
        }
    }

    Outcome LayerSet::load(int layer, std::string const& graphicId, picojson::value const& data)
    {
        if (!validLayer(layer))
        {
            return problem(404, "Not found", "no layer " + std::to_string(layer) + " (1 to 8)");
        }
        std::string instance = randomId();
        std::optional<Graphic> graphic;
        // An upload or delete may collect the package between find() and recording it here. The
        // library asks packagesInUse() under its lock just before it deletes, so once the package
        // is recorded it is either kept, or already gone from the library: then look again.
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            graphic = _library.find(graphicId);
            if (!graphic)
            {
                break;
            }
            {
                std::lock_guard lock{_mu};
                auto& l = at(layer);
                l = Layer{};
                l.number = layer;
                l.state = "loading";
                l.graphicId = graphic->id;
                l.graphicName = graphic->name;
                l.package = graphic->package;
                l.stepCount = graphic->stepCount();
                l.instanceId = instance;
                l.data = data;
                l.pending = 1;
            }
            if (_library.findInPackage(graphic->package, graphic->id))
            {
                break;
            }
            graphic.reset();
        }
        if (!graphic)
        {
            std::lock_guard lock{_mu};
            if (at(layer).instanceId == instance)
            {
                at(layer) = Layer{};
                at(layer).number = layer;
            }
            return problem(404, "Not found", "no graphic '" + graphicId + "' in the library");
        }
        changed();
        auto const module = "https://ograf.local/packages/" + graphic->package + "/" + graphic->mainPath();
        auto const r = _renderer.call(layer, "load",
            object({{"module", picojson::value(module)}, {"data", data}, {"renderCharacteristics", characteristics(_render)}}));
        Outcome out = fromCall(r, "load");
        {
            std::lock_guard lock{_mu};
            auto& l = at(layer);
            if (l.instanceId != instance)
            {
                return problem(409, "Replaced", "the layer was cleared or loaded again meanwhile");
            }
            l.pending = 0;
            if (r.kind == CallResult::Kind::Ok && statusCode(r.value) < 400)
            {
                l.state = "ready";
                l.error.clear();
            }
            else
            {
                l.state = "error";
                l.error = r.kind == CallResult::Kind::Ok ? "load() answered " + std::to_string(statusCode(r.value)) : out.body.get("detail").to_str();
            }
            saveLocked();
        }
        changed();
        if (r.kind != CallResult::Kind::Ok)
        {
            return out;
        }
        auto body = r.value.is<picojson::object>() ? r.value.get<picojson::object>() : picojson::object{};
        body["graphicInstanceId"] = picojson::value(instance);
        body["statusCode"] = picojson::value(static_cast<double>(statusCode(r.value)));
        return {200, picojson::value(body)};
    }

    Outcome LayerSet::action(int layer, std::string const& instanceId, std::string const& method, picojson::value const& params, std::string const& actionId)
    {
        if (!validLayer(layer))
        {
            return problem(404, "Not found", "no layer " + std::to_string(layer) + " (1 to 8)");
        }
        picojson::value args = params.is<picojson::object>() ? params : picojson::value(picojson::object{});
        if (method == "customAction")
        {
            auto o = args.get<picojson::object>();
            o["id"] = picojson::value(actionId);
            if (!o.contains("payload"))
            {
                o["payload"] = picojson::value();
            }
            args = picojson::value(o);
        }
        std::string instance;
        {
            std::lock_guard lock{_mu};
            auto& l = at(layer);
            if (l.state == "empty" || (!instanceId.empty() && l.instanceId != instanceId))
            {
                return problem(404, "Not found", "no graphic instance '" + instanceId + "' on layer " + std::to_string(layer));
            }
            ++l.pending;
            instance = l.instanceId;
        }
        changed();
        auto const r = _renderer.call(layer, method, args);
        Outcome out = fromCall(r, method);
        {
            std::lock_guard lock{_mu};
            auto& l = at(layer);
            if (l.instanceId != instance)
            {
                return r.kind == CallResult::Kind::Ok ? problem(409, "Replaced", "the layer was cleared or loaded again meanwhile") : out;
            }
            l.pending = l.pending > 0 ? l.pending - 1 : 0;
            if (r.kind == CallResult::Kind::Ok && statusCode(r.value) < 400)
            {
                l.error.clear();
                if (method == "playAction")
                {
                    auto const& step = r.value.is<picojson::object>() ? r.value.get("currentStep") : picojson::value();
                    if (step.is<double>())
                    {
                        l.step = static_cast<int>(step.get<double>());
                        l.state = "on";
                    }
                    else
                    {
                        l.step.reset();
                        l.state = l.stepCount == 0 ? "on" : "stopped"; // stepCount 0 plays and goes out by itself
                    }
                }
                else if (method == "stopAction")
                {
                    l.step.reset();
                    l.state = "stopped";
                }
                else if (method == "updateAction" && args.get("data").is<picojson::object>())
                {
                    l.data = merge(l.data, args.get("data"));
                    saveLocked();
                }
                else if (method == "updateAction")
                {
                    l.data = args.get("data");
                    saveLocked();
                }
            }
            else if (r.kind == CallResult::Kind::Ok)
            {
                l.error = method + "() answered " + std::to_string(statusCode(r.value));
            }
            else
            {
                l.error = out.body.get("detail").to_str();
            }
        }
        changed();
        if (r.kind != CallResult::Kind::Ok)
        {
            return out;
        }
        auto body = r.value.is<picojson::object>() ? r.value.get<picojson::object>() : picojson::object{};
        body["graphicInstanceId"] = picojson::value(instance);
        body["statusCode"] = picojson::value(static_cast<double>(statusCode(r.value)));
        return {200, picojson::value(body)};
    }

    Outcome LayerSet::clear(int layer)
    {
        if (!validLayer(layer))
        {
            return problem(404, "Not found", "no layer " + std::to_string(layer) + " (1 to 8)");
        }
        bool loaded = false;
        {
            std::lock_guard lock{_mu};
            loaded = at(layer).state != "empty";
            at(layer) = Layer{};
            at(layer).number = layer;
            saveLocked();
        }
        if (loaded)
        {
            changed();
            // dispose() in the page; when the page is gone there is nothing left to dispose.
            auto const r = _renderer.call(layer, "dispose", picojson::value(picojson::object{}));
            if (r.kind == CallResult::Kind::Threw || r.kind == CallResult::Kind::Timeout)
            {
                log::warn("ograf_dispose_failed", {{"layer", layer}, {"error", r.error}});
            }
        }
        return {200, picojson::value(picojson::object{})};
    }

    std::vector<Layer> LayerSet::clearWhere(std::optional<int> layer, std::string const& graphicId, std::string const& instanceId)
    {
        std::vector<Layer> matches;
        for (auto const& l : snapshot())
        {
            if (l.state != "empty" && (!layer || *layer == l.number) && (graphicId.empty() || graphicId == l.graphicId) &&
                (instanceId.empty() || instanceId == l.instanceId))
            {
                matches.push_back(l);
            }
        }
        for (auto const& l : matches)
        {
            clear(l.number);
        }
        return matches;
    }

    std::vector<Layer> LayerSet::snapshot() const
    {
        std::lock_guard lock{_mu};
        return {_layers.begin(), _layers.end()};
    }

    std::set<std::string> LayerSet::packagesInUse() const
    {
        std::lock_guard lock{_mu};
        std::set<std::string> out;
        for (auto const& l : _layers)
        {
            if (!l.package.empty())
            {
                out.insert(l.package);
            }
        }
        return out;
    }

    bool LayerSet::anyLoaded() const
    {
        std::lock_guard lock{_mu};
        for (auto const& l : _layers)
        {
            if (l.state != "empty")
            {
                return true;
            }
        }
        return false;
    }

    void LayerSet::saveLocked() const
    {
        picojson::array items;
        for (auto const& l : _layers)
        {
            if (l.state != "empty")
            {
                items.push_back(object({{"layer", picojson::value(static_cast<double>(l.number))}, {"graphicId", picojson::value(l.graphicId)},
                    {"package", picojson::value(l.package)}, {"data", l.data}}));
            }
        }
        if (!writeFileAtomic(_statePath, object({{"version", picojson::value(1.0)}, {"layers", picojson::value(items)}}).serialize(true)))
        {
            log::warn("ograf_layers_not_saved", {{"path", _statePath}});
        }
    }

    void LayerSet::loadSaved()
    {
        auto const text = readFile(_statePath);
        picojson::value doc;
        if (!text || !parseJson(doc, *text).empty() || !doc.get("layers").is<picojson::array>())
        {
            return;
        }
        // The library is asked before the layers' lock is taken: the library asks the layers
        // (packagesInUse) under its own lock, so the two are never taken in the other order.
        std::vector<std::pair<picojson::value, Graphic>> found;
        for (auto const& item : doc.get("layers").get<picojson::array>())
        {
            int const n = item.get("layer").is<double>() ? static_cast<int>(item.get("layer").get<double>()) : 0;
            auto const graphic = validLayer(n) ? _library.findInPackage(item.get("package").to_str(), item.get("graphicId").to_str()) : std::nullopt;
            if (graphic)
            {
                found.emplace_back(item, *graphic);
            }
        }
        std::lock_guard lock{_mu};
        for (auto const& [item, g] : found)
        {
            int const n = static_cast<int>(item.get("layer").get<double>());
            auto const* graphic = &g;
            auto& l = at(n);
            l.state = "ready"; // loaded into the page by restore()
            l.graphicId = graphic->id;
            l.graphicName = graphic->name;
            l.package = graphic->package;
            l.stepCount = graphic->stepCount();
            l.instanceId = randomId();
            l.data = item.get("data");
        }
    }

    void LayerSet::restore(bool withSteps)
    {
        for (auto const& before : snapshot())
        {
            if (before.state == "empty")
            {
                continue;
            }
            auto const graphic = _library.findInPackage(before.package, before.graphicId);
            if (!graphic)
            {
                // Its package is gone (it should not be: a layer keeps its package). Say so instead
                // of showing a layer as ready that the new page does not have.
                {
                    std::lock_guard lock{_mu};
                    auto& l = at(before.number);
                    if (l.instanceId == before.instanceId)
                    {
                        l.state = "error";
                        l.pending = 0;
                        l.step.reset();
                        l.error = "restore failed: graphic '" + before.graphicId + "' is no longer in the library";
                    }
                }
                log::warn("ograf_layer_restored", {{"layer", before.number}, {"graphic", before.graphicId}, {"state", "error"}, {"error", "graphic missing"}});
                if (_onRestore)
                {
                    _onRestore(false);
                }
                changed();
                continue;
            }
            auto const module = "https://ograf.local/packages/" + graphic->package + "/" + graphic->mainPath();
            auto const r = _renderer.call(before.number, "load",
                object({{"module", picojson::value(module)}, {"data", before.data}, {"renderCharacteristics", characteristics(_render)}}));
            bool ok = r.kind == CallResult::Kind::Ok && statusCode(r.value) < 400;
            std::string state = ok ? "ready" : "error";
            if (ok && withSteps && before.state == "on" && before.step && before.stepCount != 0)
            {
                auto const p = _renderer.call(before.number, "playAction",
                    object({{"goto", picojson::value(static_cast<double>(*before.step))}, {"skipAnimation", picojson::value(true)}}));
                ok = p.kind == CallResult::Kind::Ok && statusCode(p.value) < 400;
                state = ok ? "on" : "error";
            }
            {
                std::lock_guard lock{_mu};
                auto& l = at(before.number);
                if (l.instanceId == before.instanceId)
                {
                    l.state = state;
                    l.pending = 0;
                    l.error = ok ? std::string{} : "restore failed: " + (r.error.empty() ? std::string("the graphic answered an error") : r.error);
                    if (state != "on")
                    {
                        l.step.reset();
                    }
                }
            }
            log::info("ograf_layer_restored", {{"layer", before.number}, {"graphic", before.graphicId}, {"state", state}});
            if (_onRestore)
            {
                _onRestore(ok);
            }
            changed();
        }
    }

    void LayerSet::pageLost()
    {
        {
            std::lock_guard lock{_mu};
            for (auto& l : _layers)
            {
                l.pending = 0;
            }
        }
        changed();
    }

    void LayerSet::forgetAll()
    {
        {
            std::lock_guard lock{_mu};
            for (auto& l : _layers)
            {
                int const number = l.number;
                l = Layer{};
                l.number = number;
            }
            saveLocked();
        }
        changed();
    }

    picojson::value layerJson(Layer const& l)
    {
        picojson::object o;
        o["layer"] = picojson::value(static_cast<double>(l.number));
        o["state"] = picojson::value(l.state);
        if (l.state != "empty")
        {
            o["graphicId"] = picojson::value(l.graphicId);
            o["graphicName"] = picojson::value(l.graphicName);
            o["package"] = picojson::value(l.package);
            o["graphicInstanceId"] = picojson::value(l.instanceId);
            o["stepCount"] = picojson::value(static_cast<double>(l.stepCount));
            o["data"] = l.data;
        }
        o["step"] = l.step ? picojson::value(static_cast<double>(*l.step)) : picojson::value();
        o["pending"] = picojson::value(static_cast<double>(l.pending));
        o["error"] = picojson::value(l.error);
        return picojson::value(o);
    }
}
