// SPDX-License-Identifier: MIT
// The siblings' small HTTP/1.1 server (SPEC §3), one thread per connection, extended for
// this function: WebSocket endpoints with one session per connection (text and binary in
// both directions), and raw takeover of a connection (the DevTools WebSocket tunnel).
// Based on mxl-color-corrector's src/http.cpp (MIT, same author).
#pragma once

#include <atomic>
#include <chrono>
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
        std::string bodyFile; // set instead of body for a LargeBodies route (removed after the answer)
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
    /// A client that stops reading never blocks a sender for long: text and control frames wait
    /// at most kStuckTimeout for room (in order, never dropped), then the socket is closed;
    /// binary frames (previews) never wait: a frame that does not fit is dropped (the next one
    /// is newer), and a client that takes none for kStuckTimeout is closed.
    class WebSocket
    {
    public:
        WebSocket(int fd, std::string peer);
        ~WebSocket();
        WebSocket(WebSocket const&) = delete;
        WebSocket& operator=(WebSocket const&) = delete;

        static constexpr std::chrono::milliseconds kStuckTimeout{5000};

        enum class Sent
        {
            Yes,
            Dropped, // the client's socket is full (or a text send is in progress)
            Closed,
        };

        bool sendText(std::string_view text);
        /// Never blocks; see above.
        Sent sendBinary(std::string_view data);
        /// Sends a close frame and shuts the socket down; the reader thread ends.
        void close(int code = 1000);
        [[nodiscard]] bool open() const { return _open.load(); }
        [[nodiscard]] std::string const& peer() const { return _peer; }

    private:
        friend class HttpServer;
        bool send(unsigned opcode, std::string_view payload);
        void fail(); // with _sendMutex held
        Sent dropped(); // with _sendMutex held: counts towards kStuckTimeout

        int _fd;
        std::string _pending; // the unsent rest of a binary frame; goes out before anything else
        bool _dropping = false;
        std::chrono::steady_clock::time_point _droppingSince;
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

    /// Requests whose body the server writes to a file instead of memory (uploads). Every other
    /// body stays limited to 4 MiB.
    struct LargeBodies
    {
        std::function<bool(HttpRequest const&)> match;
        std::size_t maxBytes = 0;
        std::string dir; // must exist
        /// Asked with the head, before a byte of the body is read or stored (the token check):
        /// false with `reject` filled answers that and closes the connection, the body unread.
        std::function<bool(HttpRequest const&, HttpResponse& reject)> admit = {};
        std::size_t maxConcurrent = 2; // bodies streamed (or waiting for their answer) at once; more: 429
        int deadlineMs = 600'000;      // for the whole body; a slower client gets 408 and its file is removed
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
        /// Before start().
        void streamLargeBodies(LargeBodies large);
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
