// SPDX-License-Identifier: MIT
#include "util/net.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

namespace mbs::util
{
    std::string primaryIpv4()
    {
        ifaddrs* list = nullptr;
        if (::getifaddrs(&list) != 0)
        {
            return {};
        }
        std::string found;
        for (ifaddrs* it = list; it != nullptr && found.empty(); it = it->ifa_next)
        {
            if (it->ifa_addr == nullptr || it->ifa_addr->sa_family != AF_INET)
            {
                continue;
            }
            char text[INET_ADDRSTRLEN] = {};
            ::inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in const*>(it->ifa_addr)->sin_addr, text, sizeof(text));
            std::string const address(text);
            if (address.rfind("127.", 0) != 0)
            {
                found = address;
            }
        }
        ::freeifaddrs(list);
        return found;
    }

    bool ownsListener(int port)
    {
        // Inodes of sockets in LISTEN state (st 0A) on the port.
        std::set<std::string> inodes;
        for (char const* table : {"/proc/net/tcp", "/proc/net/tcp6"})
        {
            std::ifstream in(table);
            std::string line;
            std::getline(in, line); // header
            while (std::getline(in, line))
            {
                std::istringstream fields(line);
                std::string slot, local, remote, state, queues, timer, retransmit, uid, timeout, inode;
                fields >> slot >> local >> remote >> state >> queues >> timer >> retransmit >> uid >> timeout >> inode;
                auto const colon = local.rfind(':');
                if (state != "0A" || colon == std::string::npos)
                {
                    continue;
                }
                if (std::strtol(local.substr(colon + 1).c_str(), nullptr, 16) == port)
                {
                    inodes.insert(inode);
                }
            }
        }
        if (inodes.empty())
        {
            return false;
        }
        DIR* dir = ::opendir("/proc/self/fd");
        if (dir == nullptr)
        {
            return false;
        }
        bool owned = false;
        while (dirent* entry = ::readdir(dir))
        {
            char target[256] = {};
            std::string const path = std::string("/proc/self/fd/") + entry->d_name;
            ssize_t const n = ::readlink(path.c_str(), target, sizeof(target) - 1);
            if (n <= 0)
            {
                continue;
            }
            std::string const link(target, static_cast<std::size_t>(n));
            if (link.rfind("socket:[", 0) == 0 && inodes.count(link.substr(8, link.size() - 9)) != 0)
            {
                owned = true;
                break;
            }
        }
        ::closedir(dir);
        return owned;
    }

    int connectTcp(std::string const& host, int port, int timeoutMs)
    {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<std::uint16_t>(port));
        if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
        {
            return -1;
        }
        int const fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd < 0)
        {
            return -1;
        }
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 && errno != EINPROGRESS)
        {
            ::close(fd);
            return -1;
        }
        pollfd pfd{fd, POLLOUT, 0};
        int error = 0;
        socklen_t len = sizeof(error);
        if (::poll(&pfd, 1, timeoutMs) != 1 || ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) != 0 || error != 0)
        {
            ::close(fd);
            return -1;
        }
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) & ~O_NONBLOCK);
        return fd;
    }

    int httpGet(std::string const& host, int port, std::string const& path, int timeoutMs, std::string* body)
    {
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        int const fd = connectTcp(host, port, timeoutMs);
        if (fd < 0)
        {
            return -1;
        }
        // HTTP/1.1: Chromium's DevTools server does not answer HTTP/1.0.
        std::string const request = "GET " + path + " HTTP/1.1\r\nHost: " + host + ":" + std::to_string(port) + "\r\nConnection: close\r\n\r\n";
        if (::send(fd, request.data(), request.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(request.size()))
        {
            ::close(fd);
            return -1;
        }
        std::string response;
        char buf[8192];
        while (response.size() < 8 * 1024 * 1024)
        {
            auto const left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            pollfd pfd{fd, POLLIN, 0};
            if (left <= 0 || ::poll(&pfd, 1, static_cast<int>(left)) != 1)
            {
                ::close(fd);
                return -1;
            }
            ssize_t const n = ::recv(fd, buf, sizeof(buf), 0);
            if (n < 0)
            {
                ::close(fd);
                return -1;
            }
            if (n == 0)
            {
                break;
            }
            response.append(buf, static_cast<std::size_t>(n));
            // A server may keep the connection open: stop at Content-Length.
            if (auto const end = response.find("\r\n\r\n"); end != std::string::npos)
            {
                std::string head = response.substr(0, end);
                std::transform(head.begin(), head.end(), head.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (auto const at = head.find("\r\ncontent-length:"); at != std::string::npos &&
                    response.size() >= end + 4 + std::strtoull(head.c_str() + at + 17, nullptr, 10))
                {
                    break;
                }
            }
        }
        ::close(fd);
        // "HTTP/1.x NNN ..."
        auto const space = response.find(' ');
        if (response.rfind("HTTP/", 0) != 0 || space == std::string::npos || response.size() < space + 4)
        {
            return -1;
        }
        int const status = std::atoi(response.substr(space + 1, 3).c_str());
        if (body != nullptr)
        {
            auto const end = response.find("\r\n\r\n");
            *body = end == std::string::npos ? std::string{} : response.substr(end + 4);
        }
        return status;
    }
}
