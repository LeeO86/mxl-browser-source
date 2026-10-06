// SPDX-License-Identifier: MIT
// The web port (SPEC §10, docs/api.md): UI, REST, WebSockets, health, metrics, DevTools proxy.
#include "app/application.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <regex>
#include <sstream>
#include <string_view>

#include "app/mxladapters.hpp"
#include "include/cef_version.h"
#include "mxlio/setup.hpp"
#include "nmos/node.hpp"
#include "picojson/picojson.h"
#include "util/logging.hpp"
#include "util/net.hpp"
#include "util/proc.hpp"
#include "webui_index.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb/stb_image_write.h"

#ifndef MBS_MXL_REVISION
#define MBS_MXL_REVISION "unknown"
#endif
#ifndef MBS_NMOS_CPP_REF
#define MBS_NMOS_CPP_REF "unknown"
#endif

namespace mbs::app
{
    namespace
    {
        namespace fs = std::filesystem;
        using picojson::array;
        using picojson::object;
        using picojson::value;

        std::string json(std::string const& text)
        {
            return value(text).serialize();
        }

        void reply(ops::HttpResponse& res, int status, std::string body, std::string const& type = "application/json; charset=utf-8")
        {
            res.status = status;
            res.contentType = type;
            res.body = std::move(body);
        }

        void error(ops::HttpResponse& res, int status, std::string const& message)
        {
            reply(res, status, R"({"error":)" + json(message) + "}");
        }

        // The request body as a JSON object; false (and a 400) when it is not one.
        bool bodyObject(ops::HttpRequest const& req, ops::HttpResponse& res, object& out)
        {
            value doc;
            if (req.body.empty())
            {
                out = object{};
                return true;
            }
            if (!picojson::parse(doc, req.body).empty() || !doc.is<object>())
            {
                error(res, 400, "body must be a JSON object");
                return false;
            }
            out = doc.get<object>();
            return true;
        }

        bool startsWith(std::string const& text, std::string const& prefix)
        {
            return text.rfind(prefix, 0) == 0;
        }

        std::string cookie(ops::HttpRequest const& req, std::string const& name)
        {
            std::istringstream in(req.header("Cookie"));
            std::string part;
            while (std::getline(in, part, ';'))
            {
                auto const start = part.find_first_not_of(' ');
                if (start == std::string::npos)
                {
                    continue;
                }
                part = part.substr(start);
                if (startsWith(part, name + "="))
                {
                    return part.substr(name.size() + 1);
                }
            }
            return {};
        }

        // Constant-time comparison of the token.
        bool sameSecret(std::string const& a, std::string const& b)
        {
            if (a.size() != b.size())
            {
                return false;
            }
            unsigned char diff = 0;
            for (std::size_t i = 0; i < a.size(); ++i)
            {
                diff |= static_cast<unsigned char>(a[i] ^ b[i]);
            }
            return diff == 0;
        }

        std::string sourceJson(config::Source const& s)
        {
            return config::sourceToJson(s).serialize();
        }

        // `^[A-Za-z_$][A-Za-z0-9_$.]*$` (SPEC §4.4).
        bool validFunctionName(std::string const& name)
        {
            static std::regex const pattern(R"(^[A-Za-z_$][A-Za-z0-9_$.]*$)");
            return !name.empty() && name.size() <= 128 && std::regex_match(name, pattern);
        }

        // Copies bytes between the two sockets until one side closes.
        void pump(int a, int b)
        {
            pollfd fds[2] = {{a, POLLIN, 0}, {b, POLLIN, 0}};
            char buf[65536];
            while (true)
            {
                if (::poll(fds, 2, 30000) <= 0)
                {
                    continue;
                }
                for (int i = 0; i < 2; ++i)
                {
                    if ((fds[i].revents & (POLLIN | POLLHUP | POLLERR)) == 0)
                    {
                        continue;
                    }
                    ssize_t const n = ::recv(fds[i].fd, buf, sizeof(buf), 0);
                    if (n <= 0 || !ops::writeAll(fds[1 - i].fd, std::string_view(buf, static_cast<std::size_t>(n))))
                    {
                        return;
                    }
                }
            }
        }
    }

    std::string encodeJpeg(std::vector<std::uint8_t> const& rgb, int width, int height, int quality)
    {
        std::string out;
        if (width <= 0 || height <= 0 || rgb.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3)
        {
            return out;
        }
        stbi_write_jpg_to_func(
            [](void* context, void* data, int size) { static_cast<std::string*>(context)->append(static_cast<char const*>(data), static_cast<std::size_t>(size)); },
            &out, width, height, 3, rgb.data(), quality);
        return out;
    }

