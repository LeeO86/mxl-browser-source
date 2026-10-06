// SPDX-License-Identifier: MIT
#include "ops/urlpolicy.hpp"

#include <arpa/inet.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>

namespace mbs::ops
{
    namespace
    {
        std::string lower(std::string_view text)
        {
            std::string out(text);
            std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return out;
        }

        // 16 bytes for IPv6, 4 for IPv4; an IPv4-mapped IPv6 address becomes its IPv4 form, so
        // [::ffff:127.0.0.1] is loopback like 127.0.0.1.
        std::optional<std::vector<std::uint8_t>> addressBytes(std::string_view text)
        {
            std::string const s(text);
            std::array<std::uint8_t, 16> buf{};
            if (::inet_pton(AF_INET, s.c_str(), buf.data()) == 1)
            {
                return std::vector<std::uint8_t>(buf.begin(), buf.begin() + 4);
            }
            if (::inet_pton(AF_INET6, s.c_str(), buf.data()) == 1)
            {
                static constexpr std::uint8_t kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
                if (std::memcmp(buf.data(), kMapped, 12) == 0)
                {
                    return std::vector<std::uint8_t>(buf.begin() + 12, buf.end());
                }
                return std::vector<std::uint8_t>(buf.begin(), buf.end());
            }
            return std::nullopt;
        }

        int defaultPort(std::string const& scheme)
        {
            if (scheme == "http" || scheme == "ws")
            {
                return 80;
            }
            if (scheme == "https" || scheme == "wss")
            {
                return 443;
            }
            return 0;
        }

        bool endsWith(std::string_view text, std::string_view suffix)
        {
            return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
        }
    }

    std::optional<ParsedUrl> parseUrl(std::string_view url)
    {
        auto const sep = url.find("://");
        if (sep == std::string_view::npos || sep == 0)
        {
            return std::nullopt;
        }
        ParsedUrl out;
        out.scheme = lower(url.substr(0, sep));
        auto rest = url.substr(sep + 3);
        auto const end = rest.find_first_of("/?#");
        auto authority = rest.substr(0, end);
        if (auto const at = authority.rfind('@'); at != std::string_view::npos)
        {
            authority = authority.substr(at + 1);
        }
        std::string_view portText;
        if (!authority.empty() && authority.front() == '[')
        {
            auto const close = authority.find(']');
            if (close == std::string_view::npos)
            {
                return std::nullopt;
            }
            out.host = lower(authority.substr(1, close - 1));
            if (close + 1 < authority.size() && authority[close + 1] == ':')
            {
                portText = authority.substr(close + 2);
            }
        }
        else
        {
            auto const colon = authority.find(':');
            out.host = lower(authority.substr(0, colon));
            if (colon != std::string_view::npos)
            {
                portText = authority.substr(colon + 1);
            }
        }
        while (!out.host.empty() && out.host.back() == '.')
        {
            out.host.pop_back(); // "localhost." is localhost
        }
        out.port = defaultPort(out.scheme);
        if (!portText.empty())
        {
            int port = 0;
            for (char c : portText)
            {
                if (!std::isdigit(static_cast<unsigned char>(c)))
                {
                    return std::nullopt;
                }
                port = port * 10 + (c - '0');
                if (port > 65535)
                {
                    return std::nullopt;
                }
            }
            out.port = port;
        }
        return out;
    }

