// SPDX-License-Identifier: MIT
#include "config/document.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "config/config.hpp"

namespace mbs::config
{
    namespace
    {
        std::string const kSchemaVersionKey = "version";

        bool isNumber(picojson::value const& v)
        {
            return v.is<double>();
        }
    }

    std::optional<std::uint32_t> parseBackground(std::string const& text)
    {
        if (text == "transparent")
        {
            return 0x00000000u;
        }
        if (text.size() != 7 || text[0] != '#')
        {
            return std::nullopt;
        }
        std::uint32_t rgb = 0;
        for (std::size_t i = 1; i < 7; ++i)
        {
            char const c = static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
            int d = 0;
            if (c >= '0' && c <= '9')
            {
                d = c - '0';
            }
            else if (c >= 'a' && c <= 'f')
            {
                d = c - 'a' + 10;
            }
            else
            {
                return std::nullopt;
            }
            rgb = (rgb << 4) | static_cast<std::uint32_t>(d);
        }
        return 0xFF000000u | rgb;
    }

    std::optional<std::string> mergeSource(picojson::object const& in, Source& base)
    {
        Source s = base;
        for (auto const& [key, v] : in)
        {
            if (key == "url")
            {
                if (!v.is<std::string>() || v.get<std::string>().empty())
                {
                    return "url must be a non-empty string";
                }
                s.url = v.get<std::string>();
            }
            else if (key == "background")
            {
                if (!v.is<std::string>() || !parseBackground(v.get<std::string>()))
                {
                    return "background must be 'transparent' or '#RRGGBB'";
                }
                s.background = v.get<std::string>();
            }
            else if (key == "zoom")
            {
                if (!isNumber(v) || v.get<double>() < 0.25 || v.get<double>() > 5.0)
                {
                    return "zoom must be between 0.25 and 5";
                }
                s.zoom = v.get<double>();
            }
            else if (key == "device_scale_factor")
            {
                if (!isNumber(v) || v.get<double>() < 0.5 || v.get<double>() > 4.0)
                {
                    return "device_scale_factor must be between 0.5 and 4";
                }
                s.deviceScaleFactor = v.get<double>();
            }
            else if (key == "css" || key == "js" || key == "user_agent_suffix")
            {
                if (!v.is<std::string>())
                {
                    return key + " must be a string";
                }
                (key == "css" ? s.css : key == "js" ? s.js : s.userAgentSuffix) = v.get<std::string>();
            }
            else if (key == "reload_interval_s")
            {
                if (!isNumber(v) || v.get<double>() < 0 || v.get<double>() > 86400)
                {
                    return "reload_interval_s must be between 0 and 86400";
                }
                s.reloadIntervalS = static_cast<int>(v.get<double>());
            }
            else if (key == "audio")
            {
                if (!v.is<bool>())
                {
                    return "audio must be a boolean";
                }
                s.audio = v.get<bool>();
            }
            else if (key != "name")
            {
                return "unknown source field '" + key + "'";
            }
        }
        base = s;
        return std::nullopt;
    }

    picojson::value sourceToJson(Source const& s)
    {
        picojson::object o;
        o["url"] = picojson::value(s.url);
        o["background"] = picojson::value(s.background);
        o["zoom"] = picojson::value(s.zoom);
        o["device_scale_factor"] = picojson::value(s.deviceScaleFactor);
        o["css"] = picojson::value(s.css);
        o["js"] = picojson::value(s.js);
        o["user_agent_suffix"] = picojson::value(s.userAgentSuffix);
        o["reload_interval_s"] = picojson::value(static_cast<double>(s.reloadIntervalS));
        o["audio"] = picojson::value(s.audio);
        return picojson::value(o);
    }

    Document::Document(std::string path)
        : _path(std::move(path))
    {
    }

    void Document::load()
    {
        std::ifstream in(_path);
        if (!in)
        {
            return;
        }
        std::stringstream text;
        text << in.rdbuf();
        picojson::value doc;
        auto const err = picojson::parse(doc, text.str());
        if (!err.empty())
        {
            throw std::runtime_error(_path + " is not valid JSON: " + err);
        }
        std::lock_guard lock{_mu};
        if (auto const e = replaceLocked(doc))
        {
            throw std::runtime_error(_path + ": " + *e);
        }
    }

    std::optional<std::string> Document::importDocument(picojson::value const& doc)
    {
        std::lock_guard lock{_mu};
        if (auto const e = replaceLocked(doc))
        {
            return e;
        }
        if (!saveLocked())
        {
            return std::string("cannot write ") + _path;
        }
        return std::nullopt;
    }