    bool Application::authorized(ops::HttpRequest const& req) const
    {
        if (_cfg.apiToken.empty())
        {
            return true;
        }
        auto const header = req.header("Authorization");
        if (startsWith(header, "Bearer ") && sameSecret(header.substr(7), _cfg.apiToken))
        {
            return true;
        }
        // Browsers cannot set headers on a WebSocket or a link: ?token= and, for DevTools, a cookie.
        if (auto const q = ops::queryValue(req.query, "token"); !q.empty() && sameSecret(q, _cfg.apiToken))
        {
            return true;
        }
        auto const c = cookie(req, "mbs_token");
        return !c.empty() && sameSecret(ops::urlDecode(c), _cfg.apiToken);
    }

    std::string Application::infoJson() const
    {
        std::string const requested = _cfg.render == config::Render::Auto ? "auto" : _cfg.render == config::Render::Gpu ? "gpu" : "software";
        auto const render = renderJson();
        return std::string(R"({"version":)") + json(MBS_VERSION) + R"(,"cef":)" + json(CEF_VERSION) + R"(,"chromium":)" +
               json(std::to_string(CHROME_VERSION_MAJOR) + "." + std::to_string(CHROME_VERSION_MINOR) + "." + std::to_string(CHROME_VERSION_BUILD) + "." +
                    std::to_string(CHROME_VERSION_PATCH)) +
               R"(,"mxl_revision":)" + json(MBS_MXL_REVISION) + R"(,"nmos_cpp":)" + json(MBS_NMOS_CPP_REF) + R"(,"format":{"name":)" + json(_cfg.format.name) +
               R"(,"width":)" + std::to_string(_cfg.format.width) + R"(,"height":)" + std::to_string(_cfg.format.height) + R"(,"rate":")" +
               std::to_string(_cfg.format.rateNum) + "/" + std::to_string(_cfg.format.rateDen) + R"("},"key_mode":)" + json(config::toString(_cfg.keyMode)) +
               R"(,"audio_channels":)" + std::to_string(_cfg.audioChannels) + R"(,"render":{"requested":)" + json(requested) + R"(,"actual":)" + render +
               R"(},"label":)" + json(_cfg.label) + R"(,"devtools":)" + (_cfg.devtools ? "true" : "false") + R"(,"token":)" +
               (_cfg.apiToken.empty() ? "false" : "true") + "}";
    }

    std::string Application::nmosJson() const
    {
        auto const senders = _doc->senders();
        auto enabled = [&](char const* name) {
            auto const it = senders.find(name);
            return it == senders.end() || it->second ? "true" : "false";
        };
        std::string out = R"({"registered":)" + std::string(_registered.load() ? "true" : "false") + R"(,"node_id":")" + _ids.node + R"(","device_id":")" +
                          _ids.device + R"(","domain_id":")" + _domainId + R"(","senders":{"video":{"id":")" + _ids.videoSender + R"(","flow_id":")" +
                          _ids.videoFlow + R"(","enabled":)" + enabled("video") + "}";
        if (_cfg.keyMode == config::KeyMode::FillKey)
        {
            out += R"(,"key":{"id":")" + _ids.keySender + R"(","flow_id":")" + _ids.keyFlow + R"(","enabled":)" + enabled("key") + "}";
        }
        if (_cfg.audioChannels > 0)
        {
            out += R"(,"audio":{"id":")" + _ids.audioSender + R"(","flow_id":")" + _ids.audioFlow + R"(","enabled":)" + enabled("audio") + "}";
        }
        return out + "}}";
    }

    std::string Application::statusJson() const
    {
        auto const stats = _engine->stats();
        std::string reason;
        bool const isReady = ready(&reason);
        std::string peaks = "[";
        for (std::size_t i = 0; i < stats.audioPeakDbfs.size(); ++i)
        {
            peaks += (i == 0 ? "" : ",") + std::to_string(stats.audioPeakDbfs[i]);
        }
        peaks += "]";
        bool stream = false;
        {
            std::lock_guard lock{_pageMutex};
            stream = _audioStream;
        }
        return std::string(R"({"type":"status","ready":)") + (isReady ? "true" : "false") + R"(,"not_ready":)" + json(reason) + R"(,"page":)" + pageJson() +
               R"(,"render":)" + renderJson() + R"(,"grains":{"video":)" + std::to_string(stats.tick.grains) + R"(,"key":)" +
               std::to_string(_cfg.keyMode == config::KeyMode::FillKey ? stats.tick.grains : 0) + R"(,"repeated":)" +
               std::to_string(stats.tick.repeated + stats.tick.repeatedHold) + R"(,"missed":)" + std::to_string(stats.tick.missed) + R"(,"late_paints":)" +
               std::to_string(stats.latePaints) + R"(,"begin_frames":)" + std::to_string(stats.tick.beginFrames) + R"(},"audio":{"channels":)" +
               std::to_string(_cfg.audioChannels) + R"(,"stream":)" + (stream ? "true" : "false") + R"(,"drift_ppm":)" + std::to_string(stats.audio.driftPpm) +
               R"(,"buffer_ms":)" + std::to_string(stats.audio.fillSeconds * 1000.0) + R"(,"peaks_dbfs":)" + peaks + R"(,"underruns":)" +
               std::to_string(stats.audio.underruns) + R"(,"overruns":)" + std::to_string(stats.audio.overruns) + R"(},"interact":{"sessions":)" +
               std::to_string(_interact->sessions()) + R"(,"controlled":)" + (_interact->controlled() ? "true" : "false") + R"(},"nmos":)" + nmosJson() +
               R"(,"devtools":{"enabled":)" + (_cfg.devtools ? "true" : "false") + R"(,"sessions":)" + std::to_string(_devtoolsSessions.load()) + "}}";
    }

    std::string Application::metricsText()
    {
        auto const s = _engine->stats();
        auto& m = _metrics;
        m.set("info",
            {{"version", MBS_VERSION}, {"cef", CEF_VERSION}, {"chromium", std::to_string(CHROME_VERSION_MAJOR)}, {"mxl_revision", MBS_MXL_REVISION},
                {"format", _cfg.format.name}, {"key_mode", config::toString(_cfg.keyMode)}},
            1);
        m.setCounter("grains_total", {{"flow", "video"}}, static_cast<double>(s.tick.grains));
        if (_cfg.keyMode == config::KeyMode::FillKey)
        {
            m.setCounter("grains_total", {{"flow", "key"}}, static_cast<double>(s.tick.grains));
        }
        m.setCounter("repeated_grains_total", {{"reason", "late"}}, static_cast<double>(s.tick.repeated));
        m.setCounter("repeated_grains_total", {{"reason", "hold"}}, static_cast<double>(s.tick.repeatedHold));
        m.setCounter("missed_grains_total", {}, static_cast<double>(s.tick.missed));
        m.setCounter("begin_frames_total", {}, static_cast<double>(s.tick.beginFrames));
        m.setCounter("late_paints_total", {}, static_cast<double>(s.latePaints));
        m.setCounter("paints_dropped_total", {}, static_cast<double>(s.paintsDropped));
        m.setCounter("mxl_write_failures_total", {}, _outputs ? static_cast<double>(_outputs->failures()) : 0.0);
        if (_cfg.audioChannels > 0)
        {
            m.setCounter("audio_samples_total", {}, static_cast<double>(s.audio.samples));
            m.setCounter("audio_underruns_total", {}, static_cast<double>(s.audio.underruns));
            m.setCounter("audio_overruns_total", {}, static_cast<double>(s.audio.overruns));
            m.setCounter("audio_resets_total", {}, static_cast<double>(s.audio.resets));
            m.set("audio_drift_ppm", {}, s.audio.driftPpm);
            m.set("audio_buffer_seconds", {}, s.audio.fillSeconds);
            for (std::size_t c = 0; c < s.audioPeakDbfs.size(); ++c)
            {
                m.set("audio_peak_dbfs", {{"channel", std::to_string(c + 1)}}, s.audioPeakDbfs[c]);
            }
        }
        m.set("interact_sessions", {}, _interact->controlled() ? 1 : 0);
        for (auto const& [key, count] : _interact->counters().events)
        {
            auto const slash = key.find('/');
            m.setCounter("interaction_events_total", {{"type", key.substr(0, slash)}, {"result", key.substr(slash + 1)}}, static_cast<double>(count));
        }
        m.set("devtools_sessions", {}, _devtoolsSessions.load());
        // Every sender of this configuration, enabled unless a stored IS-05 state says otherwise
        // (the document only holds states that were changed).
        auto const stored = _doc->senders();
        std::vector<std::string> names{"video"};
        if (_cfg.keyMode == config::KeyMode::FillKey)
        {
            names.emplace_back("key");
        }
        if (_cfg.audioChannels > 0)
        {
            names.emplace_back("audio");
        }
        for (auto const& name : names)
        {
            auto const it = stored.find(name);
            m.set("sender_enabled", {{"sender", name}}, it == stored.end() || it->second ? 1 : 0);
        }
        // CPU and memory (SPEC §12): the CEF processes under ours, and the standard process_* names.
        auto const cef = util::descendantsUsage();
        m.setCounter("cef_processes_cpu_seconds_total", {}, cef.cpuSeconds);
        m.set("cef_processes_resident_bytes", {}, static_cast<double>(cef.residentBytes));
        auto const self = util::selfUsage();
        return m.render() + "# TYPE process_cpu_seconds_total counter\nprocess_cpu_seconds_total " + std::to_string(self.cpuSeconds) +
               "\n# TYPE process_resident_memory_bytes gauge\nprocess_resident_memory_bytes " + std::to_string(self.residentBytes) + "\n";
    }

    std::string Application::configJson() const
    {
        // The table as loaded, with the file layer's current values for keys the environment
        // does not set; secrets show only whether they are set.
        auto const file = _doc->settings();
        std::string out = R"({"settings":[)";
        bool first = true;
        for (auto const& row : _table)
        {
            std::string value = row.value;
            std::string source = row.source;
            if (source != "env")
            {
                if (auto const it = file.find(row.key); it != file.end())
                {
                    value = it->second;
                    source = "file";
                }
            }
            out += (first ? "" : ",") + std::string(R"({"key":)") + json(row.key) + R"(,"value":)" + json(row.secret ? "" : value) + R"(,"default":)" +
                   json(row.defaultValue) + R"(,"source":)" + json(source) + R"(,"restart":)" + (row.restart ? "true" : "false") + R"(,"secret":)" +
                   (row.secret ? "true" : "false") + (row.secret ? std::string(R"(,"set":)") + (value.empty() ? "false" : "true") : std::string()) +
                   R"(,"description":)" + json(row.description) + "}";
            first = false;
        }
        return out + R"(],"file":)" + json(_doc->path()) + "}";
    }

    std::optional<ops::WebSocketHandlers> Application::upgrade(ops::HttpRequest const& req, ops::HttpResponse& reject)
    {
        if (req.path != "/api/v1/events" && req.path != "/api/v1/interact")
        {
            error(reject, 404, "not found");
            return std::nullopt;
        }
        if (!authorized(req))
        {
            error(reject, 401, "token required");
            return std::nullopt;
        }
        if (_stopping.load())
        {
            error(reject, 503, "stopping");
            return std::nullopt;
        }
        ops::WebSocketHandlers h;
        if (req.path == "/api/v1/events")
        {
            h.open = [this](std::shared_ptr<ops::WebSocket> const& socket) {
                {
                    std::lock_guard lock{_socketsMutex};
                    _eventSockets.insert(socket);
                }
                socket->sendText(statusJson());
            };
            h.close = [this](std::shared_ptr<ops::WebSocket> const& socket) {
                std::lock_guard lock{_socketsMutex};
                _eventSockets.erase(socket);
            };
            return h;
        }
        // /api/v1/interact: one hub session per connection; previews go out as binary frames.
        auto session = std::make_shared<int>(0);
        h.open = [this, session](std::shared_ptr<ops::WebSocket> const& socket) {
            std::weak_ptr<ops::WebSocket> weak = socket;
            *session = _interact->open(ops::InteractHub::Peer{[weak](std::string const& text) {
                                                                  auto s = weak.lock();
                                                                  return s && s->sendText(text);
                                                              },
                socket->peer()});
            std::lock_guard lock{_socketsMutex};
            _interactSockets[*session] = socket;
        };
        h.message = [this, session](std::shared_ptr<ops::WebSocket> const&, std::string const& message, bool binary) {
            if (!binary)
            {
                _interact->message(*session, message);
            }
        };
        h.close = [this, session](std::shared_ptr<ops::WebSocket> const&) {
            _interact->close(*session);
            std::lock_guard lock{_socketsMutex};
            _interactSockets.erase(*session);
        };
        return h;
    }

    // DevTools (SPEC §10.4): the frontend and the page WebSocket are tunnelled to CEF's port on
    // 127.0.0.1 with Host and Origin rewritten (Chromium refuses other hosts and origins).
    bool Application::devtoolsTunnel(int fd, ops::HttpRequest const& req)
    {
        if (!startsWith(req.path, "/devtools/") || startsWith(req.path, "/devtools/json/") || !_cfg.devtools)
        {
            return false;
        }
        if (!authorized(req))
        {
            ops::writeAll(fd, "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: 27\r\nConnection: close\r\n\r\n{\"error\":\"token required\"}");
            ::close(fd);
            return true;
        }
        if (!_cfg.apiToken.empty() && cookie(req, "mbs_token").empty() && req.header("Upgrade").empty())
        {
            // A link with ?token=: keep it as a cookie for the frontend's own requests.
            ops::writeAll(fd, "HTTP/1.1 302 Found\r\nSet-Cookie: mbs_token=" + ops::queryValue(req.query, "token") +
                                  "; Path=/devtools; HttpOnly; SameSite=Strict\r\nLocation: " + req.path + (req.query.empty() ? "" : "?" + req.query) +
                                  "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            ::close(fd);
            return true;
        }
        int const upstream = util::connectTcp("127.0.0.1", _cfg.devtoolsPort, 2000);
        if (upstream < 0)
        {
            ops::writeAll(fd, "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            ::close(fd);
            return true;
        }
        std::string head;
        std::istringstream lines(req.raw);
        std::string line;
        bool firstLine = true;
        while (std::getline(lines, line))
        {
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            if (line.empty())
            {
                break;
            }
            auto const colon = line.find(':');
            auto const name = firstLine || colon == std::string::npos ? std::string{} : line.substr(0, colon);
            std::string lowerName = name;
            std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lowerName == "host")
            {
                line = "Host: 127.0.0.1:" + std::to_string(_cfg.devtoolsPort);
            }
            else if (lowerName == "origin")
            {
                line = "Origin: http://127.0.0.1:" + std::to_string(_cfg.devtoolsPort);
            }
            else if (lowerName == "cookie" || lowerName == "authorization")
            {
                continue; // the token stays here
            }
            head += line + "\r\n";
            firstLine = false;
        }
        head += "\r\n";
        bool const socket = !req.header("Upgrade").empty();
        if (socket)
        {
            _devtoolsSessions.fetch_add(1);
            log::warn("devtools_session", {{"client", req.peer}, {"path", req.path}});
        }
        if (ops::writeAll(upstream, head))
        {
            pump(fd, upstream);
        }
        if (socket)
        {
            _devtoolsSessions.fetch_sub(1);
        }
        ::close(upstream);
        ::close(fd);
        return true;
    }

    void Application::handle(ops::HttpRequest const& req, ops::HttpResponse& res)
    {
        auto const& path = req.path;
        auto const& method = req.method;

        // --- ops (no token: probes and scrapers)
        if (path == "/livez")
        {
            // The CEF message loop answers: a UI-thread task completes within 2 s.
            auto done = std::make_shared<std::atomic<bool>>(false);
            cef::postToUi([done] { done->store(true); });
            for (int i = 0; i < 200 && !done->load(); ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            done->load() ? reply(res, 200, R"({"live":true})") : reply(res, 503, R"({"live":false,"reason":"CEF message loop does not answer"})");
            return;
        }
        if (path == "/readyz")
        {
            std::string reason;
            ready(&reason) ? reply(res, 200, R"({"ready":true})") : reply(res, 503, R"({"ready":false,"reason":)" + json(reason) + "}");
            return;
        }
        if (path == "/metrics")
        {
            reply(res, 200, metricsText(), "text/plain; version=0.0.4");
            return;
        }
        if (path == "/statusz")
        {
            auto const s = _engine->stats();
            std::string reason;
            std::ostringstream out;
            out << "mxl-browser-source " << MBS_VERSION << " (CEF " << CEF_VERSION << ")\n"
                << "ready: " << (ready(&reason) ? "yes" : "no (" + reason + ")") << "\n"
                << "page: " << pageJson() << "\n"
                << "render: " << renderJson() << "\n"
                << "grains: " << s.tick.grains << ", repeated " << s.tick.repeated << ", held " << s.tick.repeatedHold << ", missed " << s.tick.missed
                << ", late paints " << s.latePaints << "\n"
                << "audio: drift " << s.audio.driftPpm << " ppm, buffer " << s.audio.fillSeconds * 1000 << " ms, underruns " << s.audio.underruns << "\n"
                << "nmos: " << nmosJson() << "\n";
            reply(res, 200, out.str(), "text/plain; charset=utf-8");
            return;
        }
        if (path == "/" || path == "/index.html")
        {
            reply(res, 200, std::string(webui::IndexHtml()), "text/html; charset=utf-8");
            return;
        }
        if (startsWith(path, "/devtools/"))
        {
            if (!_cfg.devtools)
            {
                error(res, 404, "DevTools is off (BROWSER_DEVTOOLS=false)");
                return;
            }
            if (!authorized(req))
            {
                error(res, 401, "token required");
                return;
            }
            if (path == "/devtools/json/list" || path == "/devtools/json" || path == "/devtools/json/version")
            {
                std::string body;
                int const status = util::httpGet("127.0.0.1", _cfg.devtoolsPort, path.substr(9), 3000, &body);
                if (status != 200)
                {
                    error(res, 502, "DevTools does not answer");
                    return;
                }
                // ws://127.0.0.1:<port>/devtools/page/X → the proxied path on this host.
                auto const host = req.header("Host");
                bool const secure = req.header("X-Forwarded-Proto") == "https";
                std::string const inner = "127.0.0.1:" + std::to_string(_cfg.devtoolsPort);
                std::string out;
                std::size_t pos = 0;
                while (true)
                {
                    auto const at = body.find(inner, pos);
                    if (at == std::string::npos)
                    {
                        out += body.substr(pos);
                        break;
                    }
                    out += body.substr(pos, at - pos) + host;
                    pos = at + inner.size();
                }
                // The frontend CEF bundles, through the tunnel, instead of the appspot copy.
                constexpr std::string_view kRemoteFrontend = "https://chrome-devtools-frontend.appspot.com/serve_rev/";
                for (auto at = out.find(kRemoteFrontend); at != std::string::npos; at = out.find(kRemoteFrontend, at))
                {
                    auto const revEnd = out.find('/', at + kRemoteFrontend.size()); // after "@<revision>"
                    if (revEnd == std::string::npos)
                    {
                        break;
                    }
                    out.replace(at, revEnd + 1 - at, "/devtools/");
                }
                if (secure)
                {
                    for (auto at = out.find("\"ws://"); at != std::string::npos; at = out.find("\"ws://", at))
                    {
                        out.replace(at, 6, "\"wss://");
                    }
                    for (auto at = out.find("?ws="); at != std::string::npos; at = out.find("?ws=", at + 5))
                    {
                        out.replace(at, 4, "?wss=");
                    }
                }
                reply(res, 200, out);
                return;
            }
            error(res, 404, "not found");
            return;
        }
        if (!startsWith(path, "/api/v1/"))
        {
            error(res, 404, "not found");
            return;
        }
        if (!authorized(req))
        {
            error(res, 401, "token required");
            return;
        }

        // --- information
        if (path == "/api/v1/info" && method == "GET")
        {
            reply(res, 200, infoJson());
            return;
        }
        if (path == "/api/v1/status" && method == "GET")
        {
            reply(res, 200, statusJson());
            return;
        }
        if (path == "/api/v1/nmos" && method == "GET")
        {
            reply(res, 200, nmosJson());
            return;
        }
        if (path == "/api/v1/console" && method == "GET")
        {
            std::string out = R"({"messages":[)";
            {
                std::lock_guard lock{_pageMutex};
                bool first = true;
                for (auto const& line : _console)
                {
                    out += (first ? "" : ",") + line;
                    first = false;
                }
            }
            reply(res, 200, out + "]}");
            return;
        }
        if (path == "/api/v1/preview.jpg" && method == "GET")
        {
            std::vector<std::uint8_t> rgb;
            std::uint64_t sequence = 0;
            _engine->requestPreview();
            for (int i = 0; i < 25 && !_engine->preview(rgb, sequence); ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }
            std::string jpeg;
            {
                std::lock_guard lock{_jpegMutex};
                if (!rgb.empty() && sequence != _jpegSequence)
                {
                    _jpeg = encodeJpeg(rgb, static_cast<int>(_engine->previewWidth()), static_cast<int>(_engine->previewHeight()));
                    _jpegSequence = sequence;
                }
                jpeg = _jpeg;
            }
            if (jpeg.empty())
            {
                error(res, 503, "no frame yet");
                return;
            }
            reply(res, 200, std::move(jpeg), "image/jpeg");
            return;
        }

        // --- source (SPEC §4.1)
        if (path == "/api/v1/source")
        {
            if (method == "GET")
            {
                reply(res, 200, sourceJson(_doc->source()));
                return;
            }
            if (method == "PUT" || method == "PATCH")
            {
                object body;
                if (!bodyObject(req, res, body))
                {
                    return;
                }
                auto const previous = _doc->source();
                config::Source next = method == "PUT" ? config::Source{} : previous;
                if (auto const why = config::mergeSource(body, next))
                {
                    error(res, 400, *why);
                    return;
                }
                if (next.url != previous.url)
                {
                    if (auto const verdict = ops::UrlPolicy(ops::UrlPolicySettings{_cfg.urlAllow, _cfg.urlDeny, _cfg.nmosRegistryAddress, _hostAddress,
                                                                {_cfg.webPort, _cfg.nmosPort, _cfg.nmosPort + 1, _cfg.devtoolsPort}})
                                                  .check(next.url, true);
                        !verdict.allowed)
                    {
                        error(res, 403, "URL refused by the policy: " + verdict.reason);
                        return;
                    }
                }
                if (!_doc->setSource(next))
                {
                    error(res, 500, "config file cannot be written");
                    return;
                }
                applySource(previous, next, false);
                reply(res, 200, sourceJson(next));
                return;
            }
            error(res, 405, "method not allowed");
            return;
        }
        if (startsWith(path, "/api/v1/source/") && method == "POST")
        {
            object body;
            if (!bodyObject(req, res, body))
            {
                return;
            }
            auto const action = path.substr(15);
            if (action == "navigate")
            {
                auto const it = body.find("url");
                if (it == body.end() || !it->second.is<std::string>())
                {
                    error(res, 400, "url required");
                    return;
                }
                auto const previous = _doc->source();
                auto next = previous;
                next.url = it->second.get<std::string>();
                auto const verdict = ops::UrlPolicy(ops::UrlPolicySettings{_cfg.urlAllow, _cfg.urlDeny, _cfg.nmosRegistryAddress, _hostAddress,
                                                        {_cfg.webPort, _cfg.nmosPort, _cfg.nmosPort + 1, _cfg.devtoolsPort}})
                                         .check(next.url, true);
                if (!verdict.allowed)
                {
                    _metrics.inc("navigation_blocked_total", {{"reason", verdict.reason}});
                    error(res, 403, "URL refused by the policy: " + verdict.reason);
                    return;
                }
                _doc->setSource(next);
                applySource(previous, next, true);
                reply(res, 200, sourceJson(next));
                return;
            }
            if (action == "reload")
            {
                auto const it = body.find("ignore_cache");
                bool const ignore = it != body.end() && it->second.is<bool>() && it->second.get<bool>();
                {
                    std::lock_guard lock{_pageMutex};
                    _autoReloadBlocked = false;
                    _crashes.clear();
                }
                _browser->reload(ignore);
                reply(res, 202, R"({"queued":true})");
                return;
            }
            if (action == "stop")
            {
                _browser->stopLoad();
                reply(res, 202, R"({"queued":true})");
                return;
            }
            if (action == "clear-cache")
            {
                _browser->devToolsMethod("Network.clearBrowserCache", "");
                auto const it = body.find("cookies");
                if (it != body.end() && it->second.is<bool>() && it->second.get<bool>())
                {
                    _browser->devToolsMethod("Network.clearBrowserCookies", "");
                }
                reply(res, 202, R"({"queued":true})");
                return;
            }
            if (action == "execute")
            {
                auto const it = body.find("js");
                if (it == body.end() || !it->second.is<std::string>())
                {
                    error(res, 400, "js required");
                    return;
                }
                log::info("execute_javascript", {{"client", req.peer}, {"bytes", static_cast<std::uint64_t>(it->second.get<std::string>().size())}});
                _browser->executeJavaScript(it->second.get<std::string>());
                reply(res, 202, R"({"queued":true})");
                return;
            }
            error(res, 404, "not found");
            return;
        }

        // --- presets
        if (path == "/api/v1/presets")
        {
            if (method == "GET")
            {
                array list;
                for (auto const& p : _doc->presets())
                {
                    auto v = config::sourceToJson(p.source);
                    v.get<object>()["name"] = value(p.name);
                    list.push_back(v);
                }
                reply(res, 200, value(list).serialize());
                return;
            }
            if (method == "POST")
            {
                object body;
                if (!bodyObject(req, res, body))
                {
                    return;
                }
                auto const name = body.find("name");
                if (name == body.end() || !name->second.is<std::string>() || name->second.get<std::string>().empty())
                {
                    error(res, 400, "name required");
                    return;
                }
                config::Preset preset;
                preset.name = name->second.get<std::string>();
                if (auto const why = config::mergeSource(body, preset.source))
                {
                    error(res, 400, *why);
                    return;
                }
                _doc->upsertPreset(preset);
                reply(res, 201, R"({"name":)" + json(preset.name) + "}");
                return;
            }
            error(res, 405, "method not allowed");
            return;
        }
        if (startsWith(path, "/api/v1/presets/"))
        {
            auto rest = path.substr(16);
            bool const apply = rest.size() > 6 && rest.substr(rest.size() - 6) == "/apply";
            auto const name = ops::urlDecode(apply ? rest.substr(0, rest.size() - 6) : rest);
            if (method == "DELETE" && !apply)
            {
                _doc->deletePreset(name) ? reply(res, 204, "") : error(res, 404, "no such preset");
                return;
            }
            if (method == "POST" && apply)
            {
                for (auto const& p : _doc->presets())
                {
                    if (p.name == name)
                    {
                        auto const previous = _doc->source();
                        _doc->setSource(p.source);
                        applySource(previous, p.source, true);
                        reply(res, 200, sourceJson(p.source));
                        return;
                    }
                }
                error(res, 404, "no such preset");
                return;
            }
            error(res, 405, "method not allowed");
            return;
        }

        // --- templates (SPEC §4.4)
        if (path == "/api/v1/templates" && method == "GET")
        {
            std::set<std::string> files;
            for (auto const& dir : {_cfg.templatesDir, std::string("/usr/local/share/mxl-browser-source/templates")})
            {
                std::error_code ec;
                for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
                {
                    if (it->is_regular_file() && (it->path().extension() == ".html" || it->path().extension() == ".htm"))
                    {
                        files.insert(it->path().lexically_relative(dir).generic_string());
                    }
                }
            }
            array list;
            for (auto const& f : files)
            {
                list.push_back(value(f));
            }
            object out;
            out["files"] = value(list);
            reply(res, 200, value(out).serialize());
            return;
        }
        if (startsWith(path, "/api/v1/template/") && method == "POST")
        {
            object body;
            if (!bodyObject(req, res, body))
            {
                return;
            }
            auto const verb = path.substr(17);
            std::string call;
            if (verb == "play" || verb == "stop" || verb == "next")
            {
                call = "if(typeof " + verb + "==='function'){" + verb + "();}else{console.warn('template has no " + verb + "()');}";
            }
            else if (verb == "remove")
            {
                call = "if(typeof remove==='function'){remove();}";
            }
            else if (verb == "update")
            {
                auto const it = body.find("data");
                if (it == body.end())
                {
                    error(res, 400, "data required");
                    return;
                }
                // An object is passed as a JSON string, as CasparCG passes its template data.
                std::string const data = it->second.is<std::string>() ? it->second.get<std::string>() : it->second.serialize();
                call = "if(typeof update==='function'){update(" + json(data) + ");}else{console.warn('template has no update()');}";
            }
            else if (verb == "invoke")
            {
                auto const fn = body.find("function");
                if (fn == body.end() || !fn->second.is<std::string>() || !validFunctionName(fn->second.get<std::string>()))
                {
                    error(res, 400, "function must match ^[A-Za-z_$][A-Za-z0-9_$.]*$");
                    return;
                }
                std::string args = "[]";
                if (auto const a = body.find("args"); a != body.end())
                {
                    if (!a->second.is<array>())
                    {
                        error(res, 400, "args must be an array");
                        return;
                    }
                    args = a->second.serialize();
                }
                call = fn->second.get<std::string>() + "(..." + args + ");";
            }
            else
            {
                error(res, 404, "unknown verb");
                return;
            }
            log::info("template_call", {{"verb", verb}, {"client", req.peer}});
            queueTemplateCall(call);
            reply(res, 202, R"({"queued":true})");
            return;
        }

        // --- configuration (SPEC §11)
        if (path == "/api/v1/config")
        {
            if (method == "GET")
            {
                reply(res, 200, configJson());
                return;
            }
            if (method == "PUT")
            {
                object body;
                if (!bodyObject(req, res, body))
                {
                    return;
                }
                auto settings = _doc->settings();
                auto const env = config::processEnvironment();
                array restart;
                for (auto const& [key, v] : body)
                {
                    if (!config::isKnownKey(key))
                    {
                        error(res, 400, "unknown setting " + key);
                        return;
                    }
                    if (env.count(key) != 0)
                    {
                        error(res, 409, key + " is set by the environment");
                        return;
                    }
                    if (v.is<picojson::null>())
                    {
                        settings.erase(key);
                    }
                    else
                    {
                        settings[key] = v.is<std::string>() ? v.get<std::string>() : v.serialize();
                    }
                    if (config::isRestartKey(key))
                    {
                        restart.push_back(value(key));
                    }
                }
                try
                {
                    config::load(env, settings, _cfg.nmosSeed);
                }
                catch (config::ConfigError const& ex)
                {
                    error(res, 400, ex.what());
                    return;
                }
                if (!_doc->setSettings(settings))
                {
                    error(res, 500, "config file cannot be written");
                    return;
                }
                object out;
                out["restart_required"] = value(restart);
                reply(res, 200, value(out).serialize());
                return;
            }
            error(res, 405, "method not allowed");
            return;
        }
        if (path == "/api/v1/config/env" && method == "GET")
        {
            std::string out;
            auto const file = _doc->settings();
            for (auto const& row : _table)
            {
                if (row.secret)
                {
                    continue;
                }
                auto value = row.value;
                if (row.source != "env")
                {
                    if (auto const it = file.find(row.key); it != file.end())
                    {
                        value = it->second;
                    }
                }
                out += row.key + "=" + value + "\n";
            }
            reply(res, 200, out, "text/plain; charset=utf-8");
            return;
        }
        if (path == "/api/v1/config/export" && method == "GET")
        {
            reply(res, 200, _doc->toJson({"BROWSER_API_TOKEN"}).serialize());
            return;
        }
        if (path == "/api/v1/config/import" && method == "POST")
        {
            value doc;
            if (!picojson::parse(doc, req.body).empty())
            {
                error(res, 400, "not JSON");
                return;
            }
            auto const before = _doc->settings();
            auto const previous = _doc->source();
            if (auto const why = _doc->importDocument(doc))
            {
                error(res, 400, *why);
                return;
            }
            array restart;
            auto const after = _doc->settings();
            for (auto const& key : config::knownKeys())
            {
                auto const a = before.find(key);
                auto const b = after.find(key);
                bool const changed = (a == before.end()) != (b == after.end()) || (a != before.end() && b != after.end() && a->second != b->second);
                if (changed && config::isRestartKey(key))
                {
                    restart.push_back(value(key));
                }
            }
            applySource(previous, _doc->source(), _doc->source().url != previous.url);
            object out;
            out["restart_required"] = value(restart);
            reply(res, 200, value(out).serialize());
            return;
        }
        error(res, 404, "not found");
    }
}
