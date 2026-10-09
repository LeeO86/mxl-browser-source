// SPDX-License-Identifier: MIT
#include "config/config.hpp"

#include <arpa/inet.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <set>
#include <sstream>

#include "picojson/picojson.h"

extern char** environ;

namespace mbs::config
{
    namespace
    {
        struct Def
        {
            char const* key;
            char const* defaultValue; // "" where the default is derived
            bool restart;
            bool secret;
            char const* description;
        };

        // SPEC §11, in table order.
        std::vector<Def> const& defs()
        {
            static std::vector<Def> const table = {
                {"WEB_PORT", "8160", true, false, "Web port: UI, REST, WebSockets, health, metrics"},
                {"NMOS_PORT", "3312", true, false, "NMOS Node and Connection APIs; NMOS_PORT+1 is reserved"},
                {"NMOS_SEED", "", true, false, "Seed of every UUIDv5 id (default: hostname)"},
                {"NMOS_LABEL", "MXL Browser Source", true, false, "Node and device label"},
                {"NMOS_TAGS", "{}", true, false, "JSON tags on node and device"},
                {"NMOS_REGISTRY_ADDRESS", "", true, false, "Static NMOS registry address (empty: no registration)"},
                {"NMOS_REGISTRY_PORT", "3210", true, false, "Registration API port"},
                {"NMOS_QUERY_ADDRESS", "", true, false, "Query API address (default: the registry address)"},
                {"NMOS_QUERY_PORT", "", true, false, "Query API port (default: registry port + 1)"},
                {"NMOS_DNS_SD", "false", true, false, "DNS-SD browse and advertisement"},
                {"NMOS_HOST_ADDRESS", "", true, false, "The only address ever announced (IPv4 literal; default: first non-loopback)"},
                {"MXL_DOMAIN_SCAN_PATH", "/Volumes/mxl", true, false, "MXL root; must be a tmpfs"},
                {"MXL_OUTPUT_DOMAIN_DIR", "", true, false, "Own MXL domain (default: <root>/browser-source-<seed>)"},
                {"MXL_OUTPUT_DOMAIN_ID", "", true, false, "Own domain id (default: UUIDv5 of the seed)"},
                {"MXL_HISTORY_DURATION_NS", "200000000", true, false, "Ring history written once into options.json"},
                {"MXL_CLEANUP_ON_EXIT", "false", false, false, "Remove the own domain on SIGTERM"},
                {"SHUTDOWN_TIMEOUT_S", "10", false, false, "SIGTERM budget in seconds"},
                {"LOG_LEVEL", "info", false, false, "trace, debug, info, warn, error"},
                {"LOG_FORMAT", "json", false, false, "json or text"},
                {"BROWSER_STATE_DIR", "/config", true, false, "All state: config, templates, fonts, profile"},
                {"BROWSER_CONFIG_FILE", "", true, false, "Config document (default: <state>/config.json)"},
                {"BROWSER_LABEL", "Browser", true, false, "Sender label prefix and group hint"},
                {"BROWSER_FORMAT", "1080p50", true, false, "720p50, 1080p25, 1080p29.97, 1080p50, 1080p59.94"},
                {"BROWSER_KEY_MODE", "off", true, false, "off, v210a, fill_key"},
                {"BROWSER_FILL", "straight", true, false, "straight or premultiplied fill when keyed"},
                {"BROWSER_AUDIO_CHANNELS", "2", true, false, "0, 2, 8 or 16"},
                {"BROWSER_AUDIO_BUFFER_MS", "60", true, false, "Audio FIFO target"},
                {"BROWSER_AV_OFFSET_MS", "0", false, false, "Audio trim; positive delays audio"},
                {"BROWSER_VIDEO_DELAY_GRAINS", "auto", true, false, "Video delay in grains for A/V alignment (auto or 0-10)"},
                {"BROWSER_FRAME_LEAD", "1", true, false, "Video latency in periods assumed by BROWSER_VIDEO_DELAY_GRAINS=auto (1-2)"},
                {"BROWSER_RENDER", "auto", true, false, "auto, gpu, software"},
                {"BROWSER_ON_PAGE_ERROR", "hold", false, false, "hold, transparent, black, slate"},
                {"BROWSER_HANG_TIMEOUT_MS", "3000", false, false, "The renderer does not answer a liveness probe for this long: hang"},
                {"BROWSER_MAX_RESIDENT_MB", "0", false, false, "Memory warning threshold of all CEF processes (0 = off)"},
                {"BROWSER_POPUPS", "block", false, false, "block or same_window"},
                {"BROWSER_CONFIRM_DIALOGS", "cancel", false, false, "cancel or accept"},
                {"BROWSER_URL_ALLOW", "", false, false, "Comma list of host globs and CIDRs; when set, only these"},
                {"BROWSER_URL_DENY", "", false, false, "Comma list of host globs and CIDRs, added to the built-in deny list"},
                {"BROWSER_PREVIEW_FPS", "10", false, false, "Preview frames per second (1-25)"},
                {"BROWSER_PREVIEW_WIDTH", "960", false, false, "Preview width in pixels"},
                {"BROWSER_INTERACT_TIMEOUT_S", "120", false, false, "Interaction ends after this idle time"},
                {"BROWSER_DEVTOOLS", "false", true, false, "DevTools proxy under /devtools/"},
                {"BROWSER_DEVTOOLS_PORT", "9222", true, false, "Internal DevTools port (loopback only)"},
                {"BROWSER_API_TOKEN", "", true, true, "Optional bearer token for the API"},
                {"BROWSER_PROFILE", "ephemeral", true, false, "ephemeral or persistent"},
                {"BROWSER_WEBAUTHN", "false", true, false, "Offer WebAuthn (passkeys) to pages; off: sign-in pages fall back to other methods"},
                {"BROWSER_TEMPLATES_DIR", "", true, false, "Local templates (default: <state>/templates)"},
                {"BROWSER_FONTS_DIR", "", true, false, "Extra fonts (default: <state>/fonts)"},
                {"BROWSER_CA_DIR", "/etc/mxl-browser-source/ca", true, false, "PEM files imported into the NSS database"},
                {"HTTPS_PROXY", "", true, false, "Proxy for page requests"},
                {"NO_PROXY", "", true, false, "Proxy bypass list"},
                {"BROWSER_REQUIRE_TMPFS", "true", true, false, "Refuse to start when the MXL root is not a tmpfs"},
                {"BROWSER_CHROMIUM_FLAGS_APPEND", "", true, false, "Extra Chromium switches, comma separated (expert)"},
            };
            return table;
        }

