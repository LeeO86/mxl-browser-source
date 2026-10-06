// SPDX-License-Identifier: MIT
// The config document (SPEC §11): settings layer, source, presets, sender enable
// states. Written atomically by the UI and the API; a hand edit is read at start.
#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "picojson/picojson.h"

namespace mbs::config
{
    struct Source
    {
        std::string url = "https://templates.local/blank.html";
        std::string background = "transparent"; // or #RRGGBB
        double zoom = 1.0;
        double deviceScaleFactor = 1.0;
        std::string css;
        std::string js;
        std::string userAgentSuffix;
        int reloadIntervalS = 0;
        bool audio = true;

        bool operator==(Source const&) const = default;
    };

    struct Preset
    {
        std::string name;
        Source source;
    };

    /// Parses `#RRGGBB` or `transparent` into 0xAARRGGBB; nullopt when invalid.
    std::optional<std::uint32_t> parseBackground(std::string const& text);

    /// Source fields from JSON; only present fields change `base`. Returns an error text.
    std::optional<std::string> mergeSource(picojson::object const& in, Source& base);
    picojson::value sourceToJson(Source const& s);

    class Document
    {
    public:
        explicit Document(std::string path);

        /// Reads the file if it exists. Throws std::runtime_error when it is not valid JSON.
        void load();

        [[nodiscard]] std::map<std::string, std::string> settings() const;
        [[nodiscard]] Source source() const;
        [[nodiscard]] std::vector<Preset> presets() const;
        [[nodiscard]] std::map<std::string, bool> senders() const;
        [[nodiscard]] std::string path() const
        {
            return _path;
        }

        /// Each setter writes the file atomically. Returns false when the write failed.
        bool setSettings(std::map<std::string, std::string> settings);
        bool setSource(Source source);
        bool upsertPreset(Preset preset);
        bool deletePreset(std::string const& name);
        bool setSender(std::string const& name, bool enabled);

        /// The whole document (export). Secrets in `omit` are left out of the settings.
        [[nodiscard]] picojson::value toJson(std::vector<std::string> const& omit = {}) const;
        /// Replaces the document from an export (import) and writes it. Returns an error text.
        std::optional<std::string> importDocument(picojson::value const& doc);

    private:
        std::optional<std::string> replaceLocked(picojson::value const& doc);
        [[nodiscard]] picojson::value toJsonLocked(std::vector<std::string> const& omit) const;
        bool saveLocked() const;

        mutable std::mutex _mu;
        std::string _path;
        std::map<std::string, std::string> _settings;
        Source _source;
        std::vector<Preset> _presets;
        std::map<std::string, bool> _senders;
    };
}
