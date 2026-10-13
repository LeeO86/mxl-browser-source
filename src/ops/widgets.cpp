// SPDX-License-Identifier: MIT
#include "ops/widgets.hpp"

#include <cctype>
#include <sstream>

namespace mbs::ops
{
    namespace
    {
        void error(HttpResponse& res, int status, std::string const& message)
        {
            res.status = status;
            res.contentType = "application/json; charset=utf-8";
            res.body = R"({"error":")" + message + R"("})";
        }

        // True when the source list names this Origin (or holds `*`).
        bool listedOrigin(std::string const& sources, std::string origin)
        {
            auto const normal = [](std::string text) {
                for (char& c : text)
                {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                while (!text.empty() && text.back() == '/')
                {
                    text.pop_back();
                }
                return text;
            };
            origin = normal(origin);
            if (origin.empty() || origin == "null")
            {
                return false;
            }
            std::istringstream tokens(sources);
            std::string token;
            while (tokens >> token)
            {
                if (token == "*" || normal(token) == origin)
                {
                    return true;
                }
            }
            return false;
        }
    }

    bool isWidgetPath(std::string_view path)
    {
        return path == "/widgets" || path.starts_with("/widget/");
    }

    void widgetResponse(std::string const& frameAncestors, HttpRequest const& req, std::string_view page, HttpResponse& res)
    {
        if (req.method != "GET")
        {
            res.headers.emplace_back("Allow", "GET");
            error(res, 405, "widgets answer GET only");
            return;
        }
        if (req.path == "/widgets")
        {
            // One browser source per process: the widget needs no parameter, the instance is the source.
            res.body = std::string(R"([{"id":"interaction","title":"Browser interaction","params":{"type":"object","properties":{}},)") +
                       R"("min_size":{"w":640,"h":400},"version":")" + MBS_VERSION + R"("},)" +
                       R"({"id":"ograf","title":"OGraf graphics","params":{"type":"object","properties":{"layers":{"type":"string",)" +
                       R"("description":"Comma list of the layers to show (1-8); all when empty"}}},"min_size":{"w":640,"h":480},"version":")" + MBS_VERSION +
                       R"("}])";
            if (auto const origin = req.header("Origin"); listedOrigin(frameAncestors, origin))
            {
                res.headers.emplace_back("Access-Control-Allow-Origin", origin);
            }
            res.headers.emplace_back("Vary", "Origin");
            return;
        }
        if (req.path != "/widget/interaction" && req.path != "/widget/ograf")
        {
            error(res, 404, "widget not found");
            return;
        }
        auto const theme = queryValue(req.query, "theme");
        if (!theme.empty() && theme != "dark" && theme != "light" && theme != "transparent")
        {
            error(res, 400, "theme must be dark, light or transparent");
            return;
        }
        // The page picks the widget from its URL. Only these routes may be framed.
        res.contentType = "text/html; charset=utf-8";
        res.body = std::string(page);
        res.headers.emplace_back("Content-Security-Policy", "frame-ancestors " + frameAncestors);
    }
}