        std::string trim(std::string s)
        {
            auto const notSpace = [](unsigned char c) { return std::isspace(c) == 0; };
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
            s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
            return s;
        }

        std::string lower(std::string s)
        {
            for (auto& c : s)
            {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return s;
        }

        int parseInt(std::string const& key, std::string const& value, int lo, int hi)
        {
            try
            {
                std::size_t pos = 0;
                long const n = std::stol(value, &pos);
                if (pos != value.size())
                {
                    throw std::invalid_argument("trailing");
                }
                if (n < lo || n > hi)
                {
                    throw ConfigError(key, "must be between " + std::to_string(lo) + " and " + std::to_string(hi));
                }
                return static_cast<int>(n);
            }
            catch (ConfigError const&)
            {
                throw;
            }
            catch (std::exception const&)
            {
                throw ConfigError(key, "not an integer: '" + value + "'");
            }
        }

        bool parseBool(std::string const& key, std::string const& value)
        {
            auto const v = lower(value);
            if (v == "true" || v == "1" || v == "yes" || v == "on")
            {
                return true;
            }
            if (v == "false" || v == "0" || v == "no" || v == "off")
            {
                return false;
            }
            throw ConfigError(key, "not a boolean: '" + value + "'");
        }

        std::vector<std::string> parseList(std::string const& value)
        {
            std::vector<std::string> out;
            std::stringstream in(value);
            std::string item;
            while (std::getline(in, item, ','))
            {
                item = trim(item);
                if (!item.empty())
                {
                    out.push_back(item);
                }
            }
            return out;
        }

        bool ipv4Literal(std::string const& s)
        {
            in_addr addr{};
            return inet_pton(AF_INET, s.c_str(), &addr) == 1;
        }

        template<typename E>
        E parseEnum(std::string const& key, std::string const& value, std::vector<std::pair<char const*, E>> const& options)
        {
            auto const v = lower(value);
            std::string names;
            for (auto const& [name, e] : options)
            {
                if (v == name)
                {
                    return e;
                }
                names += names.empty() ? name : std::string(", ") + name;
            }
            throw ConfigError(key, "must be one of " + names + " (got '" + value + "')");
        }
    }

