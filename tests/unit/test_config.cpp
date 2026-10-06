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
    CHECK(loaded.config.keyMode == KeyMode::Off);
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

TEST_CASE("formats and secrets")
{
    auto const f = parseFormat("1080p59.94");
    CHECK(f.rateNum == 60000);
    CHECK(f.rateDen == 1001);
    CHECK(row(load({{"BROWSER_API_TOKEN", "s3cret"}}, {}, "host"), "BROWSER_API_TOKEN").secret);
}
