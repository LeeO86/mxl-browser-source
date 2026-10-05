// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "ops/interact.hpp"

using namespace mbs::ops;

namespace
{
    struct FakePage : PageInput
    {
        std::vector<std::string> calls;
        void mouseMove(int x, int y, std::uint32_t, bool leave) override
        {
            calls.push_back((leave ? "leave " : "move ") + std::to_string(x) + "," + std::to_string(y));
        }
        void mouseClick(int x, int y, MouseButton button, bool up, int clicks, std::uint32_t modifiers) override
        {
            calls.push_back("click " + std::to_string(x) + "," + std::to_string(y) + " b" + std::to_string(static_cast<int>(button)) + (up ? " up" : " down") +
                            " x" + std::to_string(clicks) + " m" + std::to_string(modifiers));
        }
        void mouseWheel(int, int, int dx, int dy, std::uint32_t) override { calls.push_back("wheel " + std::to_string(dx) + "," + std::to_string(dy)); }
        void key(KeyEvent const& e) override { calls.push_back("key " + std::to_string(static_cast<int>(e.type)) + " " + std::to_string(e.windowsKeyCode) + " " + std::to_string(e.character)); }
        void imeComposition(std::u16string const& text, int start, int end) override
        {
            calls.push_back("ime " + std::to_string(text.size()) + " " + std::to_string(start) + "-" + std::to_string(end));
        }
        void imeCommit(std::u16string const& text) override { calls.push_back("commit " + std::to_string(text.size())); }
        void imeFinish() override { calls.push_back("finish"); }
        void imeCancel() override { calls.push_back("cancel"); }
        void focus(bool f) override { calls.push_back(f ? "focus" : "blur"); }
    };

    struct Client
    {
        std::vector<std::string> received;
        InteractHub::Peer peer()
        {
            return {[this](std::string const& t) {
                        received.push_back(t);
                        return true;
                    },
                "10.0.0.1"};
        }
        [[nodiscard]] bool got(std::string const& part) const
        {
            for (auto const& m : received)
            {
                if (m.find(part) != std::string::npos)
                {
                    return true;
                }
            }
            return false;
        }
        [[nodiscard]] std::string const& last() const { return received.back(); }
    };

    InteractSettings settings()
    {
        InteractSettings s;
        s.viewWidth = 1920;
        s.viewHeight = 1080;
        s.timeoutS = 120;
        return s;
    }
}

TEST_CASE("interaction is off until a session enables it; observers cannot inject")
{
    FakePage page;
    InteractHub hub(settings(), page);
    Client a;
    Client b;
    int const ia = hub.open(a.peer());
    int const ib = hub.open(b.peer());
    CHECK(a.got(R"("controller":"none")"));
    hub.message(ia, R"({"type":"pointer","action":"move","x":0.5,"y":0.5})");
    CHECK(page.calls.empty());
    CHECK(a.last().find("not_controller") != std::string::npos);

    hub.message(ia, R"({"type":"interact","enable":true})");
    CHECK(hub.controlled());
    CHECK(a.got(R"("controller":"self")"));
    CHECK(b.got(R"("controller":"other")"));
    hub.message(ib, R"({"type":"pointer","action":"move","x":0.1,"y":0.1})");
    CHECK(b.last().find("not_controller") != std::string::npos);
    // b cannot take control without asking for it, and then can.
    hub.message(ib, R"({"type":"interact","enable":true})");
    CHECK(b.last().find("controlled") != std::string::npos);
    hub.message(ib, R"({"type":"interact","enable":true,"take":true})");
    CHECK(a.last().find(R"("controller":"other")") != std::string::npos);
    CHECK(b.last().find(R"("controller":"self")") != std::string::npos);
    // The controller leaving releases control.
    hub.close(ib);
    CHECK(!hub.controlled());
    CHECK(a.last().find(R"("controller":"none")") != std::string::npos);
}

