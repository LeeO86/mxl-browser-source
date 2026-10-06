// SPDX-License-Identifier: MIT
#include "mxlio/setup.hpp"

#include <sys/resource.h>
#include <sys/vfs.h>

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
            picojson::object o;
            o["id"] = picojson::value(wantedId);
            o["label"] = picojson::value(label);
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
}
