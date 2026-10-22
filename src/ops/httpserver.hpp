// SPDX-License-Identifier: MIT
// The siblings' small HTTP/1.1 server (SPEC §3), one thread per connection, extended for
// this function: WebSocket endpoints with one session per connection (text and binary in
// both directions), and raw takeover of a connection (the DevTools WebSocket tunnel).
// Based on mxl-color-corrector's src/http.cpp (MIT, same author).
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
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

    /// One WebSocket connection. Sends are thread-safe, never block the caller and never write
    /// half a frame between others; a failed send closes the socket.
    /// Text and control frames are written at once when the socket has room, else they wait in
    /// order in an outbox that the connection's thread drains. The socket is closed when the
    /// outbox holds more than kOutboxBytes or its oldest frame is older than kStuckTimeout.
    /// Binary frames (previews) are dropped instead of queued when the socket is full (the next
    /// one is newer); one that has started is finished first. A client that takes no binary
    /// frame for kStuckTimeout is closed too.
    class WebSocket
    {
    public:
        WebSocket(int fd, std::string peer);
        ~WebSocket();
        WebSocket(WebSocket const&) = delete;
        WebSocket& operator=(WebSocket const&) = delete;

        static constexpr std::chrono::milliseconds kStuckTimeout{5000};
        static constexpr std::size_t kOutboxBytes = 4 * 1024 * 1024;

        enum class Sent
        {
            Yes,
            Dropped, // the client's socket is full
            Closed,
        };

        /// Queues the message; false when the socket is (or got) closed.
        bool sendText(std::string_view text);
        Sent sendBinary(std::string_view data);
        /// Sends a close frame and shuts the socket down; the reader thread ends.
        void close(int code = 1000);
        [[nodiscard]] bool open() const { return _open.load(); }
        [[nodiscard]] std::string const& peer() const { return _peer; }

    private:
        friend class HttpServer;
        using Clock = std::chrono::steady_clock;
        struct Frame
        {
            std::string bytes;
            std::size_t sent = 0;
            Clock::time_point queued;
        };

        bool send(unsigned opcode, std::string_view payload);
        /// For the connection's thread: writes what the outbox can; true while it still holds data.
        bool flush();
        // With _sendMutex held:
        void fail();
        void flushLocked(Clock::time_point now);
        Sent dropped(Clock::time_point now); // counts towards kStuckTimeout

        int _fd;
        std::deque<Frame> _outbox;
        std::size_t _outboxBytes = 0;
        bool _dropping = false;
        Clock::time_point _droppingSince;
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
