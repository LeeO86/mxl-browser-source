// SPDX-License-Identifier: MIT
// The siblings' small HTTP/1.1 server (SPEC §3), one thread per connection, extended for
// this function: WebSocket endpoints with one session per connection (text and binary in
// both directions), and raw takeover of a connection (the DevTools WebSocket tunnel).
// Based on mxl-color-corrector's src/http.cpp (MIT, same author).
#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mbs::ops
{
    struct HttpRequest
    {
        std::string method;
        std::string path;   // without the query
        std::string query;  // after '?', undecoded
        std::vector<std::pair<std::string, std::string>> headers;
        std::string body;
        std::string peer; // client IP address
        std::string raw;  // request line and headers as received (for a tunnel)

        /// Case-insensitive header lookup; empty when missing.
        [[nodiscard]] std::string header(std::string_view name) const;
    };

    struct HttpResponse
    {
        int status = 200;
        std::string contentType = "application/json; charset=utf-8";
        std::string body;
        std::vector<std::pair<std::string, std::string>> headers;
    };

    /// One WebSocket connection. Sends are thread-safe; a failed send closes the socket.
    class WebSocket
    {
    public:
        WebSocket(int fd, std::string peer);
        ~WebSocket();
        WebSocket(WebSocket const&) = delete;
        WebSocket& operator=(WebSocket const&) = delete;

        bool sendText(std::string_view text);
        bool sendBinary(std::string_view data);
        /// Sends a close frame and shuts the socket down; the reader thread ends.
        void close(int code = 1000);
        [[nodiscard]] bool open() const { return _open.load(); }
        [[nodiscard]] std::string const& peer() const { return _peer; }

    private:
        friend class HttpServer;
        bool send(unsigned opcode, std::string_view payload);

        int _fd;
        std::string _peer;
        std::mutex _sendMutex;
        std::atomic<bool> _open{true};
    };

    struct WebSocketHandlers
    {
        std::function<void(std::shared_ptr<WebSocket> const&)> open;
        std::function<void(std::shared_ptr<WebSocket> const&, std::string const& message, bool binary)> message;
        std::function<void(std::shared_ptr<WebSocket> const&)> close;
        std::size_t maxMessage = 64 * 1024; // larger messages close the connection (1009)
    };

    class HttpServer
    {
    public:
        using Handler = std::function<void(HttpRequest const&, HttpResponse&)>;
        /// For a request with `Upgrade: websocket`: the handlers of that endpoint, or nullopt
        /// with `reject` filled (404 for an unknown path, 401 without the token, …).
        using UpgradeHandler = std::function<std::optional<WebSocketHandlers>(HttpRequest const&, HttpResponse& reject)>;
        /// Asked first for every request; returns true when it took the connection over (it then
        /// owns and closes `fd`).
        using RawHandler = std::function<bool(int fd, HttpRequest const&)>;

        HttpServer(Handler handler, UpgradeHandler upgrade = {}, RawHandler raw = {});
        ~HttpServer();
        HttpServer(HttpServer const&) = delete;
        HttpServer& operator=(HttpServer const&) = delete;

        /// Binds all interfaces on `port` (0 = any free port). False with `error` when it cannot.
        bool start(int port, std::string& error);
        void stop();
        [[nodiscard]] int port() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
    };

    /// Blocking send of all bytes (MSG_NOSIGNAL); false when the peer is gone.
    bool writeAll(int fd, std::string_view data);
    /// Percent-decodes a query value ('+' is a space).
    std::string urlDecode(std::string_view text);
    /// The value of `key` in a query string, decoded; empty when missing.
    std::string queryValue(std::string_view query, std::string_view key);
}
