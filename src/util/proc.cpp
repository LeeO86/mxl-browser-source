// SPDX-License-Identifier: MIT
#include "util/proc.hpp"

#include <dirent.h>
#include <unistd.h>

#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace mbs::util
{
    namespace
    {
        struct Stat
        {
            long ppid = 0;
            std::uint64_t ticks = 0;         // utime + stime
            std::uint64_t reapedTicks = 0;   // cutime + cstime
            std::uint64_t residentPages = 0; // rss
        };

        // /proc/<pid>/stat: fields after the name (which may contain spaces): 3 state, 4 ppid,
        // 14 utime, 15 stime, 16 cutime, 17 cstime, 24 rss.
        bool readStat(std::string const& pid, Stat& out)
        {
            std::ifstream in("/proc/" + pid + "/stat");
            std::string line;
            if (!std::getline(in, line))
            {
                return false;
            }
            auto const close = line.rfind(')');
            if (close == std::string::npos)
            {
                return false;
            }
            std::istringstream rest(line.substr(close + 2));
            std::vector<std::string> f;
            for (std::string field; rest >> field && f.size() < 22;)
            {
                f.push_back(field);
            }
            if (f.size() < 22)
            {
                return false;
            }
            out.ppid = std::stol(f[1]);
            out.ticks = std::stoull(f[11]) + std::stoull(f[12]);
            out.reapedTicks = std::stoull(f[13]) + std::stoull(f[14]);
            out.residentPages = std::stoull(f[21]);
            return true;
        }

        double tick() { return 1.0 / static_cast<double>(::sysconf(_SC_CLK_TCK)); }
        std::uint64_t page() { return static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE)); }
    }

    ProcUsage selfUsage()
    {
        Stat s;
        if (!readStat(std::to_string(::getpid()), s))
        {
            return {};
        }
        return {static_cast<double>(s.ticks) * tick(), s.residentPages * page()};
    }

    ProcUsage descendantsUsage()
    {
        std::map<long, Stat> all;
        if (DIR* dir = ::opendir("/proc"))
        {
            while (dirent* entry = ::readdir(dir))
            {
                std::string const name = entry->d_name;
                if (name.empty() || !std::isdigit(static_cast<unsigned char>(name[0])))
                {
                    continue;
                }
                Stat s;
                if (readStat(name, s))
                {
                    all.emplace(std::stol(name), s);
                }
            }
            ::closedir(dir);
        }
        long const self = ::getpid();
        ProcUsage usage;
        std::uint64_t ticks = all.count(self) != 0 ? all.at(self).reapedTicks : 0; // reaped direct children
        for (auto const& [pid, s] : all)
        {
            long up = s.ppid;
            for (int depth = 0; depth < 8 && up > 1 && up != self; ++depth)
            {
                auto const it = all.find(up);
                up = it == all.end() ? 0 : it->second.ppid;
            }
            if (pid != self && up == self)
            {
                ticks += s.ticks + s.reapedTicks;
                usage.residentBytes += s.residentPages * page();
            }
        }
        usage.cpuSeconds = static_cast<double>(ticks) * tick();
        return usage;
    }
}
