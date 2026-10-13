// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <string>

#include "ops/widgets.hpp"
#include "picojson/picojson.h"

using namespace mbs::ops;

namespace
{
    HttpResponse get(std::string const& path, std::string const& query = "", std::string const& origin = "", std::string const& method = "GET")
    {
        HttpRequest req;
        req.method = method;
        req.path = path;
        req.query = query;
        if (!origin.empty())
        {
            req.headers.emplace_back("origin", origin);
        }
        HttpResponse res;
        widgetResponse("'self' https://designer.example", req, "<!doctype html><title>ui</title>", res);
        return res;
    }

    std::string header(HttpResponse const& res, std::string const& name)
    {
        for (auto const& [key, value] : res.headers)
        {
            if (key == name)
            {
                return value;
            }
        }
        return "(none)";
    }
}

TEST_CASE("widgets: the list with CORS for listed origins only")
{
    auto const list = get("/widgets", "", "https://designer.example");
    CHECK(list.status == 200);
    CHECK(header(list, "Access-Control-Allow-Origin") == "https://designer.example");
    CHECK(header(list, "Vary") == "Origin");
    picojson::value doc;
    REQUIRE(picojson::parse(doc, list.body).empty());
    REQUIRE(doc.is<picojson::array>());
    REQUIRE(doc.get<picojson::array>().size() == 2);
    CHECK(doc.get<picojson::array>()[1].get("id").to_str() == "ograf");
    auto const& widget = doc.get<picojson::array>()[0];
    CHECK(widget.get("id").to_str() == "interaction");
    CHECK(widget.get("version").to_str() == MBS_VERSION);
    CHECK(widget.get("min_size").serialize() == R"({"h":400,"w":640})");
    CHECK(widget.get("params").serialize() == R"({"properties":{},"type":"object"})");
    auto const other = get("/widgets", "", "https://evil.example");
    CHECK(other.status == 200);
    CHECK(header(other, "Access-Control-Allow-Origin") == "(none)");
    CHECK(header(other, "Vary") == "Origin");
    CHECK(header(get("/widgets"), "Access-Control-Allow-Origin") == "(none)");
    auto const post = get("/widgets", "", "", "POST");
    CHECK(post.status == 405);
    CHECK(header(post, "Allow") == "GET");
    CHECK(get("/widgets", "", "", "OPTIONS").status == 405);
}

TEST_CASE("widgets: the page carries CSP frame-ancestors and no X-Frame-Options")
{
    auto const page = get("/widget/interaction", "theme=transparent");
    CHECK(page.status == 200);
    CHECK(page.contentType.starts_with("text/html"));
    CHECK(page.body == "<!doctype html><title>ui</title>");
    CHECK(header(page, "Content-Security-Policy") == "frame-ancestors 'self' https://designer.example");
    CHECK(header(page, "X-Frame-Options") == "(none)");
    CHECK(get("/widget/interaction").status == 200);
    CHECK(get("/widget/ograf", "layers=1,2").status == 200);
    CHECK(get("/widget/interaction", "theme=light&source=any").status == 200); // other parameters are ignored
    CHECK(get("/widget/interaction", "theme=pink").status == 400);
    CHECK(get("/widget/interaction", "", "", "POST").status == 405);
    CHECK(get("/widget/template").status == 404);
    CHECK(isWidgetPath("/widgets"));
    CHECK(isWidgetPath("/widget/interaction"));
    CHECK_FALSE(isWidgetPath("/api/v1/info"));
}