    std::optional<std::string> Document::replaceLocked(picojson::value const& doc)
    {
        if (!doc.is<picojson::object>())
        {
            return "the config document must be a JSON object";
        }
        auto const& o = doc.get<picojson::object>();
        std::map<std::string, std::string> settings;
        Source source;
        std::vector<Preset> presets;
        std::map<std::string, bool> senders;
        if (auto const it = o.find("settings"); it != o.end())
        {
            if (!it->second.is<picojson::object>())
            {
                return "settings must be an object";
            }
            for (auto const& [key, v] : it->second.get<picojson::object>())
            {
                if (!isKnownKey(key))
                {
                    return "unknown setting '" + key + "'";
                }
                if (v.is<std::string>())
                {
                    settings[key] = v.get<std::string>();
                }
                else if (v.is<double>() || v.is<bool>())
                {
                    settings[key] = v.to_str();
                }
                else
                {
                    return "setting '" + key + "' must be a string";
                }
            }
        }
        if (auto const it = o.find("source"); it != o.end())
        {
            if (!it->second.is<picojson::object>())
            {
                return "source must be an object";
            }
            if (auto const e = mergeSource(it->second.get<picojson::object>(), source))
            {
                return "source: " + *e;
            }
        }
        if (auto const it = o.find("presets"); it != o.end())
        {
            if (!it->second.is<picojson::array>())
            {
                return "presets must be an array";
            }
            for (auto const& item : it->second.get<picojson::array>())
            {
                if (!item.is<picojson::object>())
                {
                    return "each preset must be an object";
                }
                auto const& po = item.get<picojson::object>();
                auto const name = po.find("name");
                if (name == po.end() || !name->second.is<std::string>() || name->second.get<std::string>().empty())
                {
                    return "each preset needs a name";
                }
                Preset p;
                p.name = name->second.get<std::string>();
                if (auto const e = mergeSource(po, p.source))
                {
                    return "preset '" + p.name + "': " + *e;
                }
                presets.push_back(std::move(p));
            }
        }
        if (auto const it = o.find("senders"); it != o.end())
        {
            if (!it->second.is<picojson::object>())
            {
                return "senders must be an object";
            }
            for (auto const& [name, v] : it->second.get<picojson::object>())
            {
                if (!v.is<bool>())
                {
                    return "senders." + name + " must be a boolean";
                }
                senders[name] = v.get<bool>();
            }
        }
        _settings = std::move(settings);
        _source = std::move(source);
        _presets = std::move(presets);
        _senders = std::move(senders);
        return std::nullopt;
    }

    std::map<std::string, std::string> Document::settings() const
    {
        std::lock_guard lock{_mu};
        return _settings;
    }

    Source Document::source() const
    {
        std::lock_guard lock{_mu};
        return _source;
    }

    std::vector<Preset> Document::presets() const
    {
        std::lock_guard lock{_mu};
        return _presets;
    }

    std::map<std::string, bool> Document::senders() const
    {
        std::lock_guard lock{_mu};
        return _senders;
    }

    bool Document::setSettings(std::map<std::string, std::string> settings)
    {
        std::lock_guard lock{_mu};
        _settings = std::move(settings);
        return saveLocked();
    }

    bool Document::setSource(Source source)
    {
        std::lock_guard lock{_mu};
        _source = std::move(source);
        return saveLocked();
    }

    bool Document::upsertPreset(Preset preset)
    {
        std::lock_guard lock{_mu};
        for (auto& p : _presets)
        {
            if (p.name == preset.name)
            {
                p = std::move(preset);
                return saveLocked();
            }
        }
        _presets.push_back(std::move(preset));
        return saveLocked();
    }

    bool Document::deletePreset(std::string const& name)
    {
        std::lock_guard lock{_mu};
        auto const before = _presets.size();
        std::erase_if(_presets, [&](Preset const& p) { return p.name == name; });
        if (_presets.size() == before)
        {
            return false;
        }
        return saveLocked();
    }

    bool Document::setSender(std::string const& name, bool enabled)
    {
        std::lock_guard lock{_mu};
        _senders[name] = enabled;
        return saveLocked();
    }

    picojson::value Document::toJson(std::vector<std::string> const& omit) const
    {
        std::lock_guard lock{_mu};
        return toJsonLocked(omit);
    }

    picojson::value Document::toJsonLocked(std::vector<std::string> const& omit) const
    {
        picojson::object o;
        o[kSchemaVersionKey] = picojson::value(1.0);
        picojson::object settings;
        {
            for (auto const& [key, value] : _settings)
            {
                if (std::find(omit.begin(), omit.end(), key) == omit.end())
                {
                    settings[key] = picojson::value(value);
                }
            }
        }
        o["settings"] = picojson::value(settings);
        o["source"] = sourceToJson(_source);
        picojson::array presets;
        for (auto const& p : _presets)
        {
            auto v = sourceToJson(p.source);
            v.get<picojson::object>()["name"] = picojson::value(p.name);
            presets.push_back(v);
        }
        o["presets"] = picojson::value(presets);
        picojson::object senders;
        for (auto const& [name, enabled] : _senders)
        {
            senders[name] = picojson::value(enabled);
        }
        o["senders"] = picojson::value(senders);
        return picojson::value(o);
    }

    bool Document::saveLocked() const
    {
        std::error_code ec;
        auto const dir = std::filesystem::path(_path).parent_path();
        if (!dir.empty())
        {
            std::filesystem::create_directories(dir, ec);
        }
        auto const tmp = _path + ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            if (!out)
            {
                return false;
            }
            out << toJsonLocked({}).serialize(true);
            if (!out)
            {
                return false;
            }
        }
        std::filesystem::rename(tmp, _path, ec);
        return !ec;
    }
}
