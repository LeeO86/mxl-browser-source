// SPDX-License-Identifier: MIT
// Small network helpers: the announced address, and whether this process owns a listening
// TCP socket (nmos-cpp swallows listener errors, SPEC §8.3).
#pragma once

#include <string>

namespace mbs::util
{
    /// First non-loopback IPv4 address of the host, empty when there is none.
    std::string primaryIpv4();

    /// True when a socket of this process listens on TCP `port` (IPv4 or IPv6): the listening
    /// inode from /proc/net/tcp{,6} is one of /proc/self/fd.
    bool ownsListener(int port);

    /// HTTP/1.0 GET of `path` on an IPv4 literal; the status code, or -1 when the request did
    /// not complete within `timeoutMs`. `body` receives the response body when given.
    int httpGet(std::string const& host, int port, std::string const& path, int timeoutMs, std::string* body = nullptr);

    /// A TCP connection to an IPv4 literal, or -1.
    int connectTcp(std::string const& host, int port, int timeoutMs);
}
