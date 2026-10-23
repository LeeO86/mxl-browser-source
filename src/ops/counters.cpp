// SPDX-License-Identifier: MIT
#include "ops/counters.hpp"

#include <initializer_list>
#include <string>

namespace mbs::ops
{
    namespace
    {
        void zero(util::Metrics& m, char const* name, char const* label, std::initializer_list<char const*> values)
        {
            for (auto const* v : values)
            {
                m.inc(name, {{label, v}}, 0);
            }
        }
    }

    void startCounters(util::Metrics& m, bool audio)
    {
        for (auto const* name : {"page_hangs_total", "popups_blocked_total", "downloads_blocked_total", "preview_frames_total", "preview_frames_dropped_total"})
        {
            m.inc(name, {}, 0);
        }
        // The label values each producer can report (cef/runtime.cpp, ops/urlpolicy.cpp, app/*).
        zero(m, "page_loads_total", "result", {"ok", "error"});
        zero(m, "renderer_crashes_total", "reason", {"abnormal", "killed", "crashed", "oom", "other", "hung"});
        zero(m, "js_dialogs_total", "type", {"alert", "confirm", "prompt", "beforeunload"});
        zero(m, "permission_denied_total", "type", {"camera", "microphone", "screen", "geolocation", "notifications", "clipboard", "midi", "other"});
        zero(m, "navigation_blocked_total", "reason",
            {"scheme", "link_local", "loopback", "kubernetes", "registry", "own_port", "denied", "not_allowed", "ograf_local", "service_worker"});
        zero(m, "console_messages_total", "level", {"debug", "info", "warning", "error"});
        zero(m, "ograf_uploads_total", "result", {"ok", "rejected"});
        zero(m, "ograf_restores_total", "result", {"ok", "error"});
        for (auto const* action : {"load", "play", "stop", "update", "custom", "clear"})
        {
            for (auto const* result : {"ok", "error", "timeout", "unavailable"})
            {
                m.inc("ograf_actions_total", {{"action", action}, {"result", result}}, 0);
            }
        }
        if (audio)
        {
            zero(m, "audio_stream_events_total", "event", {"started", "stopped", "error"});
        }
    }
}