    Format parseFormat(std::string const& name)
    {
        auto const n = lower(trim(name));
        struct Row
        {
            char const* name;
            std::uint32_t w, h;
            std::int64_t num, den;
        };
        static Row const rows[] = {
            {"720p50", 1280, 720, 50, 1},
            {"1080p25", 1920, 1080, 25, 1},
            {"1080p29.97", 1920, 1080, 30000, 1001},
            {"1080p50", 1920, 1080, 50, 1},
            {"1080p59.94", 1920, 1080, 60000, 1001},
        };
        for (auto const& r : rows)
        {
            if (n == r.name)
            {
                return Format{r.name, r.w, r.h, r.num, r.den};
            }
        }
        if (n.rfind("2160p", 0) == 0)
        {
            throw ConfigError("BROWSER_FORMAT", "2160p is a later stage (needs the GPU conversion path, SPEC §5.6)");
        }
        if (n.find('i') != std::string::npos)
        {
            throw ConfigError("BROWSER_FORMAT", "interlaced formats come with v1.1 (SPEC §5.6)");
        }
        throw ConfigError("BROWSER_FORMAT", "unknown format '" + name + "' (720p50, 1080p25, 1080p29.97, 1080p50, 1080p59.94)");
    }

    std::string toString(KeyMode mode)
    {
        switch (mode)
        {
        case KeyMode::V210a:
            return "v210a";
        case KeyMode::FillKey:
            return "fill_key";
        default:
            return "off";
        }
    }

    std::string toString(Render mode)
    {
        switch (mode)
        {
        case Render::Gpu:
            return "gpu";
        case Render::Software:
            return "software";
        default:
            return "auto";
        }
    }

    std::vector<std::string> const& knownKeys()
    {
        static std::vector<std::string> const keys = [] {
            std::vector<std::string> out;
            for (auto const& d : defs())
            {
                out.emplace_back(d.key);
            }
            return out;
        }();
        return keys;
    }

    bool isKnownKey(std::string const& key)
    {
        auto const& keys = knownKeys();
        return std::find(keys.begin(), keys.end(), key) != keys.end();
    }

    bool isRestartKey(std::string const& key)
    {
        for (auto const& d : defs())
        {
            if (key == d.key)
            {
                return d.restart;
            }
        }
        return true;
    }