TEST_CASE("pointer, wheel, keys, text and IME become page events; acks carry the grain")
{
    FakePage page;
    InteractHub hub(settings(), page);
    hub.setGrainSource([] { return std::uint64_t{4242}; });
    Client a;
    int const id = hub.open(a.peer());
    hub.message(id, R"({"type":"interact","enable":true})");
    hub.message(id, R"({"type":"pointer","action":"down","x":0.5,"y":0.25,"button":"left","clicks":2,"modifiers":["shift"],"seq":7})");
    REQUIRE(page.calls.size() == 1);
    CHECK(page.calls[0] == "click 960,270 b0 down x2 m2");
    CHECK(a.last() == R"({"type":"ack","seq":7,"grain":4242})");
    hub.message(id, R"({"type":"pointer","action":"leave","x":0,"y":0})");
    CHECK(page.calls.back() == "leave 0,0");
    hub.message(id, R"({"type":"wheel","x":0.5,"y":0.5,"dx":0,"dy":-120})");
    CHECK(page.calls.back() == "wheel 0,-120");
    page.calls.clear();
    hub.message(id, R"({"type":"key","action":"down","code":"KeyA","key":"a"})");
    REQUIRE(page.calls.size() == 2); // RAWKEYDOWN + CHAR
    CHECK(page.calls[0] == "key 0 65 0");
    CHECK(page.calls[1] == "key 3 65 97");
    page.calls.clear();
    hub.message(id, R"({"type":"text","text":"Grüezi"})");
    CHECK(page.calls.size() == 6);
    page.calls.clear();
    hub.message(id, R"({"type":"ime","action":"composition","text":"にほ","selection":[2,2]})");
    hub.message(id, R"({"type":"ime","action":"commit","text":"日本"})");
    hub.message(id, R"({"type":"focus","focused":true})");
    REQUIRE(page.calls.size() == 3);
    CHECK(page.calls[0] == "ime 2 2-2");
    CHECK(page.calls[1] == "commit 2");
    CHECK(page.calls[2] == "focus");
    CHECK(hub.counters().events.at("pointer/ok") == 2);
}

TEST_CASE("malformed messages are answered with an error and ignored")
{
    FakePage page;
    InteractHub hub(settings(), page);
    Client a;
    int const id = hub.open(a.peer());
    hub.message(id, R"({"type":"interact","enable":true})");
    hub.message(id, "not json");
    CHECK(a.last().find("bad_message") != std::string::npos);
    hub.message(id, R"({"type":"teleport"})");
    CHECK(a.last().find("bad_message") != std::string::npos);
    hub.message(id, R"({"type":"pointer","action":"jump","x":0,"y":0})");
    CHECK(a.last().find("bad_message") != std::string::npos);
    hub.message(id, R"({"type":"key","action":"down","code":"KeyA","modifiers":["hyper"]})");
    CHECK(a.last().find("bad_message") != std::string::npos);
    CHECK(page.calls.empty());
}

TEST_CASE("control ends after the idle timeout; the input limit drops the excess")
{
    FakePage page;
    auto s = settings();
    s.maxMessagesPerSecond = 5;
    InteractHub hub(s, page);
    Client a;
    int const id = hub.open(a.peer());
    auto const t0 = InteractHub::Clock::now();
    hub.message(id, R"({"type":"interact","enable":true})", t0);
    for (int i = 0; i < 10; ++i)
    {
        hub.message(id, R"({"type":"pointer","action":"move","x":0.5,"y":0.5})", t0 + std::chrono::milliseconds(10 * i));
    }
    CHECK(page.calls.size() == 4); // 5 per second, the interact message included
    CHECK(a.got("rate_limited"));
    hub.tick(t0 + std::chrono::seconds(60));
    CHECK(hub.controlled());
    hub.tick(t0 + std::chrono::seconds(121));
    CHECK(!hub.controlled());
    CHECK(a.last().find(R"("controller":"none")") != std::string::npos);
}

TEST_CASE("preview rates: watchers up to the default, the controller up to 25")
{
    FakePage page;
    InteractHub hub(settings(), page);
    Client a;
    int const id = hub.open(a.peer());
    CHECK(hub.previewRates().at(id) == 10);
    hub.message(id, R"({"type":"preview","fps":25})");
    CHECK(hub.previewRates().at(id) == 10);
    hub.message(id, R"({"type":"interact","enable":true})");
    hub.message(id, R"({"type":"preview","fps":25})");
    CHECK(hub.previewRates().at(id) == 25);
}
