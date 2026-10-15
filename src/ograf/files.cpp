// SPDX-License-Identifier: MIT
#include "ograf/files.hpp"

#include <ctime>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

namespace mbs::ograf
{
    std::string randomId()
    {
        static thread_local std::mt19937_64 rng{std::random_device{}()};
        std::uint64_t const a = rng();
        std::uint64_t const b = rng();
        unsigned char bytes[16];
        for (int i = 0; i < 8; ++i)
        {
            bytes[i] = static_cast<unsigned char>(a >> (8 * i));
            bytes[8 + i] = static_cast<unsigned char>(b >> (8 * i));
        }
        bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
        bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);
        static constexpr char kHex[] = "0123456789abcdef";
        std::string out;
        for (int i = 0; i < 16; ++i)
        {
            if (i == 4 || i == 6 || i == 8 || i == 10)
            {
                out += '-';
            }
            out += kHex[bytes[i] >> 4];
            out += kHex[bytes[i] & 15];
        }
        return out;
    }

    std::string isoNow()
    {
        std::time_t const now = std::time(nullptr);
        std::tm tm{};
        gmtime_r(&now, &tm);
        char text[32];
        std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &tm);
        return text;
    }

    std::optional<std::string> readFile(std::string const& path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            return std::nullopt;
        }
        std::ostringstream out;
        out << in.rdbuf();
        return out.str();
    }

    bool writeFileAtomic(std::string const& path, std::string const& text)
    {
        std::error_code ec;
        auto const tmp = path + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out || !(out << text) || !out.flush())
            {
                return false;
            }
        }
        std::filesystem::rename(tmp, path, ec);
        return !ec;
    }

    std::string parseJson(picojson::value& out, std::string const& text)
    {
        return picojson::parse(out, text);
    }
}