    Loaded load(std::map<std::string, std::string> const& env, std::map<std::string, std::string> const& file, std::string const& hostname)
    {
        Loaded out;
        std::map<std::string, std::string> values;
        for (auto const& d : defs())
        {
            SettingInfo info;
            info.key = d.key;
            info.defaultValue = d.defaultValue;
            info.restart = d.restart;
            info.secret = d.secret;
            info.description = d.description;
            std::optional<std::string> value;
            auto envIt = env.find(d.key);
            if (envIt == env.end() && (info.key == "HTTPS_PROXY" || info.key == "NO_PROXY"))
            {
                envIt = env.find(lower(info.key));
            }
            if (envIt != env.end())
            {
                value = envIt->second;
                info.source = "env";
            }
            else if (auto const fileIt = file.find(d.key); fileIt != file.end())
            {
                value = fileIt->second;
                info.source = "file";
            }
            else
            {
                info.source = "default";
            }
            info.value = value ? trim(*value) : info.defaultValue;
            values[info.key] = info.value;
            out.table.push_back(std::move(info));
        }
        auto const get = [&](char const* key) { return values.at(key); };
        auto& c = out.config;

        c.webPort = parseInt("WEB_PORT", get("WEB_PORT"), 1, 65535);
        c.nmosPort = parseInt("NMOS_PORT", get("NMOS_PORT"), 1, 65534);
        if (c.nmosPort == c.webPort || c.nmosPort + 1 == c.webPort)
        {
            throw ConfigError("NMOS_PORT", "NMOS_PORT and NMOS_PORT+1 must differ from WEB_PORT");
        }
        c.nmosSeed = get("NMOS_SEED").empty() ? hostname : get("NMOS_SEED");
        c.nmosLabel = get("NMOS_LABEL");
        c.nmosTagsJson = get("NMOS_TAGS").empty() ? "{}" : get("NMOS_TAGS");
        {
            picojson::value v;
            auto const err = picojson::parse(v, c.nmosTagsJson);
            if (!err.empty() || !v.is<picojson::object>())
            {
                throw ConfigError("NMOS_TAGS", "must be a JSON object of string arrays");
            }
            for (auto const& [name, values] : v.get<picojson::object>())
            {
                if (!values.is<picojson::array>())
                {
                    throw ConfigError("NMOS_TAGS", "tag '" + name + "' must be an array of strings");
                }
                for (auto const& item : values.get<picojson::array>())
                {
                    if (!item.is<std::string>())
                    {
                        throw ConfigError("NMOS_TAGS", "tag '" + name + "' must be an array of strings");
                    }
                    c.nmosTags[name].push_back(item.get<std::string>());
                }
            }
        }
        c.nmosRegistryAddress = get("NMOS_REGISTRY_ADDRESS");
        c.nmosRegistryPort = parseInt("NMOS_REGISTRY_PORT", get("NMOS_REGISTRY_PORT"), 1, 65535);
        c.nmosQueryAddress = get("NMOS_QUERY_ADDRESS").empty() ? c.nmosRegistryAddress : get("NMOS_QUERY_ADDRESS");
        c.nmosQueryPort = get("NMOS_QUERY_PORT").empty() ? c.nmosRegistryPort + 1 : parseInt("NMOS_QUERY_PORT", get("NMOS_QUERY_PORT"), 1, 65535);
        c.nmosDnsSd = parseBool("NMOS_DNS_SD", get("NMOS_DNS_SD"));
        c.nmosHostAddress = get("NMOS_HOST_ADDRESS");
        if (!c.nmosHostAddress.empty())
        {
            if (!ipv4Literal(c.nmosHostAddress) || c.nmosHostAddress == "0.0.0.0" || c.nmosHostAddress.rfind("127.", 0) == 0)
            {
                throw ConfigError("NMOS_HOST_ADDRESS", "must be a routable IPv4 literal (not 0.0.0.0 or loopback)");
            }
        }

        c.mxlScanPath = get("MXL_DOMAIN_SCAN_PATH");
        if (c.mxlScanPath.empty())
        {
            throw ConfigError("MXL_DOMAIN_SCAN_PATH", "must not be empty");
        }
        c.mxlOutputDomainDir = get("MXL_OUTPUT_DOMAIN_DIR").empty() ? c.mxlScanPath + "/browser-source-" + c.nmosSeed : get("MXL_OUTPUT_DOMAIN_DIR");
        c.mxlOutputDomainId = get("MXL_OUTPUT_DOMAIN_ID");
        {
            auto const h = get("MXL_HISTORY_DURATION_NS");
            try
            {
                std::size_t pos = 0;
                c.mxlHistoryNs = std::stoull(h, &pos);
                if (pos != h.size() || c.mxlHistoryNs < 40000000ull || c.mxlHistoryNs > 10000000000ull)
                {
                    throw std::invalid_argument("range");
                }
            }
            catch (std::exception const&)
            {
                throw ConfigError("MXL_HISTORY_DURATION_NS", "must be between 40000000 (40 ms) and 10000000000 (10 s)");
            }
        }
        c.mxlCleanupOnExit = parseBool("MXL_CLEANUP_ON_EXIT", get("MXL_CLEANUP_ON_EXIT"));
        c.shutdownTimeoutS = parseInt("SHUTDOWN_TIMEOUT_S", get("SHUTDOWN_TIMEOUT_S"), 1, 300);
        c.logLevel = lower(get("LOG_LEVEL"));
        if (c.logLevel != "trace" && c.logLevel != "debug" && c.logLevel != "info" && c.logLevel != "warn" && c.logLevel != "error")
        {
            throw ConfigError("LOG_LEVEL", "must be trace, debug, info, warn or error");
        }
        c.logFormat = lower(get("LOG_FORMAT"));
        if (c.logFormat != "json" && c.logFormat != "text")
        {
            throw ConfigError("LOG_FORMAT", "must be json or text");
        }

        c.stateDir = get("BROWSER_STATE_DIR");
        c.configFile = get("BROWSER_CONFIG_FILE").empty() ? c.stateDir + "/config.json" : get("BROWSER_CONFIG_FILE");
        c.label = get("BROWSER_LABEL");
        if (c.label.empty() || c.label.find(':') != std::string::npos)
        {
            throw ConfigError("BROWSER_LABEL", "must not be empty or contain ':'");
        }
        c.format = parseFormat(get("BROWSER_FORMAT"));
        c.keyMode = parseEnum<KeyMode>("BROWSER_KEY_MODE", get("BROWSER_KEY_MODE"),
            {{"off", KeyMode::Off}, {"v210a", KeyMode::V210a}, {"fill_key", KeyMode::FillKey}});
        c.fill = parseEnum<Fill>("BROWSER_FILL", get("BROWSER_FILL"), {{"straight", Fill::Straight}, {"premultiplied", Fill::Premultiplied}});
        c.audioChannels = parseInt("BROWSER_AUDIO_CHANNELS", get("BROWSER_AUDIO_CHANNELS"), 0, 16);
        if (c.audioChannels != 0 && c.audioChannels != 2 && c.audioChannels != 8 && c.audioChannels != 16)
        {
            throw ConfigError("BROWSER_AUDIO_CHANNELS", "must be 0, 2, 8 or 16");
        }
        c.audioBufferMs = parseInt("BROWSER_AUDIO_BUFFER_MS", get("BROWSER_AUDIO_BUFFER_MS"), 20, 500);
        c.avOffsetMs = parseInt("BROWSER_AV_OFFSET_MS", get("BROWSER_AV_OFFSET_MS"), -500, 500);
        c.videoDelayGrains = lower(get("BROWSER_VIDEO_DELAY_GRAINS")) == "auto" ? -1 : parseInt("BROWSER_VIDEO_DELAY_GRAINS", get("BROWSER_VIDEO_DELAY_GRAINS"), 0, 10);
        c.frameLead = parseInt("BROWSER_FRAME_LEAD", get("BROWSER_FRAME_LEAD"), 1, 2);
        c.render = parseEnum<Render>("BROWSER_RENDER", get("BROWSER_RENDER"), {{"auto", Render::Auto}, {"gpu", Render::Gpu}, {"software", Render::Software}});
        c.onPageError = parseEnum<PageErrorMode>("BROWSER_ON_PAGE_ERROR", get("BROWSER_ON_PAGE_ERROR"),
            {{"hold", PageErrorMode::Hold}, {"transparent", PageErrorMode::Transparent}, {"black", PageErrorMode::Black}, {"slate", PageErrorMode::Slate}});
        c.hangTimeoutMs = parseInt("BROWSER_HANG_TIMEOUT_MS", get("BROWSER_HANG_TIMEOUT_MS"), 500, 60000);
        c.maxResidentMb = parseInt("BROWSER_MAX_RESIDENT_MB", get("BROWSER_MAX_RESIDENT_MB"), 0, 1 << 20);
        c.popupsSameWindow = parseEnum<bool>("BROWSER_POPUPS", get("BROWSER_POPUPS"), {{"block", false}, {"same_window", true}});
        c.confirmAccept = parseEnum<bool>("BROWSER_CONFIRM_DIALOGS", get("BROWSER_CONFIRM_DIALOGS"), {{"cancel", false}, {"accept", true}});
        c.urlAllow = parseList(get("BROWSER_URL_ALLOW"));
        c.urlDeny = parseList(get("BROWSER_URL_DENY"));
        c.previewFps = parseInt("BROWSER_PREVIEW_FPS", get("BROWSER_PREVIEW_FPS"), 1, 25);
        c.previewWidth = parseInt("BROWSER_PREVIEW_WIDTH", get("BROWSER_PREVIEW_WIDTH"), 160, 1920);
        c.interactTimeoutS = parseInt("BROWSER_INTERACT_TIMEOUT_S", get("BROWSER_INTERACT_TIMEOUT_S"), 5, 3600);
        c.devtools = parseBool("BROWSER_DEVTOOLS", get("BROWSER_DEVTOOLS"));
        c.devtoolsPort = parseInt("BROWSER_DEVTOOLS_PORT", get("BROWSER_DEVTOOLS_PORT"), 1, 65535);
        c.apiToken = get("BROWSER_API_TOKEN");
        c.persistentProfile = parseEnum<bool>("BROWSER_PROFILE", get("BROWSER_PROFILE"), {{"ephemeral", false}, {"persistent", true}});
        c.webauthn = parseBool("BROWSER_WEBAUTHN", get("BROWSER_WEBAUTHN"));
        c.templatesDir = get("BROWSER_TEMPLATES_DIR").empty() ? c.stateDir + "/templates" : get("BROWSER_TEMPLATES_DIR");
        c.fontsDir = get("BROWSER_FONTS_DIR").empty() ? c.stateDir + "/fonts" : get("BROWSER_FONTS_DIR");
        c.caDir = get("BROWSER_CA_DIR");
        c.httpsProxy = get("HTTPS_PROXY");
        c.noProxy = get("NO_PROXY");
        c.requireTmpfs = parseBool("BROWSER_REQUIRE_TMPFS", get("BROWSER_REQUIRE_TMPFS"));
        c.chromiumFlagsAppend = parseList(get("BROWSER_CHROMIUM_FLAGS_APPEND"));
        if (c.devtools && c.devtoolsPort == c.webPort)
        {
            throw ConfigError("BROWSER_DEVTOOLS_PORT", "must differ from WEB_PORT");
        }
        return out;
    }

    std::map<std::string, std::string> processEnvironment()
    {
        std::map<std::string, std::string> out;
        for (char** e = environ; e != nullptr && *e != nullptr; ++e)
        {
            std::string const entry(*e);
            auto const eq = entry.find('=');
            if (eq != std::string::npos)
            {
                out[entry.substr(0, eq)] = entry.substr(eq + 1);
            }
        }
        return out;
    }
}
