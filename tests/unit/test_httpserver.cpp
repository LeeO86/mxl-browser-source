// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>

#include "ops/httpserver.hpp"

using namespace mbs::ops;

namespace
{
    int connectTo(int port)
    {
        int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<std::uint16_t>(port));
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        return fd;
    }

    // Reads until `done(data)` or 2 s pass.
    template <typename Done>
    std::string readUntil(int fd, Done done)
    {
        std::string data;
        for (int i = 0; i < 40 && !done(data); ++i)
        {
            pollfd pfd{fd, POLLIN, 0};
            if (::poll(&pfd, 1, 50) <= 0)
            {
                continue;
            }
            char buf[4096];
            auto const n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0)
            {
                break;
            }
            data.append(buf, static_cast<std::size_t>(n));
        }
        return data;
    }

    // A masked client text frame.
    std::string clientFrame(std::string const& text)
    {
        std::string frame;
        frame.push_back(static_cast<char>(0x81));
        frame.push_back(static_cast<char>(0x80 | text.size()));
        unsigned char const mask[4] = {1, 2, 3, 4};
        frame.append(reinterpret_cast<char const*>(mask), 4);
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            frame.push_back(static_cast<char>(text[i] ^ mask[i % 4]));
        }
        return frame;
    }
}

TEST_CASE("the server answers requests with query, body and peer address")
{
    HttpServer server([](HttpRequest const& req, HttpResponse& res) {
        res.body = req.method + " " + req.path + " " + queryValue(req.query, "name") + " " + req.body + " " + req.peer;
    });
    std::string error;
    REQUIRE(server.start(0, error));
    int const fd = connectTo(server.port());
    REQUIRE(writeAll(fd, "POST /api/v1/x?name=a%20b&y=1 HTTP/1.1\r\nHost: t\r\nContent-Length: 4\r\n\r\nbody"));
    auto const reply = readUntil(fd, [](std::string const& d) { return d.find("127.0.0.1") != std::string::npos; });
    ::close(fd);
    CHECK(reply.find("HTTP/1.1 200 OK") == 0);
    CHECK(reply.find("POST /api/v1/x a b body 127.0.0.1") != std::string::npos);
    server.stop();
}

TEST_CASE("WebSocket: handshake, a text message, the server's answer, rejection")
{
    std::string received;
    HttpServer server([](HttpRequest const&, HttpResponse& res) { res.status = 404; },
        [&](HttpRequest const& req, HttpResponse& reject) -> std::optional<WebSocketHandlers> {
            if (req.path != "/ws")
            {
                reject.status = 404;
                return std::nullopt;
            }
            WebSocketHandlers h;
            h.message = [&](std::shared_ptr<WebSocket> const& socket, std::string const& message, bool binary) {
                received = message;
                socket->sendText(binary ? "binary" : "echo:" + message);
            };
            return h;
        });
    std::string error;
    REQUIRE(server.start(0, error));
    int const fd = connectTo(server.port());
    // The key and accept value of RFC 6455 §1.3.
    REQUIRE(writeAll(fd, "GET /ws HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                         "Sec-WebSocket-Version: 13\r\n\r\n" +
                         clientFrame("hello")));
    auto const reply = readUntil(fd, [](std::string const& d) { return d.find("echo:hello") != std::string::npos; });
    CHECK(reply.find("101 Switching Protocols") != std::string::npos);
    CHECK(reply.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);
    CHECK(reply.find("echo:hello") != std::string::npos);
    CHECK(received == "hello");
    ::close(fd);

    int const other = connectTo(server.port());
    REQUIRE(writeAll(other, "GET /nope HTTP/1.1\r\nUpgrade: websocket\r\nSec-WebSocket-Key: x\r\n\r\n"));
    auto const rejected = readUntil(other, [](std::string const& d) { return d.find("\r\n\r\n") != std::string::npos; });
    CHECK(rejected.find("HTTP/1.1 404") == 0);
    ::close(other);
    server.stop();
}

TEST_CASE("a busy port fails to start")
{
    HttpServer a([](HttpRequest const&, HttpResponse&) {});
    HttpServer b([](HttpRequest const&, HttpResponse&) {});
    std::string error;
    REQUIRE(a.start(0, error));
    CHECK_FALSE(b.start(a.port(), error));
    CHECK(error.find("bind failed") != std::string::npos);
}
