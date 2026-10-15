// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <map>
#include <string>

#include "config/config.hpp"

using namespace mbs::config;

namespace
{
    using Map = std::map<std::string, std::string>;

    // The table row of `key` (an empty row when the key is missing).
    SettingInfo row(Loaded const& loaded, std::string const& key)
    {
        for (auto const& info : loaded.table)
        {
            if (info.key == key)
            {
                return info;
            }
        }
        return {};
    }

    std::string errorKey(Map const& env)
    {
        try
        {
            (void)load(env, {}, "host");
        }
        catch (ConfigError const& ex)
        {
            return ex.key();
        }
        return {};
    }
}

TEST_CASE("defaults")
{
    auto const loaded = load({}, {}, "host");
    CHECK(loaded.config.webPort == 8160);
    CHECK(loaded.config.format.name == "1080p50");
    CHECK(toString(loaded.config.keyMode) == "off"); // not the enum: doctest would call config::toString itself
    CHECK_FALSE(loaded.config.nmosDnsSd);
    CHECK(row(loaded, "WEB_PORT").source == "default");
    CHECK(loaded.table.size() == knownKeys().size());
}

TEST_CASE("environment over config file over defaults (G1)")
{
    auto const fromFile = load({}, {{"WEB_PORT", "9000"}}, "host");
    CHECK(fromFile.config.webPort == 9000);
    CHECK(row(fromFile, "WEB_PORT").source == "file");
    auto const fromEnv = load({{"WEB_PORT", "9100"}}, {{"WEB_PORT", "9000"}}, "host");
    CHECK(fromEnv.config.webPort == 9100);
    CHECK(row(fromEnv, "WEB_PORT").source == "env");
}

TEST_CASE("unknown variables are ignored, invalid values name their key (exit 78)")
{
    CHECK_NOTHROW(load({{"SOME_OTHER_TOOL", "x"}, {"PATH", "/usr/bin"}}, {}, "host"));
    CHECK(errorKey({{"WEB_PORT", "eighty"}}) == "WEB_PORT");
    CHECK(errorKey({{"BROWSER_FORMAT", "999p"}}) == "BROWSER_FORMAT");
    CHECK(errorKey({{"BROWSER_KEY_MODE", "luma"}}) == "BROWSER_KEY_MODE");
    CHECK(errorKey({{"NMOS_TAGS", "{not json"}}) == "NMOS_TAGS");
}

TEST_CASE("only routable IPv4 literals are announced (G5)")
{
    CHECK(errorKey({{"NMOS_HOST_ADDRESS", "127.0.0.1"}}) == "NMOS_HOST_ADDRESS");
    CHECK(errorKey({{"NMOS_HOST_ADDRESS", "0.0.0.0"}}) == "NMOS_HOST_ADDRESS");
    CHECK(errorKey({{"NMOS_HOST_ADDRESS", "node-1.example"}}) == "NMOS_HOST_ADDRESS");
    CHECK(load({{"NMOS_HOST_ADDRESS", "10.0.0.5"}}, {}, "host").config.nmosHostAddress == "10.0.0.5");
}

TEST_CASE("WebAuthn is hidden from pages unless enabled")
{
    CHECK_FALSE(load({}, {}, "host").config.webauthn);
    CHECK(load({{"BROWSER_WEBAUTHN", "true"}}, {}, "host").config.webauthn);
    CHECK(errorKey({{"BROWSER_WEBAUTHN", "maybe"}}) == "BROWSER_WEBAUTHN");
}

TEST_CASE("service workers are off unless enabled")
{
    CHECK_FALSE(load({}, {}, "host").config.serviceWorkers);
    CHECK(load({{"BROWSER_SERVICE_WORKERS", "true"}}, {}, "host").config.serviceWorkers);
    CHECK(errorKey({{"BROWSER_SERVICE_WORKERS", "sometimes"}}) == "BROWSER_SERVICE_WORKERS");
}

TEST_CASE("frame-loop settings: pipeline, real-time threads, conversion threads, WebGPU")
{
    auto const d = load({}, {}, "host").config;
    CHECK(d.framePipeline == 1);
    CHECK(d.realtime);
    CHECK(d.convertThreads == 4);
    CHECK_FALSE(d.webgpu);
    auto const c = load({{"BROWSER_FRAME_PIPELINE", "0"}, {"BROWSER_REALTIME", "false"}, {"BROWSER_CONVERT_THREADS", "8"}, {"BROWSER_WEBGPU", "true"}}, {}, "host").config;
    CHECK(c.framePipeline == 0);
    CHECK_FALSE(c.realtime);
    CHECK(c.convertThreads == 8);
    CHECK(c.webgpu);
    CHECK(errorKey({{"BROWSER_FRAME_PIPELINE", "2"}}) == "BROWSER_FRAME_PIPELINE");
    CHECK(errorKey({{"BROWSER_CONVERT_THREADS", "0"}}) == "BROWSER_CONVERT_THREADS");
    CHECK(errorKey({{"BROWSER_REALTIME", "maybe"}}) == "BROWSER_REALTIME");
}

