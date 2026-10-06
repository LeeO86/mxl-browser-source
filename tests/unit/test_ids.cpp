// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <set>

#include "nmos/ids.hpp"
#include "ops/httpserver.hpp"
#include "util/net.hpp"

TEST_CASE("ids are UUIDv5 of the seed; a format or key mode change renews only the flows")
{
    auto const a = mbs::nmos::makeIds("lab-1", "1080p50", "v210a", 2);
    auto const b = mbs::nmos::makeIds("lab-1", "1080p50", "v210a", 2);
    CHECK(a.node == b.node);
    CHECK(a.videoFlow == b.videoFlow);
    CHECK(a.node.size() == 36);
    CHECK(a.node[14] == '5'); // version 5
    std::set<std::string> const all{a.node, a.device, a.videoSource, a.keySource, a.audioSource, a.videoFlow, a.keyFlow, a.audioFlow, a.videoSender,
        a.keySender, a.audioSender, a.domain};
    CHECK(all.size() == 12);

    auto const c = mbs::nmos::makeIds("lab-1", "720p50", "off", 8);
    CHECK(c.videoFlow != a.videoFlow);
    CHECK(c.keyFlow != a.keyFlow);
    CHECK(c.audioFlow != a.audioFlow);
    CHECK(c.videoSender == a.videoSender);
    CHECK(c.node == a.node);
    CHECK(mbs::nmos::makeIds("lab-2", "1080p50", "v210a", 2).node != a.node);
}

TEST_CASE("the listener check finds this process's own listening socket only")
{
    mbs::ops::HttpServer server([](mbs::ops::HttpRequest const&, mbs::ops::HttpResponse&) {});
    std::string error;
    REQUIRE(server.start(0, error));
    CHECK(mbs::util::ownsListener(server.port()));
    int const port = server.port();
    server.stop();
    CHECK_FALSE(mbs::util::ownsListener(port));
}
