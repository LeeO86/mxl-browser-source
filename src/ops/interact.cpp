// SPDX-License-Identifier: MIT
#include "ops/interact.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "picojson/picojson.h"

namespace mbs::ops
{
    namespace
    {
        using Outbox = std::vector<std::pair<std::function<bool(std::string const&)>, std::string>>;

        void flush(Outbox& out)
        {
            for (auto& [send, text] : out)
            {
                if (send)
                {
                    send(text);
                }
            }
            out.clear();
        }

        std::string errorJson(std::string const& code, std::string const& message)
        {
            picojson::object o;
            o["type"] = picojson::value("error");
            o["code"] = picojson::value(code);
            o["message"] = picojson::value(message);
            return picojson::value(o).serialize();
        }

        double number(picojson::object const& o, char const* key, double fallback)
        {
            auto const it = o.find(key);
            return it != o.end() && it->second.is<double>() ? it->second.get<double>() : fallback;
        }

        std::string text(picojson::object const& o, char const* key)
        {
            auto const it = o.find(key);
            return it != o.end() && it->second.is<std::string>() ? it->second.get<std::string>() : std::string{};
        }

        bool flag(picojson::object const& o, char const* key)
        {
            auto const it = o.find(key);
            return it != o.end() && it->second.is<bool>() && it->second.get<bool>();
        }

        // `modifiers` as flags; false when present but not an array of known names.
        bool modifiers(picojson::object const& o, std::uint32_t& flags)
        {
            auto const it = o.find("modifiers");
            if (it == o.end())
            {
                return true;
            }
            if (!it->second.is<picojson::array>())
            {
                return false;
            }
            std::vector<std::string> names;
            for (auto const& v : it->second.get<picojson::array>())
            {
                if (!v.is<std::string>())
                {
                    return false;
                }
                names.push_back(v.get<std::string>());
            }
            return modifierFlags(names, flags);
        }
    }

    InteractHub::InteractHub(InteractSettings settings, PageInput& page)
        : _settings(settings)
        , _page(page)
    {
    }

    void InteractHub::setStateParts(std::function<std::string()> page, std::function<std::string()> render)
    {
        std::lock_guard lock{_mutex};
        _pagePart = std::move(page);
        _renderPart = std::move(render);
    }

    void InteractHub::setGrainSource(std::function<std::uint64_t()> nextGrain)
    {
        std::lock_guard lock{_mutex};
        _nextGrain = std::move(nextGrain);
    }

    void InteractHub::setViewSize(int width, int height)
    {
        std::lock_guard lock{_mutex};
        _settings.viewWidth = width;
        _settings.viewHeight = height;
    }

