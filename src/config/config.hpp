// SPDX-License-Identifier: MIT
// Settings (SPEC §11): environment > config file > defaults. Invalid values throw
// ConfigError; the process exits 78.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace mbs::config
{
    class ConfigError : public std::runtime_error
    {
    public:
        ConfigError(std::string key, std::string const& reason)
            : std::runtime_error(key + ": " + reason)
            , _key(std::move(key))
        {
        }
        [[nodiscard]] std::string const& key() const
        {
            return _key;
        }

    private:
        std::string _key;
    };

    struct Format
    {
        std::string name = "1080p50";
        std::uint32_t width = 1920;
        std::uint32_t height = 1080;
        std::int64_t rateNum = 50;
        std::int64_t rateDen = 1;
    };

    /// Parses a format name (`1080p50`, `720p50`, `1080p59.94` …). Throws ConfigError.
    Format parseFormat(std::string const& name);

    enum class KeyMode
    {
        Off,
        V210a,
        FillKey
    };
    enum class Fill
    {
        Straight,
        Premultiplied
    };
    enum class Render
    {
        Auto,
        Gpu,
        Software
    };
    enum class PageErrorMode
    {
        Hold,
        Transparent,
        Black,
        Slate
    };

    std::string toString(KeyMode mode);
    std::string toString(Render mode);

    struct Config
    {
        int webPort = 8160;
        int nmosPort = 3312;
        std::string nmosSeed;
        std::string nmosLabel = "MXL Browser Source";
        std::map<std::string, std::vector<std::string>> nmosTags;
        std::string nmosTagsJson = "{}";
        std::string nmosRegistryAddress;
        int nmosRegistryPort = 3210;
        std::string nmosQueryAddress;
        int nmosQueryPort = 3211;
        bool nmosDnsSd = false;
        std::string nmosHostAddress;

        std::string mxlScanPath = "/Volumes/mxl";
        std::string mxlOutputDomainDir;
        std::string mxlOutputDomainId;
        std::uint64_t mxlHistoryNs = 200000000;
        bool mxlCleanupOnExit = false;
        int shutdownTimeoutS = 10;
        std::string logLevel = "info";
        std::string logFormat = "json";

        std::string stateDir = "/config";
        std::string configFile;
        std::string label = "Browser";
        Format format;
        KeyMode keyMode = KeyMode::Off;
        Fill fill = Fill::Straight;
        int audioChannels = 2;
        int audioBufferMs = 60;
        int avOffsetMs = 0;
        int videoDelayGrains = -1; // -1 = auto
        int frameLead = 1;
        Render render = Render::Auto;
        PageErrorMode onPageError = PageErrorMode::Hold;
        int hangTimeoutMs = 3000;
        int maxResidentMb = 0;
        bool popupsSameWindow = false;
        bool confirmAccept = false;
        std::vector<std::string> urlAllow;
        std::vector<std::string> urlDeny;
        int previewFps = 10;
        int previewWidth = 960;
        int interactTimeoutS = 120;
        bool devtools = false;
        int devtoolsPort = 9222;
        std::string apiToken;
        bool persistentProfile = false;
        bool webauthn = false;
        std::string templatesDir;
        std::string fontsDir;
        std::string caDir = "/etc/mxl-browser-source/ca";
        std::string httpsProxy;
        std::string noProxy;
        bool requireTmpfs = true;
        std::vector<std::string> chromiumFlagsAppend;
    };

    /// One row of the settings table (GET /api/v1/config).
    struct SettingInfo
    {
        std::string key;
        std::string value;
        std::string defaultValue;
        std::string source; // env | file | default
        bool restart = true;
        bool secret = false;
        std::string description;
    };

    struct Loaded
    {
        Config config;
        std::vector<SettingInfo> table;
    };

    /// All known setting keys (environment names), in table order.
    std::vector<std::string> const& knownKeys();
    [[nodiscard]] bool isKnownKey(std::string const& key);
    [[nodiscard]] bool isRestartKey(std::string const& key);

    /// Builds the configuration. `env` holds the process environment (unknown keys are
    /// ignored), `file` the settings layer of the config document. Throws ConfigError.
    Loaded load(std::map<std::string, std::string> const& env, std::map<std::string, std::string> const& file,
        std::string const& hostname);

    /// The process environment as a map.
    std::map<std::string, std::string> processEnvironment();
}
