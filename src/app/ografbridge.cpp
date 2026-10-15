// SPDX-License-Identifier: MIT
#include "app/ografbridge.hpp"

#include "cef/runtime.hpp"

namespace mbs::app
{
    OgrafBridge::OgrafBridge(cef::Browser& browser, std::chrono::milliseconds timeout, util::Metrics& metrics)
        : _browser(browser)
        , _timeout(timeout)
        , _metrics(metrics)
    {
    }

    ograf::CallResult OgrafBridge::call(int layer, std::string const& method, picojson::value const& args)
    {
        auto const start = std::chrono::steady_clock::now();
        auto r = callOnce(layer, method, args);
        static constexpr char const* kResult[] = {"ok", "error", "timeout", "unavailable"};
        auto const action = method == "playAction" ? "play" : method == "stopAction" ? "stop" : method == "updateAction" ? "update" : method == "customAction" ? "custom" : method == "dispose" ? "clear" : method;
        _metrics.inc("ograf_actions_total", {{"action", action}, {"result", kResult[static_cast<int>(r.kind)]}});
        _metrics.observe("ograf_action_seconds", {{"action", action}}, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        return r;
    }

    ograf::CallResult OgrafBridge::callOnce(int layer, std::string const& method, picojson::value const& args)
    {
        using Kind = ograf::CallResult::Kind;
        if (!_available.load())
        {
            return {Kind::Unavailable, {}, "no renderer page"};
        }
        picojson::object params;
        params["expression"] = picojson::value("window.ografRenderer.call(" + std::to_string(layer) + "," + picojson::value(method).serialize() + "," + args.serialize() + ")");
        params["awaitPromise"] = picojson::value(true);
        params["returnByValue"] = picojson::value(true);
        auto const pending = std::make_shared<Pending>();
        std::unique_lock lock{_mu};
        // Registered under the lock: deliver() waits for it, so an early answer is not lost.
        int const id = _browser.devToolsMethod("Runtime.evaluate", picojson::value(params).serialize());
        _pending.emplace(id, pending);
        bool const settled = _cv.wait_for(lock, _timeout, [&] { return pending->done; });
        _pending.erase(id);
        if (!settled)
        {
            return {Kind::Timeout, {}, "no answer in " + std::to_string(_timeout.count()) + " ms"};
        }
        return pending->result;
    }

    bool OgrafBridge::deliver(int id, bool success, std::string const& text)
    {
        using Kind = ograf::CallResult::Kind;
        std::lock_guard lock{_mu};
        auto const it = _pending.find(id);
        if (it == _pending.end())
        {
            return false;
        }
        auto& r = it->second->result;
        picojson::value doc;
        if (!success || !picojson::parse(doc, text).empty() || !doc.is<picojson::object>())
        {
            r = {Kind::Unavailable, {}, "DevTools: " + text};
        }
        else if (doc.contains("exceptionDetails"))
        {
            auto const& ex = doc.get("exceptionDetails");
            auto const description = ex.get("exception").get("description");
            r = {Kind::Threw, {}, description.is<std::string>() ? description.get<std::string>() : ex.get("text").to_str()};
        }
        else
        {
            r = {Kind::Ok, doc.get("result").get("value"), {}};
        }
        it->second->done = true;
        _cv.notify_all();
        return true;
    }

    void OgrafBridge::setAvailable(bool available)
    {
        _available.store(available);
        if (!available)
        {
            std::lock_guard lock{_mu};
            for (auto& [id, pending] : _pending)
            {
                pending->result = {ograf::CallResult::Kind::Unavailable, {}, "the renderer page went away"};
                pending->done = true;
            }
            _cv.notify_all();
        }
    }
}
