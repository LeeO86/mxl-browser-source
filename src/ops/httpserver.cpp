// SPDX-License-Identifier: MIT
#include "ops/httpserver.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <thread>

#include "util/uuid.hpp"

namespace mbs::ops
{
    namespace
    {
        std::string base64(std::uint8_t const* data, std::size_t len)
        {
            static char const* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string out;
            out.reserve((len + 2) / 3 * 4);
            for (std::size_t i = 0; i < len; i += 3)
            {
                unsigned n = static_cast<unsigned>(data[i]) << 16;
                if (i + 1 < len) n |= static_cast<unsigned>(data[i + 1]) << 8;
                if (i + 2 < len) n |= data[i + 2];
                out.push_back(table[(n >> 18) & 63]);
                out.push_back(table[(n >> 12) & 63]);
                out.push_back(i + 1 < len ? table[(n >> 6) & 63] : '=');
                out.push_back(i + 2 < len ? table[n & 63] : '=');
            }
            return out;
        }

        std::string frameHeader(unsigned opcode, std::size_t size)
        {
            std::string header;
            header.push_back(static_cast<char>(0x80 | opcode));
            if (size < 126)
            {
                header.push_back(static_cast<char>(size));
            }
            else if (size <= 65535)
            {
                header.push_back(static_cast<char>(126));
                header.push_back(static_cast<char>((size >> 8) & 0xff));
                header.push_back(static_cast<char>(size & 0xff));
            }
            else
            {
                header.push_back(static_cast<char>(127));
                auto const n = static_cast<std::uint64_t>(size);
                for (int i = 7; i >= 0; --i)
                {
                    header.push_back(static_cast<char>((n >> (i * 8)) & 0xff));
                }
            }
            return header;
        }

        std::string statusText(int status)
        {
            switch (status)
            {
            case 101: return "Switching Protocols";
            case 200: return "OK";
            case 201: return "Created";
            case 202: return "Accepted";
            case 204: return "No Content";
            case 301: return "Moved Permanently";
            case 302: return "Found";
            case 304: return "Not Modified";
            case 400: return "Bad Request";
            case 401: return "Unauthorized";
            case 403: return "Forbidden";
            case 404: return "Not Found";
            case 405: return "Method Not Allowed";
            case 409: return "Conflict";
            case 413: return "Payload Too Large";
            case 429: return "Too Many Requests";
            case 500: return "Internal Server Error";
            case 502: return "Bad Gateway";
            case 503: return "Service Unavailable";
            default: return "Error";
            }
        }

        bool iequals(std::string_view a, std::string_view b)
        {
            if (a.size() != b.size())
            {
                return false;
            }
            for (std::size_t i = 0; i < a.size(); ++i)
            {
                if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                {
                    return false;
                }
            }
            return true;
        }

        std::string responseText(HttpResponse const& res)
        {
            std::string out = "HTTP/1.1 " + std::to_string(res.status) + " " + statusText(res.status) + "\r\n";
            out += "Content-Type: " + res.contentType + "\r\n";
            out += "Content-Length: " + std::to_string(res.body.size()) + "\r\n";
            out += "Connection: close\r\n";
            out += "Cache-Control: no-store\r\n";
            for (auto const& [key, value] : res.headers)
            {
                out += key + ": " + value + "\r\n";
            }
            out += "\r\n";
            out += res.body;
            return out;
        }
    }

    bool writeAll(int fd, std::string_view data)
    {
        std::size_t off = 0;
        while (off < data.size())
        {
            ssize_t const n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
            if (n <= 0)
            {
                return false;
            }
            off += static_cast<std::size_t>(n);
        }
        return true;
    }