TEST_CASE("frame clock: internal by default, external on request, a phase shorter than the period")
{
    auto const d = load({}, {}, "host").config;
    CHECK((d.frameClock == FrameClockMode::Internal));
    CHECK(d.frameClockPhaseMs == 0);
    CHECK((load({{"BROWSER_FRAME_CLOCK", "external"}}, {}, "host").config.frameClock == FrameClockMode::External));
    auto const c = load({{"BROWSER_FRAME_CLOCK", "internal"}, {"BROWSER_FRAME_CLOCK_PHASE_MS", "5"}}, {}, "host").config;
    CHECK((c.frameClock == FrameClockMode::Internal));
    CHECK(c.frameClockPhaseMs == 5);
    CHECK(load({{"BROWSER_FORMAT", "1080p25"}, {"BROWSER_FRAME_CLOCK_PHASE_MS", "39"}}, {}, "host").config.frameClockPhaseMs == 39);
    CHECK(errorKey({{"BROWSER_FRAME_CLOCK", "vsync"}}) == "BROWSER_FRAME_CLOCK");
    CHECK(errorKey({{"BROWSER_FRAME_CLOCK_PHASE_MS", "20"}}) == "BROWSER_FRAME_CLOCK_PHASE_MS"); // 1080p50: 20 ms period
    CHECK(errorKey({{"BROWSER_FRAME_CLOCK_PHASE_MS", "-1"}}) == "BROWSER_FRAME_CLOCK_PHASE_MS");
}

TEST_CASE("audio buffer: 60 ms, adaptive up to 300 ms by default, fixed at or below the base")
{
    auto const d = load({}, {}, "host").config;
    CHECK(d.audioBufferMs == 60);
    CHECK(d.audioBufferMaxMs == 300);
    CHECK(load({{"BROWSER_AUDIO_BUFFER_MAX_MS", "60"}}, {}, "host").config.audioBufferMaxMs == 60);
    CHECK(load({{"BROWSER_AUDIO_BUFFER_MS", "400"}}, {}, "host").config.audioBufferMaxMs == 300); // below the base: fixed, not an error
    CHECK(errorKey({{"BROWSER_AUDIO_BUFFER_MAX_MS", "501"}}) == "BROWSER_AUDIO_BUFFER_MAX_MS");
    CHECK(errorKey({{"BROWSER_AUDIO_BUFFER_MAX_MS", "auto"}}) == "BROWSER_AUDIO_BUFFER_MAX_MS");
}

TEST_CASE("formats and secrets")
{
    auto const f = parseFormat("1080p59.94");
    CHECK(f.rateNum == 60000);
    CHECK(f.rateDen == 1001);
    CHECK(row(load({{"BROWSER_API_TOKEN", "s3cret"}}, {}, "host"), "BROWSER_API_TOKEN").secret);
}

TEST_CASE("WIDGET_FRAME_ANCESTORS defaults to 'self' and must be one CSP source list")
{
    CHECK(load({}, {}, "host").config.widgetFrameAncestors == "'self'");
    auto const set = load({{"WIDGET_FRAME_ANCESTORS", "'self' https://designer.example"}}, {}, "host");
    CHECK(set.config.widgetFrameAncestors == "'self' https://designer.example");
    CHECK(row(set, "WIDGET_FRAME_ANCESTORS").source == "env");
    CHECK(row(set, "WIDGET_FRAME_ANCESTORS").restart);
    CHECK(errorKey({{"WIDGET_FRAME_ANCESTORS", "'self'; script-src *"}}) == "WIDGET_FRAME_ANCESTORS");
    CHECK(errorKey({{"WIDGET_FRAME_ANCESTORS", "https://a.example, https://b.example"}}) == "WIDGET_FRAME_ANCESTORS");
}

TEST_CASE("OGraf settings: network modes and CORS origins")
{
    auto const load = [](std::map<std::string, std::string> env) { return mbs::config::load(env, {}, "host").config; };
    auto const def = load({});
    CHECK(def.ografDir == "/config/ograf");
    CHECK_FALSE(def.ografNetworkAny);
    CHECK(def.ografNetworkHosts.empty());
    CHECK(load({{"BROWSER_OGRAF_NETWORK", "any"}}).ografNetworkAny);
    CHECK(load({{"BROWSER_OGRAF_NETWORK", "data.example, *.scores.example"}}).ografNetworkHosts.size() == 2);
    CHECK(load({{"BROWSER_OGRAF_CORS_ORIGINS", "https://a.example https://b.example"}}).ografCorsOrigins.size() == 2);
    CHECK_THROWS_AS(load({{"BROWSER_OGRAF_CORS_ORIGINS", "*"}}), mbs::config::ConfigError);
    CHECK_THROWS_AS(load({{"BROWSER_OGRAF_CORS_ORIGINS", "controller.example"}}), mbs::config::ConfigError);
}
