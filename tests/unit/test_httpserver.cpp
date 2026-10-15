// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#include "ops/httpserver.hpp"
#include "util/net.hpp"

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

TEST_CASE("the start-up port check sees a busy port and a free one")
{
    HttpServer a([](HttpRequest const&, HttpResponse&) {});
    std::string error;
    REQUIRE(a.start(0, error));
    int const port = a.port();
    CHECK_FALSE(mbs::util::portFree(port, error));
    CHECK(error.find("bind failed on port " + std::to_string(port)) != std::string::npos);
    a.stop();
    CHECK(mbs::util::portFree(port, error));
}

TEST_CASE("a large-body route streams its body to a file; other bodies stay limited to 4 MiB")
{
    auto const dir = std::filesystem::temp_directory_path() / "mbs-upload-test";
    std::filesystem::create_directories(dir);
    std::string seenFile;
    std::uintmax_t seenSize = 0;
    HttpServer server([&](HttpRequest const& req, HttpResponse& res) {
        seenFile = req.bodyFile;
        seenSize = req.bodyFile.empty() ? req.body.size() : std::filesystem::file_size(req.bodyFile);
        res.body = "ok";
    });
    server.streamLargeBodies({[](HttpRequest const& req) { return req.path == "/upload"; }, 8u << 20, dir.string()});
    std::string error;
    REQUIRE(server.start(0, error));
    // `sendBody` false: only the head. The server refuses a too large body from Content-Length
    // alone; sending the body anyway raced with its close (a reset could arrive before the 413).
    auto const send = [&](std::string const& path, std::size_t size, bool sendBody = true) {
        int const fd = connectTo(server.port());
        writeAll(fd, "POST " + path + " HTTP/1.1\r\nHost: t\r\nContent-Length: " + std::to_string(size) + "\r\n\r\n");
        if (sendBody)
        {
            writeAll(fd, std::string(size, 'z'));
        }
        auto const reply = readUntil(fd, [](std::string const& d) { return d.find("\r\n\r\n") != std::string::npos; });
        ::close(fd);
        return reply;
    };
    CHECK(send("/upload", 6u << 20).find("HTTP/1.1 200") == 0);
    CHECK(seenSize == (6u << 20));
    CHECK_FALSE(seenFile.empty());
    CHECK_FALSE(std::filesystem::exists(seenFile)); // removed after the answer
    CHECK(send("/upload", 9u << 20, false).find("HTTP/1.1 413") == 0);
    CHECK(send("/other", 6u << 20, false).find("HTTP/1.1 413") == 0);
    server.stop();
    std::filesystem::remove_all(dir);
}

TEST_CASE("an upload is admitted from its head, at most two at once, within a deadline")
{
    auto const dir = std::filesystem::temp_directory_path() / "mbs-upload-limits-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    auto const files = [&] { return std::distance(std::filesystem::directory_iterator(dir), std::filesystem::directory_iterator{}); };
    HttpServer server([&](HttpRequest const&, HttpResponse& res) { res.body = "ok"; });
    LargeBodies large{[](HttpRequest const& req) { return req.path == "/upload"; }, 8u << 20, dir.string()};
    large.admit = [](HttpRequest const& req, HttpResponse& reject) {
        if (req.header("Authorization") == "Bearer s")
        {
            return true;
        }
        reject.status = 401;
        reject.body = R"({"error":"token required"})";
        return false;
    };
    large.deadlineMs = 1500;
    server.streamLargeBodies(large);
    std::string error;
    REQUIRE(server.start(0, error));
    auto const head = [&](bool token, std::size_t size) {
        int const fd = connectTo(server.port());
        writeAll(fd, "POST /upload HTTP/1.1\r\nHost: t\r\n" + std::string(token ? "Authorization: Bearer s\r\n" : "") +
                         "Content-Length: " + std::to_string(size) + "\r\n\r\n");
        return fd;
    };
    auto const answer = [](int fd) { return readUntil(fd, [](std::string const& d) { return d.find("\r\n\r\n") != std::string::npos; }); };

    // Without the token: 401 from the head alone, nothing stored. (Only the head is sent: body
    // bytes the server never reads would make its close a reset that can overtake the answer.)
    int const anonymous = head(false, 6u << 20);
    CHECK(answer(anonymous).find("HTTP/1.1 401") == 0);
    ::close(anonymous);
    CHECK(files() == 0);

    // Two slow uploads hold both slots; a third is refused with 429 without being stored.
    int const first = head(true, 1u << 20);
    int const second = head(true, 1u << 20);
    writeAll(first, std::string(1024, 'a'));
    writeAll(second, std::string(1024, 'b'));
    for (int i = 0; i < 100 && files() < 2; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(files() == 2);
    int const third = head(true, 1024);
    CHECK(answer(third).find("HTTP/1.1 429") == 0);
    ::close(third);

    // Trickled past the deadline: 408 and the partial files are removed; the slots are free again.
    CHECK(answer(first).find("HTTP/1.1 408") == 0);
    CHECK(answer(second).find("HTTP/1.1 408") == 0);
    ::close(first);
    ::close(second);
    for (int i = 0; i < 100 && files() > 0; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(files() == 0);
    int const again = head(true, 1024);
    writeAll(again, std::string(1024, 'c'));
    CHECK(answer(again).find("HTTP/1.1 200") == 0);
    ::close(again);
    server.stop();
    std::filesystem::remove_all(dir);
}