    std::string urlDecode(std::string_view text)
    {
        std::string out;
        out.reserve(text.size());
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            char const c = text[i];
            if (c == '+')
            {
                out.push_back(' ');
            }
            else if (c == '%' && i + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
                     std::isxdigit(static_cast<unsigned char>(text[i + 2])))
            {
                out.push_back(static_cast<char>(std::stoi(std::string(text.substr(i + 1, 2)), nullptr, 16)));
                i += 2;
            }
            else
            {
                out.push_back(c);
            }
        }
        return out;
    }

    std::string queryValue(std::string_view query, std::string_view key)
    {
        std::size_t pos = 0;
        while (pos <= query.size())
        {
            auto const end = std::min(query.find('&', pos), query.size());
            auto const pair = query.substr(pos, end - pos);
            auto const eq = pair.find('=');
            if (pair.substr(0, eq) == key)
            {
                return eq == std::string_view::npos ? std::string{} : urlDecode(pair.substr(eq + 1));
            }
            pos = end + 1;
        }
        return {};
    }

    std::string HttpRequest::header(std::string_view name) const
    {
        for (auto const& [key, value] : headers)
        {
            if (iequals(key, name))
            {
                return value;
            }
        }
        return {};
    }

    WebSocket::WebSocket(int fd, std::string peer)
        : _fd(fd)
        , _peer(std::move(peer))
    {
    }

    WebSocket::~WebSocket() = default;

    bool WebSocket::send(unsigned opcode, std::string_view payload)
    {
        std::lock_guard lock{_sendMutex};
        if (!_open.load())
        {
            return false;
        }
        if (!writeAll(_fd, frameHeader(opcode, payload.size())) || !writeAll(_fd, payload))
        {
            _open.store(false);
            ::shutdown(_fd, SHUT_RDWR);
            return false;
        }
        return true;
    }

    bool WebSocket::sendText(std::string_view text)
    {
        return send(0x1, text);
    }

    bool WebSocket::sendBinary(std::string_view data)
    {
        return send(0x2, data);
    }

    void WebSocket::close(int code)
    {
        std::lock_guard lock{_sendMutex};
        if (!_open.exchange(false))
        {
            return;
        }
        std::string payload;
        payload.push_back(static_cast<char>((code >> 8) & 0xff));
        payload.push_back(static_cast<char>(code & 0xff));
        writeAll(_fd, frameHeader(0x8, payload.size()));
        writeAll(_fd, payload);
        ::shutdown(_fd, SHUT_RDWR);
    }

    struct HttpServer::Impl
    {
        Handler handler;
        UpgradeHandler upgrade;
        RawHandler raw;
        int listenFd = -1;
        int boundPort = 0;
        std::atomic<bool> stop{false};
        std::thread acceptThread;

        struct Job
        {
            std::thread thread;
            std::shared_ptr<std::atomic<bool>> done;
        };
        std::mutex jobsMutex;
        std::vector<Job> jobs;
        std::mutex socketsMutex;
        std::vector<std::weak_ptr<WebSocket>> sockets;

        void reap()
        {
            std::lock_guard lock{jobsMutex};
            std::vector<Job> live;
            for (auto& job : jobs)
            {
                if (job.done->load())
                {
                    job.thread.join();
                }
                else
                {
                    live.push_back(std::move(job));
                }
            }
            jobs.swap(live);
        }

        bool readSome(int fd, std::string& data, int timeoutMs) const
        {
            pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLIN;
            if (::poll(&pfd, 1, timeoutMs) <= 0)
            {
                return false;
            }
            char buf[16384];
            ssize_t const n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0)
            {
                return false;
            }
            data.append(buf, static_cast<std::size_t>(n));
            return true;
        }

        // Reads the request; `rest` keeps bytes after the body (a WebSocket client may send its
        // first frame right after the handshake).
        bool readHttp(int fd, HttpRequest& req, std::string& rest)
        {
            std::string data;
            int idle = 0;
            while (data.find("\r\n\r\n") == std::string::npos)
            {
                if (stop.load() || data.size() > 64 * 1024)
                {
                    return false;
                }
                if (!readSome(fd, data, 500))
                {
                    if (++idle > 20) // 10 s for the request head
                    {
                        return false;
                    }
                    continue;
                }
            }
            auto const headerEnd = data.find("\r\n\r\n");
            req.raw = data.substr(0, headerEnd + 4);
            std::string const head = data.substr(0, headerEnd);
            rest = data.substr(headerEnd + 4);
            auto const lineEnd = head.find("\r\n");
            std::string const start = lineEnd == std::string::npos ? head : head.substr(0, lineEnd);
            auto const s1 = start.find(' ');
            auto const s2 = start.find(' ', s1 == std::string::npos ? 0 : s1 + 1);
            if (s1 == std::string::npos || s2 == std::string::npos)
            {
                return false;
            }
            req.method = start.substr(0, s1);
            auto const target = start.substr(s1 + 1, s2 - s1 - 1);
            auto const q = target.find('?');
            req.path = q == std::string::npos ? target : target.substr(0, q);
            req.query = q == std::string::npos ? std::string{} : target.substr(q + 1);
            std::size_t pos = lineEnd == std::string::npos ? head.size() : lineEnd + 2;
            while (pos < head.size())
            {
                auto const next = head.find("\r\n", pos);
                auto const line = head.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
                auto const colon = line.find(':');
                if (colon != std::string::npos)
                {
                    auto value = line.substr(colon + 1);
                    while (!value.empty() && value.front() == ' ')
                    {
                        value.erase(value.begin());
                    }
                    req.headers.emplace_back(line.substr(0, colon), value);
                }
                if (next == std::string::npos)
                {
                    break;
                }
                pos = next + 2;
            }
            std::size_t length = 0;
            if (auto const declared = req.header("Content-Length"); !declared.empty())
            {
                length = static_cast<std::size_t>(std::strtoull(declared.c_str(), nullptr, 10));
            }
            if (length > 4 * 1024 * 1024)
            {
                writeAll(fd, responseText(HttpResponse{413, "application/json", R"({"error":"body too large"})", {}}));
                return false;
            }
            while (rest.size() < length)
            {
                if (!readSome(fd, rest, 2000))
                {
                    return false;
                }
            }
            req.body = rest.substr(0, length);
            rest.erase(0, length);
            return true;
        }

        void serveHttp(int fd, HttpRequest const& req)
        {
            HttpResponse res;
            try
            {
                handler(req, res);
            }
            catch (std::exception const& ex)
            {
                res = HttpResponse{};
                res.status = 500;
                res.body = std::string(R"({"error":"internal"})");
            }
            writeAll(fd, responseText(res));
        }

        void serveWebSocket(int fd, HttpRequest const& req, WebSocketHandlers const& handlers, std::string buffer)
        {
            auto const key = req.header("Sec-WebSocket-Key");
            if (key.empty())
            {
                writeAll(fd, responseText(HttpResponse{400, "application/json", R"({"error":"missing websocket key"})", {}}));
                return;
            }
            auto const digest = util::sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
            std::string const accept = base64(digest.data(), digest.size());
            if (!writeAll(fd, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n"))
            {
                return;
            }
            auto socket = std::make_shared<WebSocket>(fd, req.peer);
            {
                std::lock_guard lock{socketsMutex};
                std::erase_if(sockets, [](auto const& w) { return w.expired(); });
                sockets.push_back(socket);
            }
            if (handlers.open)
            {
                handlers.open(socket);
            }
            std::string message;
            unsigned messageOpcode = 0;
            while (!stop.load() && socket->open())
            {
                bool progressed = false;
                while (buffer.size() >= 2)
                {
                    auto const* bytes = reinterpret_cast<unsigned char const*>(buffer.data());
                    bool const fin = (bytes[0] & 0x80) != 0;
                    unsigned const opcode = bytes[0] & 0x0f;
                    bool const masked = (bytes[1] & 0x80) != 0;
                    std::uint64_t len = bytes[1] & 0x7f;
                    std::size_t header = 2;
                    if (len == 126)
                    {
                        if (buffer.size() < 4)
                        {
                            break;
                        }
                        len = (std::uint64_t{bytes[2]} << 8) | bytes[3];
                        header = 4;
                    }
                    else if (len == 127)
                    {
                        if (buffer.size() < 10)
                        {
                            break;
                        }
                        len = 0;
                        for (int i = 0; i < 8; ++i)
                        {
                            len = (len << 8) | bytes[2 + i];
                        }
                        header = 10;
                    }
                    if (len > handlers.maxMessage || message.size() + len > handlers.maxMessage)
                    {
                        socket->close(1009);
                        break;
                    }
                    std::size_t const maskLen = masked ? 4 : 0;
                    if (buffer.size() < header + maskLen + len)
                    {
                        break;
                    }
                    std::string payload(static_cast<std::size_t>(len), '\0');
                    for (std::uint64_t i = 0; i < len; ++i)
                    {
                        unsigned char c = bytes[header + maskLen + i];
                        if (masked)
                        {
                            c ^= bytes[header + (i % 4)];
                        }
                        payload[static_cast<std::size_t>(i)] = static_cast<char>(c);
                    }
                    buffer.erase(0, header + maskLen + static_cast<std::size_t>(len));
                    progressed = true;
                    if (opcode == 0x8)
                    {
                        socket->close(1000);
                        break;
                    }
                    if (opcode == 0x9)
                    {
                        socket->send(0xA, payload);
                        continue;
                    }
                    if (opcode == 0xA)
                    {
                        continue;
                    }
                    if (opcode == 0x1 || opcode == 0x2)
                    {
                        messageOpcode = opcode;
                        message = std::move(payload);
                    }
                    else if (opcode == 0x0)
                    {
                        message += payload; // continuation
                    }
                    if (fin && messageOpcode != 0)
                    {
                        if (handlers.message)
                        {
                            handlers.message(socket, message, messageOpcode == 0x2);
                        }
                        message.clear();
                        messageOpcode = 0;
                    }
                }
                if (!socket->open())
                {
                    break;
                }
                if (!progressed || buffer.size() < 2)
                {
                    pollfd pfd{};
                    pfd.fd = fd;
                    pfd.events = POLLIN;
                    int const rc = ::poll(&pfd, 1, 200);
                    if (rc < 0)
                    {
                        break;
                    }
                    if (rc == 0)
                    {
                        continue;
                    }
                    char buf[16384];
                    ssize_t const n = ::recv(fd, buf, sizeof(buf), 0);
                    if (n <= 0)
                    {
                        break;
                    }
                    buffer.append(buf, static_cast<std::size_t>(n));
                }
            }
            socket->close(1001);
            if (handlers.close)
            {
                handlers.close(socket);
            }
        }

        void connection(int fd, std::string peer)
        {
            HttpRequest req;
            req.peer = std::move(peer);
            std::string rest;
            if (!readHttp(fd, req, rest))
            {
                ::close(fd);
                return;
            }
            if (raw && raw(fd, req))
            {
                return; // the raw handler owns the socket now
            }
            if (iequals(req.header("Upgrade"), "websocket") && req.method == "GET")
            {
                HttpResponse reject;
                reject.status = 404;
                reject.body = R"({"error":"not found"})";
                std::optional<WebSocketHandlers> handlers;
                if (upgrade)
                {
                    handlers = upgrade(req, reject);
                }
                if (handlers)
                {
                    serveWebSocket(fd, req, *handlers, std::move(rest));
                }
                else
                {
                    writeAll(fd, responseText(reject));
                }
                ::close(fd);
                return;
            }
            serveHttp(fd, req);
            ::close(fd);
        }

        void acceptLoop()
        {
            while (!stop.load())
            {
                reap();
                pollfd pfd{};
                pfd.fd = listenFd;
                pfd.events = POLLIN;
                if (::poll(&pfd, 1, 200) <= 0)
                {
                    continue;
                }
                sockaddr_storage addr{};
                socklen_t len = sizeof(addr);
                int const fd = ::accept(listenFd, reinterpret_cast<sockaddr*>(&addr), &len);
                if (fd < 0)
                {
                    continue;
                }
                char text[INET6_ADDRSTRLEN] = {};
                if (addr.ss_family == AF_INET)
                {
                    ::inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in const*>(&addr)->sin_addr, text, sizeof(text));
                }
                else if (addr.ss_family == AF_INET6)
                {
                    ::inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6 const*>(&addr)->sin6_addr, text, sizeof(text));
                }
                auto done = std::make_shared<std::atomic<bool>>(false);
                std::lock_guard lock{jobsMutex};
                jobs.push_back(Job{std::thread([this, fd, done, peer = std::string(text)] {
                                       connection(fd, peer);
                                       done->store(true);
                                   }),
                    done});
            }
        }

        void shutdownAll()
        {
            stop.store(true);
            if (listenFd >= 0)
            {
                ::shutdown(listenFd, SHUT_RDWR);
            }
            if (acceptThread.joinable())
            {
                acceptThread.join();
            }
            if (listenFd >= 0)
            {
                ::close(listenFd);
                listenFd = -1;
            }
            {
                std::lock_guard lock{socketsMutex};
                for (auto const& weak : sockets)
                {
                    if (auto socket = weak.lock())
                    {
                        socket->close(1001);
                    }
                }
            }
            std::vector<Job> local;
            {
                std::lock_guard lock{jobsMutex};
                local.swap(jobs);
            }
            for (auto& job : local)
            {
                job.thread.join();
            }
        }
    };

    HttpServer::HttpServer(Handler handler, UpgradeHandler upgrade, RawHandler raw)
        : _impl(std::make_unique<Impl>())
    {
        _impl->handler = std::move(handler);
        _impl->upgrade = std::move(upgrade);
        _impl->raw = std::move(raw);
    }

    HttpServer::~HttpServer()
    {
        stop();
    }

    bool HttpServer::start(int port, std::string& error)
    {
        int const fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0)
        {
            error = "socket failed";
            return false;
        }
        int yes = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(static_cast<std::uint16_t>(port));
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        {
            error = "bind failed on port " + std::to_string(port) + ": " + std::strerror(errno);
            ::close(fd);
            return false;
        }
        if (::listen(fd, 64) != 0)
        {
            error = "listen failed";
            ::close(fd);
            return false;
        }
        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len);
        _impl->listenFd = fd;
        _impl->boundPort = ntohs(bound.sin_port);
        _impl->stop.store(false);
        _impl->acceptThread = std::thread([this] { _impl->acceptLoop(); });
        return true;
    }

    void HttpServer::stop()
    {
        if (_impl)
        {
            _impl->shutdownAll();
        }
    }

    int HttpServer::port() const
    {
        return _impl->boundPort;
    }
}
