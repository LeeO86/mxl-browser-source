// SPDX-License-Identifier: MIT
#include "mxlio/setup.hpp"

#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "picojson/picojson.h"
#include "util/logging.hpp"

namespace mbs::mxlio
{
    namespace
    {
        constexpr long kTmpfsMagic = 0x01021994;

        std::string readFile(std::filesystem::path const& path)
        {
            std::ifstream in(path);
            std::stringstream text;
            text << in.rdbuf();
            return text.str();
        }

        void writeFile(std::filesystem::path const& path, std::string const& text)
        {
            auto const tmp = path.string() + ".tmp";
            {
                std::ofstream out(tmp, std::ios::trunc);
                out << text;
                if (!out)
                {
                    throw std::runtime_error("cannot write " + tmp);
                }
            }
            std::filesystem::rename(tmp, path);
        }
    }

    bool isTmpfs(std::string const& path)
    {
        struct statfs st{};
        return ::statfs(path.c_str(), &st) == 0 && static_cast<long>(st.f_type) == kTmpfsMagic;
    }

    DomainSetup prepareDomain(std::string const& root, std::string const& dir, std::string const& wantedId, std::string const& label,
        std::uint64_t historyNs, bool requireTmpfs)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (!fs::is_directory(root, ec))
        {
            throw RootError("MXL root " + root + " does not exist: mount a tmpfs there (or set MXL_DOMAIN_SCAN_PATH)");
        }
        // The flows live in `dir`. A pod mounts only that directory (a hostPath on the node's tmpfs,
        // SPEC §15.3), so a tmpfs at `dir` qualifies as well as a tmpfs root. Checked before
        // anything is created.
        if (requireTmpfs && !isTmpfs(root) && !(fs::is_directory(dir, ec) && isTmpfs(dir)))
        {
            throw RootError("MXL domain " + dir + " is not on a tmpfs: mount one at " + root + " or at " + dir +
                            ", or set BROWSER_REQUIRE_TMPFS=false for tests");
        }
        fs::create_directories(dir, ec);
        if (ec)
        {
            throw std::runtime_error("cannot create MXL domain " + dir + ": " + ec.message());
        }
        DomainSetup out;
        out.id = wantedId;
        auto const def = fs::path(dir) / "domain_def.json";
        if (fs::exists(def))
        {
            picojson::value v;
            auto const err = picojson::parse(v, readFile(def));
            if (!err.empty() || !v.is<picojson::object>() || !v.get("id").is<std::string>() || v.get("id").get<std::string>().empty())
            {
                throw std::runtime_error(def.string() + " has no id");
            }
            out.id = v.get("id").get<std::string>();
            if (out.id != wantedId)
            {
                out.mismatch = true;
                log::error("domain_id_mismatch", {{"path", def.string()}, {"existing", out.id}, {"configured", wantedId}, {"details", "keeping the existing id"}});
            }
        }
        else
        {
            // BCP-007-03 requires id, label, description and tags.
            picojson::object o;
            o["id"] = picojson::value(wantedId);
            o["label"] = picojson::value(label);
            o["description"] = picojson::value(std::string("Output domain of mxl-browser-source"));
            o["tags"] = picojson::value(picojson::object{});
            writeFile(def, picojson::value(o).serialize(true));
            out.created = true;
        }
        auto const options = fs::path(dir) / "options.json";
        if (!fs::exists(options))
        {
            picojson::object o;
            o["urn:x-mxl:option:history_duration/v1.0"] = picojson::value(static_cast<double>(historyNs));
            writeFile(options, picojson::value(o).serialize(true));
        }
        return out;
    }

    bool removeDomain(std::string const& root, std::string const& dir)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        auto const r = fs::weakly_canonical(root, ec);
        auto const d = fs::weakly_canonical(dir, ec);
        if (d.empty() || d == r || d == d.root_path() || fs::exists(d / "x-mxl-fabrics-agent.mirror"))
        {
            log::error("domain_cleanup_refused", {{"path", dir}});
            return false;
        }
        fs::remove_all(d, ec);
        if (ec)
        {
            log::warn("domain_cleanup_failed", {{"path", dir}, {"details", ec.message()}});
            return false;
        }
        log::info("domain_removed", {{"path", dir}});
        return true;
    }

    void raiseFileLimit()
    {
        rlimit files{};
        if (::getrlimit(RLIMIT_NOFILE, &files) == 0 && files.rlim_cur < files.rlim_max)
        {
            auto const before = files.rlim_cur;
            files.rlim_cur = files.rlim_max;
            if (::setrlimit(RLIMIT_NOFILE, &files) == 0)
            {
                log::info("nofile_raised", {{"from", static_cast<std::uint64_t>(before)}, {"to", static_cast<std::uint64_t>(files.rlim_cur)}});
            }
        }
    }

    std::string metricsShmDir(std::string const& shmRoot, std::string const& seed, unsigned uid)
    {
        std::string name;
        for (char const c : seed)
        {
            bool const plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
            name.push_back(plain ? c : '_');
        }
        if (name.size() > 64)
        {
            name.resize(64);
        }
        if (name.empty() || name.find_first_not_of('.') == std::string::npos)
        {
            name = "default";
        }
        return shmRoot + "/mxl-browser-source-metrics-" + name + "-" + std::to_string(uid);
    }

    bool linkToFreshDir(std::string const& link, std::string const& target, std::string& error)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        auto const fail = [&](std::string const& step, std::string const& what) {
            error = step + " " + what;
            return false;
        };
        struct stat st{};
        if (::lstat(target.c_str(), &st) == 0)
        {
            if (st.st_uid != ::getuid())
            {
                return fail("keep", target + ": it belongs to uid " + std::to_string(st.st_uid));
            }
            fs::remove_all(target, ec);
            if (ec)
            {
                return fail("clear", target + ": " + ec.message());
            }
        }
        fs::create_directories(target, ec);
        if (ec)
        {
            return fail("create", target + ": " + ec.message());
        }
        fs::permissions(target, fs::perms::owner_all, fs::perm_options::replace, ec);
        if (ec)
        {
            return fail("chmod", target + ": " + ec.message());
        }
        fs::remove_all(link, ec);
        if (ec)
        {
            return fail("remove", link + ": " + ec.message());
        }
        fs::create_directory_symlink(target, link, ec);
        if (ec)
        {
            return fail("link", link + ": " + ec.message());
        }
        return true;
    }
}
