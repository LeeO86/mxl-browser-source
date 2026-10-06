// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "ops/urlpolicy.hpp"

using mbs::ops::UrlPolicy;
using mbs::ops::UrlPolicySettings;

namespace
{
    std::string reason(UrlPolicy const& p, std::string_view url, bool top = true)
    {
        auto const v = p.check(url, top);
        return v.allowed ? "ok" : v.reason;
    }
}

TEST_CASE("URLs parse into scheme, host and port")
{
    auto u = mbs::ops::parseUrl("https://User@Graphics.Media.INT:8443/a?b#c");
    REQUIRE(u);
    CHECK(u->scheme == "https");
    CHECK(u->host == "graphics.media.int");
    CHECK(u->port == 8443);
    u = mbs::ops::parseUrl("http://[fe80::1]/x");
    REQUIRE(u);
    CHECK(u->host == "fe80::1");
    CHECK(u->port == 80);
    CHECK_FALSE(mbs::ops::parseUrl("data:text/html,hi"));
    CHECK_FALSE(mbs::ops::parseUrl("http://host:99999/"));
}

TEST_CASE("globs and CIDRs")
{
    CHECK(mbs::ops::globMatch("*.media.int", "gfx.media.int"));
    CHECK_FALSE(mbs::ops::globMatch("*.media.int", "media.int"));
    CHECK(mbs::ops::globMatch("score*board", "SCOREBOARD"));
    CHECK(mbs::ops::cidrMatch("10.42.0.0/16", "10.42.7.9"));
    CHECK_FALSE(mbs::ops::cidrMatch("10.42.0.0/16", "10.43.0.1"));
    CHECK(mbs::ops::cidrMatch("fe80::/10", "fe80::abcd"));
    CHECK(mbs::ops::cidrMatch("127.0.0.0/8", "::ffff:127.0.0.1")); // IPv4-mapped
    CHECK_FALSE(mbs::ops::cidrMatch("10.0.0.0/8", "fe80::1"));
}

TEST_CASE("schemes: only http(s) and ws(s) navigate; data and javascript only inside a page")
{
    UrlPolicy const p;
    CHECK(reason(p, "https://example.org/") == "ok");
    CHECK(reason(p, "https://templates.local/bars.html") == "ok");
    CHECK(reason(p, "about:blank") == "ok");
    CHECK(reason(p, "file:///etc/passwd") == "scheme");
    CHECK(reason(p, "chrome://gpu") == "scheme");
    CHECK(reason(p, "view-source:https://example.org") == "scheme");
    CHECK(reason(p, "devtools://devtools/bundled/inspector.html") == "scheme");
    CHECK(reason(p, "data:text/html,<h1>x</h1>") == "scheme");
    CHECK(reason(p, "data:image/png;base64,AAAA", false) == "ok");
    CHECK(reason(p, "javascript:alert(1)") == "scheme");
    CHECK(reason(p, "blob:https://example.org/1234", false) == "ok");
}

TEST_CASE("built-in denials: metadata, loopback, Kubernetes API, registry, own ports")
{
    UrlPolicySettings s;
    s.registryHost = "10.164.16.97";
    s.ownHost = "10.42.1.5";
    s.ownPorts = {8160, 3312, 3313, 9222};
    UrlPolicy const p(s);
    CHECK(reason(p, "http://169.254.169.254/latest/meta-data/") == "link_local");
    CHECK(reason(p, "http://[fe80::1]/") == "link_local");
    CHECK(reason(p, "http://metadata.google.internal/") == "link_local");
    CHECK(reason(p, "http://127.0.0.1:9222/json") == "loopback");
    CHECK(reason(p, "http://localhost/") == "loopback");
    CHECK(reason(p, "http://localhost./") == "loopback");
    CHECK(reason(p, "http://[::1]/") == "loopback");
    CHECK(reason(p, "http://[::ffff:127.0.0.1]/") == "loopback");
    CHECK(reason(p, "https://kubernetes.default.svc/api") == "kubernetes");
    CHECK(reason(p, "https://kubernetes.default.svc.cluster.local/api") == "kubernetes");
    CHECK(reason(p, "http://10.164.16.97:8010/x-nmos/") == "registry");
    CHECK(reason(p, "http://10.42.1.5:8160/api/v1/source") == "own_port");
    CHECK(reason(p, "http://10.42.1.5:8080/") == "ok");
    CHECK(reason(p, "wss://10.42.1.5:3313/", false) == "own_port");
}

TEST_CASE("deny adds to the built-in list; allow restricts to known hosts")
{
    UrlPolicySettings s;
    s.deny = {"10.43.0.0/16", "*.internal.example"};
    UrlPolicy const deny(s);
    CHECK(reason(deny, "http://10.43.0.10/") == "denied");
    CHECK(reason(deny, "https://api.internal.example/") == "denied");
    CHECK(reason(deny, "https://graphics.media.int/") == "ok");

    s.allow = {"*.media.int", "192.168.10.0/24"};
    UrlPolicy const allow(s);
    CHECK(reason(allow, "https://graphics.media.int/lower-third/") == "ok");
    CHECK(reason(allow, "http://192.168.10.20/") == "ok");
    CHECK(reason(allow, "https://example.org/") == "not_allowed");
    CHECK(reason(allow, "https://templates.local/blank.html") == "ok");
    CHECK(reason(allow, "http://10.43.0.10/") == "denied"); // deny wins
}
