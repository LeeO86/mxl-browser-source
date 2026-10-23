// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <string>

#include "ops/counters.hpp"
#include "ops/interact.hpp"

using namespace mbs;

namespace
{
    bool has(std::string const& text, std::string const& line)
    {
        return text.find("\n" + line + "\n") != std::string::npos || text.rfind(line + "\n", 0) == 0;
    }

    struct NoPage : ops::PageInput
    {
        void mouseMove(int, int, std::uint32_t, bool) override {}
        void mouseClick(int, int, ops::MouseButton, bool, int, std::uint32_t) override {}
        void mouseWheel(int, int, int, int, std::uint32_t) override {}
        void key(ops::KeyEvent const&) override {}
        void imeComposition(std::u16string const&, int, int) override {}
        void imeCommit(std::u16string const&) override {}
        void imeFinish() override {}
        void imeCancel() override {}
        void focus(bool) override {}
    };
}

TEST_CASE("the event counters are on /metrics at 0 from the start")
{
    util::Metrics m("p_");
    ops::startCounters(m, true);
    auto const text = m.render();
    for (auto const* line : {"p_page_hangs_total 0", "p_preview_frames_total 0", "p_preview_frames_dropped_total 0", "p_popups_blocked_total 0",
             "p_downloads_blocked_total 0", R"(p_page_loads_total{result="error"} 0)", R"(p_renderer_crashes_total{reason="hung"} 0)",
             R"(p_renderer_crashes_total{reason="crashed"} 0)", R"(p_js_dialogs_total{type="alert"} 0)", R"(p_permission_denied_total{type="camera"} 0)",
             R"(p_navigation_blocked_total{reason="service_worker"} 0)", R"(p_console_messages_total{level="error"} 0)",
             R"(p_ograf_actions_total{action="play",result="timeout"} 0)", R"(p_ograf_uploads_total{result="rejected"} 0)",
             R"(p_ograf_restores_total{result="error"} 0)", R"(p_audio_stream_events_total{event="error"} 0)"})
    {
        CHECK_MESSAGE(has(text, line), line);
    }
    CHECK(text.find("# TYPE p_page_hangs_total counter") != std::string::npos);

    util::Metrics silent("p_");
    ops::startCounters(silent, false);
    CHECK(silent.render().find("audio_stream_events_total") == std::string::npos);

    // An event adds to the zero series.
    m.inc("page_hangs_total");
    CHECK(has(m.render(), "p_page_hangs_total 1"));
}

TEST_CASE("the interaction counters start at 0 for every type and result")
{
    NoPage page;
    ops::InteractHub hub({}, page);
    auto const events = hub.counters().events;
    for (auto const* key : {"pointer/ok", "pointer/rejected", "key/ok", "text/rejected", "ime/ok", "focus/ok", "wheel/rejected", "interact/rejected",
             "invalid/rejected", "any/dropped"})
    {
        REQUIRE_MESSAGE(events.count(key) == 1, key);
        CHECK(events.at(key) == 0);
    }
}