    bool globMatch(std::string_view pattern, std::string_view text)
    {
        std::size_t p = 0;
        std::size_t t = 0;
        std::size_t star = std::string_view::npos;
        std::size_t mark = 0;
        auto same = [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); };
        while (t < text.size())
        {
            if (p < pattern.size() && pattern[p] != '*' && same(pattern[p], text[t]))
            {
                ++p;
                ++t;
            }
            else if (p < pattern.size() && pattern[p] == '*')
            {
                star = p++;
                mark = t;
            }
            else if (star != std::string_view::npos)
            {
                p = star + 1;
                t = ++mark;
            }
            else
            {
                return false;
            }
        }
        while (p < pattern.size() && pattern[p] == '*')
        {
            ++p;
        }
        return p == pattern.size();
    }

    bool cidrMatch(std::string_view cidr, std::string_view address)
    {
        auto const slash = cidr.find('/');
        auto const network = addressBytes(cidr.substr(0, slash));
        auto const host = addressBytes(address);
        if (!network || !host || network->size() != host->size())
        {
            return false;
        }
        int bits = static_cast<int>(network->size()) * 8;
        if (slash != std::string_view::npos)
        {
            bits = std::atoi(std::string(cidr.substr(slash + 1)).c_str());
            // An IPv4-mapped IPv6 prefix written for IPv6 (::ffff:0:0/96 + n) is not used here.
            bits = std::clamp(bits, 0, static_cast<int>(network->size()) * 8);
        }
        for (int i = 0; i < bits; ++i)
        {
            int const byte = i / 8;
            int const mask = 0x80 >> (i % 8);
            if (((*network)[static_cast<std::size_t>(byte)] & mask) != ((*host)[static_cast<std::size_t>(byte)] & mask))
            {
                return false;
            }
        }
        return true;
    }

    UrlPolicy::UrlPolicy(UrlPolicySettings settings)
        : _settings(std::move(settings))
    {
        _settings.registryHost = lower(_settings.registryHost);
        _settings.ownHost = lower(_settings.ownHost);
    }

    bool UrlPolicy::matches(std::vector<std::string> const& rules, std::string const& host) const
    {
        bool const ip = addressBytes(host).has_value();
        for (auto const& rule : rules)
        {
            bool const ruleIsAddress = rule.find('/') != std::string::npos || addressBytes(rule).has_value();
            if (ruleIsAddress ? (ip && cidrMatch(rule, host)) : globMatch(rule, host))
            {
                return true;
            }
        }
        return false;
    }

    UrlVerdict UrlPolicy::check(std::string_view url, bool topLevel) const
    {
        auto const colon = url.find(':');
        if (colon == std::string_view::npos || colon == 0)
        {
            return {false, "scheme"};
        }
        std::string const scheme = lower(url.substr(0, colon));
        if (scheme == "about")
        {
            auto const what = lower(url.substr(colon + 1));
            return what == "blank" || what == "srcdoc" ? UrlVerdict{} : UrlVerdict{false, "scheme"};
        }
        if (scheme == "data" || scheme == "javascript")
        {
            return topLevel ? UrlVerdict{false, "scheme"} : UrlVerdict{};
        }
        if (scheme == "blob")
        {
            return {}; // created by the page itself, same origin
        }
        if (scheme != "http" && scheme != "https" && scheme != "ws" && scheme != "wss")
        {
            return {false, "scheme"}; // file:, chrome:, devtools:, view-source:, custom schemes
        }
        auto const parsed = parseUrl(url);
        if (!parsed || parsed->host.empty())
        {
            return {false, "scheme"};
        }
        std::string const& host = parsed->host;
        if (host == "templates.local")
        {
            return {};
        }
        if (addressBytes(host))
        {
            if (cidrMatch("169.254.0.0/16", host) || cidrMatch("fe80::/10", host))
            {
                return {false, "link_local"};
            }
            if (cidrMatch("127.0.0.0/8", host) || cidrMatch("::1", host) || cidrMatch("0.0.0.0", host) || cidrMatch("::", host))
            {
                return {false, "loopback"};
            }
        }
        else
        {
            if (host == "localhost" || endsWith(host, ".localhost"))
            {
                return {false, "loopback"};
            }
            if (host == "metadata.google.internal" || host == "metadata")
            {
                return {false, "link_local"};
            }
            if (host == "kubernetes" || globMatch("kubernetes.default*", host))
            {
                return {false, "kubernetes"};
            }
        }
        if (!_settings.registryHost.empty() && host == _settings.registryHost)
        {
            return {false, "registry"};
        }
        if (!_settings.ownHost.empty() && host == _settings.ownHost &&
            std::find(_settings.ownPorts.begin(), _settings.ownPorts.end(), parsed->port) != _settings.ownPorts.end())
        {
            return {false, "own_port"};
        }
        if (matches(_settings.deny, host))
        {
            return {false, "denied"};
        }
        if (!_settings.allow.empty() && !matches(_settings.allow, host))
        {
            return {false, "not_allowed"};
        }
        return {};
    }
}