    std::string InteractHub::stateJson(int id) const
    {
        std::string controller = "none";
        if (_controller != 0)
        {
            controller = _controller == id ? "self" : "other";
        }
        std::string out = R"({"type":"state","interact":{"enabled":)";
        out += _controller == id ? "true" : "false";
        out += R"(,"controller":")" + controller + "\"";
        if (_controller == id)
        {
            auto const idle = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - _lastInput).count();
            out += ",\"expires_in_s\":" + std::to_string(std::max<long long>(0, _settings.timeoutS - idle));
        }
        out += "},\"page\":" + (_pagePart ? _pagePart() : std::string("{}"));
        out += ",\"render\":" + (_renderPart ? _renderPart() : std::string("{}"));
        return out + "}";
    }

    void InteractHub::count(std::string const& type, std::string const& result)
    {
        ++_counters.events[type + "/" + result];
    }

    void InteractHub::releaseButtons(Actions& actions)
    {
        for (auto const& [bit, button] : {std::pair{kLeftMouseButton, MouseButton::Left}, std::pair{kMiddleMouseButton, MouseButton::Middle},
                 std::pair{kRightMouseButton, MouseButton::Right}})
        {
            if ((_buttons & bit) != 0)
            {
                _buttons &= ~bit;
                actions.emplace_back([this, x = _lastX, y = _lastY, b = button, held = _buttons] { _page.mouseClick(x, y, b, true, 1, held); });
            }
        }
    }

    void InteractHub::setController(int id, Actions& actions)
    {
        if (_controller != id)
        {
            releaseButtons(actions);
            _controller = id;
        }
    }

    int InteractHub::open(Peer peer)
    {
        Outbox out;
        int id = 0;
        {
            std::lock_guard lock{_mutex};
            id = _nextId++;
            auto& session = _sessions[id];
            session.peer = std::move(peer);
            session.previewFps = _settings.previewFps;
            out.emplace_back(session.peer.sendText, stateJson(id));
        }
        flush(out);
        return id;
    }

    void InteractHub::close(int session)
    {
        Actions actions;
        bool released = false;
        {
            std::lock_guard lock{_mutex};
            _sessions.erase(session);
            if (_controller == session)
            {
                setController(0, actions);
                released = true;
            }
        }
        for (auto const& action : actions)
        {
            action();
        }
        if (released)
        {
            broadcastState();
        }
    }

    void InteractHub::broadcastState()
    {
        Outbox out;
        {
            std::lock_guard lock{_mutex};
            for (auto const& [id, session] : _sessions)
            {
                out.emplace_back(session.peer.sendText, stateJson(id));
            }
        }
        flush(out);
    }

    void InteractHub::broadcast(std::string const& json)
    {
        Outbox out;
        {
            std::lock_guard lock{_mutex};
            for (auto const& [id, session] : _sessions)
            {
                out.emplace_back(session.peer.sendText, json);
            }
        }
        flush(out);
    }

    void InteractHub::tick(Clock::time_point now)
    {
        Actions actions;
        bool expired = false;
        {
            std::lock_guard lock{_mutex};
            if (_controller != 0 && now - _lastInput > std::chrono::seconds(_settings.timeoutS))
            {
                setController(0, actions);
                expired = true;
            }
        }
        for (auto const& action : actions)
        {
            action();
        }
        if (expired)
        {
            broadcastState();
        }
    }

    std::map<int, int> InteractHub::previewRates() const
    {
        std::lock_guard lock{_mutex};
        std::map<int, int> rates;
        for (auto const& [id, session] : _sessions)
        {
            rates[id] = session.previewFps;
        }
        return rates;
    }

    int InteractHub::sessions() const
    {
        std::lock_guard lock{_mutex};
        return static_cast<int>(_sessions.size());
    }

    bool InteractHub::controlled() const
    {
        std::lock_guard lock{_mutex};
        return _controller != 0;
    }

    InteractCounters InteractHub::counters() const
    {
        std::lock_guard lock{_mutex};
        return _counters;
    }

    void InteractHub::message(int id, std::string const& raw, Clock::time_point now)
    {
        Outbox out;
        Actions actions; // page input, run after the lock
        bool stateChanged = false;
        {
            std::lock_guard lock{_mutex};
            auto const found = _sessions.find(id);
            if (found == _sessions.end())
            {
                return;
            }
            Session& session = found->second;
            auto const reject = [&](std::string const& type, std::string const& code, std::string const& why) {
                count(type, "rejected");
                out.emplace_back(session.peer.sendText, errorJson(code, why));
            };

            // Input limit per session: beyond it, messages are dropped (one error per second).
            if (now - session.windowStart >= std::chrono::seconds(1))
            {
                session.windowStart = now;
                session.windowCount = 0;
            }
            if (++session.windowCount > _settings.maxMessagesPerSecond)
            {
                count("any", "dropped");
                if (session.windowCount == _settings.maxMessagesPerSecond + 1)
                {
                    out.emplace_back(session.peer.sendText, errorJson("rate_limited", "too many messages, dropping"));
                }
            }
            else
            {
                picojson::value doc;
                std::string const parseError = picojson::parse(doc, raw);
                if (!parseError.empty() || !doc.is<picojson::object>())
                {
                    reject("invalid", "bad_message", "not a JSON object");
                }
                else
                {
                    auto const& o = doc.get<picojson::object>();
                    std::string const type = text(o, "type");
                    int const vw = _settings.viewWidth;
                    int const vh = _settings.viewHeight;
                    auto const px = [&](char const* key, int size) {
                        return static_cast<int>(std::lround(std::clamp(number(o, key, 0), 0.0, 1.0) * size));
                    };
                    std::uint32_t mods = 0;
                    bool const input = type == "pointer" || type == "wheel" || type == "key" || type == "text" || type == "ime" || type == "focus";
                    if (type == "hello")
                    {
                        out.emplace_back(session.peer.sendText, stateJson(id));
                    }
                    else if (type == "interact")
                    {
                        if (flag(o, "enable"))
                        {
                            if (_controller == 0 || _controller == id || flag(o, "take"))
                            {
                                setController(id, actions);
                                _lastInput = now;
                                stateChanged = true;
                            }
                            else
                            {
                                reject(type, "controlled", "another session controls the page; send take:true to take over");
                            }
                        }
                        else if (_controller == id)
                        {
                            setController(0, actions);
                            stateChanged = true;
                        }
                        else
                        {
                            out.emplace_back(session.peer.sendText, stateJson(id));
                        }
                    }
                    else if (type == "preview")
                    {
                        int const cap = _controller == id ? _settings.maxPreviewFps : _settings.previewFps;
                        session.previewFps = std::clamp(static_cast<int>(number(o, "fps", _settings.previewFps)), 1, cap);
                    }
                    else if (!input)
                    {
                        reject("invalid", "bad_message", "unknown type");
                    }
                    else if (_controller != id)
                    {
                        reject(type, "not_controller", "interaction is off for this session");
                    }
                    else if (!modifiers(o, mods))
                    {
                        reject(type, "bad_message", "unknown modifier");
                    }
                    else
                    {
                        bool ok = true;
                        if (type == "pointer")
                        {
                            std::string const action = text(o, "action");
                            int const x = px("x", vw);
                            int const y = px("y", vh);
                            std::string const b = text(o, "button");
                            MouseButton const button = b == "right" ? MouseButton::Right : b == "middle" ? MouseButton::Middle : MouseButton::Left;
                            int const clicks = std::clamp(static_cast<int>(number(o, "clicks", 1)), 1, 3);
                            std::uint32_t const bit = button == MouseButton::Right ? kRightMouseButton : button == MouseButton::Middle ? kMiddleMouseButton : kLeftMouseButton;
                            _lastX = x;
                            _lastY = y;
                            // Moves carry the held buttons, or Chromium ends a drag (text selection)
                            // at the first move; a leave releases them first.
                            if (action == "move")
                            {
                                actions.emplace_back([this, x, y, held = mods | _buttons] { _page.mouseMove(x, y, held, false); });
                            }
                            else if (action == "leave")
                            {
                                releaseButtons(actions);
                                actions.emplace_back([this, x, y, mods] { _page.mouseMove(x, y, mods, true); });
                            }
                            else if (action == "down" || action == "up")
                            {
                                bool const up = action == "up";
                                _buttons = up ? _buttons & ~bit : _buttons | bit;
                                if (!up)
                                {
                                    actions.emplace_back([this] { _page.focus(true); }); // a click focuses the page
                                }
                                actions.emplace_back([this, x, y, button, up, clicks, held = mods | _buttons] { _page.mouseClick(x, y, button, up, clicks, held); });
                            }
                            else
                            {
                                ok = false;
                            }
                        }
                        else if (type == "wheel")
                        {
                            int const x = px("x", vw);
                            int const y = px("y", vh);
                            auto const dx = static_cast<int>(std::lround(std::clamp(number(o, "dx", 0), -10000.0, 10000.0)));
                            auto const dy = static_cast<int>(std::lround(std::clamp(number(o, "dy", 0), -10000.0, 10000.0)));
                            actions.emplace_back([this, x, y, dx, dy, mods] { _page.mouseWheel(x, y, dx, dy, mods); });
                        }
                        else if (type == "key")
                        {
                            std::string const action = text(o, "action");
                            std::string const code = text(o, "code");
                            if ((action != "down" && action != "up") || code.empty())
                            {
                                ok = false;
                            }
                            else
                            {
                                for (auto const& event : keyEvents(action == "down", code, text(o, "key"), mods, flag(o, "repeat")))
                                {
                                    actions.emplace_back([this, event] { _page.key(event); });
                                }
                            }
                        }
                        else if (type == "text")
                        {
                            std::string const value = text(o, "text");
                            if (value.size() > 4096)
                            {
                                ok = false;
                            }
                            for (char16_t unit : ok ? utf16(value) : std::u16string{})
                            {
                                KeyEvent event;
                                event.type = KeyEventType::Char;
                                event.character = unit;
                                event.unmodifiedCharacter = unit;
                                actions.emplace_back([this, event] { _page.key(event); });
                            }
                        }
                        else if (type == "ime")
                        {
                            std::string const action = text(o, "action");
                            auto const value = utf16(text(o, "text"));
                            if (action == "composition")
                            {
                                int start = static_cast<int>(value.size());
                                int end = start;
                                if (auto const sel = o.find("selection"); sel != o.end() && sel->second.is<picojson::array>())
                                {
                                    auto const& a = sel->second.get<picojson::array>();
                                    if (!a.empty() && a[0].is<double>())
                                    {
                                        start = end = static_cast<int>(a[0].get<double>());
                                    }
                                    if (a.size() > 1 && a[1].is<double>())
                                    {
                                        end = static_cast<int>(a[1].get<double>());
                                    }
                                }
                                actions.emplace_back([this, value, start, end] { _page.imeComposition(value, start, end); });
                            }
                            else if (action == "commit")
                            {
                                actions.emplace_back([this, value] { _page.imeCommit(value); });
                            }
                            else if (action == "finish")
                            {
                                actions.emplace_back([this] { _page.imeFinish(); });
                            }
                            else if (action == "cancel")
                            {
                                actions.emplace_back([this] { _page.imeCancel(); });
                            }
                            else
                            {
                                ok = false;
                            }
                        }
                        else if (type == "focus")
                        {
                            bool const focused = flag(o, "focused");
                            actions.emplace_back([this, focused] { _page.focus(focused); });
                        }
                        if (!ok)
                        {
                            actions.clear();
                            reject(type, "bad_message", "invalid " + type + " message");
                        }
                        else
                        {
                            count(type, "ok");
                            _lastInput = now;
                            if (auto const seq = o.find("seq"); seq != o.end() && seq->second.is<double>())
                            {
                                auto const grain = _nextGrain ? _nextGrain() : 0;
                                out.emplace_back(session.peer.sendText, R"({"type":"ack","seq":)" + std::to_string(static_cast<long long>(seq->second.get<double>())) +
                                                                            R"(,"grain":)" + std::to_string(grain) + "}");
                            }
                        }
                    }
                }
            }
        }
        for (auto const& action : actions)
        {
            action();
        }
        flush(out);
        if (stateChanged)
        {
            broadcastState();
        }
    }
}
