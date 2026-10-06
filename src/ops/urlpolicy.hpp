// SPDX-License-Identifier: MIT
// URL policy (SPEC §14.3): which pages and sub-resources the browser may load. Checked on
// navigation, on redirects and for every sub-resource by host. DNS-based bypasses are
// possible; the NetworkPolicy is the real fence.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mbs::ops
{
    struct ParsedUrl
    {
        std::string scheme; // lowercase
        std::string host;   // lowercase, IPv6 without brackets
        int port = 0;       // explicit or the scheme's default (0 when unknown)
    };

    /// scheme://[user@]host[:port]... ; nullopt for URLs without an authority (data:, about:).
    std::optional<ParsedUrl> parseUrl(std::string_view url);

    struct UrlPolicySettings
    {
        std::vector<std::string> allow; // host globs and CIDRs; when set, only these (and templates.local)
        std::vector<std::string> deny;  // host globs and CIDRs, in addition to the built-in list
        std::string registryHost;       // NMOS registry address
        std::string ownHost;            // NMOS_HOST_ADDRESS
        std::vector<int> ownPorts;      // WEB_PORT, NMOS_PORT, NMOS_PORT+1, the DevTools port
    };

    struct UrlVerdict
    {
        bool allowed = true;
        std::string reason; // navigation_blocked_total{reason}: scheme, link_local, loopback, kubernetes, registry, own_port, denied, not_allowed
    };

    class UrlPolicy
    {
    public:
        UrlPolicy() = default;
        explicit UrlPolicy(UrlPolicySettings settings);

        /// `topLevel`: a navigation of a frame (data: and javascript: are refused there, allowed
        /// for sub-resources and inline use).
        [[nodiscard]] UrlVerdict check(std::string_view url, bool topLevel) const;

    private:
        [[nodiscard]] bool matches(std::vector<std::string> const& rules, std::string const& host) const;
        UrlPolicySettings _settings;
    };

    /// Simple glob: '*' matches any run of characters (case-insensitive).
    bool globMatch(std::string_view pattern, std::string_view text);
    /// `cidr` as a.b.c.d/n or an IPv6 prefix; a bare address is a /32 or /128.
    bool cidrMatch(std::string_view cidr, std::string_view address);
}
