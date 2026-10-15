// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "mxlio/setup.hpp"
#include "picojson/picojson.h"

namespace fs = std::filesystem;
using namespace mbs::mxlio;

TEST_CASE("the own domain must be on a tmpfs: the root or the domain directory")
{
    auto const tag = std::to_string(::getpid());
    fs::path const shm = "/dev/shm/mbs-setup-" + tag; // a tmpfs on Linux
    fs::path const disk = fs::temp_directory_path() / ("mbs-setup-" + tag);
    fs::create_directories(shm);
    fs::create_directories(disk);
    REQUIRE(isTmpfs(shm.string()));
    auto const id = "2e3021a6-6093-5d97-a25d-341aade2db6a";

    // A tmpfs root.
    CHECK(prepareDomain(shm.string(), (shm / "a").string(), id, "a", 200'000'000, true).created);
    {
        // BCP-007-03 schema: id, label, description and tags are required.
        std::ifstream in(shm / "a" / "domain_def.json");
        picojson::value v;
        in >> v;
        REQUIRE(v.is<picojson::object>());
        CHECK(v.get("id").get<std::string>() == id);
        CHECK(v.get("label").is<std::string>());
        CHECK(v.get("description").is<std::string>());
        CHECK(v.get("tags").is<picojson::object>());
    }

    if (!isTmpfs(disk.string()))
    {
        // A root on disk and the domain on a tmpfs (a pod mounts only its domain directory).
        fs::create_directories(shm / "b");
        CHECK(prepareDomain(disk.string(), (shm / "b").string(), id, "b", 200'000'000, true).created);
        // Neither: refused before anything is created.
        CHECK_THROWS_AS(prepareDomain(disk.string(), (disk / "c").string(), id, "c", 200'000'000, true), RootError);
        CHECK_FALSE(fs::exists(disk / "c"));
        // Tests may allow it.
        CHECK(prepareDomain(disk.string(), (disk / "c").string(), id, "c", 200'000'000, false).created);
    }
    fs::remove_all(shm);
    fs::remove_all(disk);
}

TEST_CASE("Chromium's histogram directory is per instance, and the link to it is made step by step")
{
    // Per NMOS seed and uid; a seed is never a path.
    CHECK(metricsShmDir("/dev/shm", "studio-a", 1000) == "/dev/shm/mxl-browser-source-metrics-studio-a-1000");
    CHECK(metricsShmDir("/dev/shm", "studio-b", 1000) != metricsShmDir("/dev/shm", "studio-a", 1000));
    CHECK(metricsShmDir("/dev/shm", "../x/y z", 1000) == "/dev/shm/mxl-browser-source-metrics-.._x_y_z-1000");
    CHECK(metricsShmDir("/dev/shm", "..", 1000) == "/dev/shm/mxl-browser-source-metrics-default-1000");
    CHECK(metricsShmDir("/dev/shm", "", 0) == "/dev/shm/mxl-browser-source-metrics-default-0");
    CHECK(metricsShmDir("/dev/shm", std::string(200, 'a'), 7).size() == std::string("/dev/shm/mxl-browser-source-metrics-").size() + 64 + 2);

    auto const root = fs::temp_directory_path() / ("mbs-metrics-" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "cache");
    auto const link = (root / "cache" / "BrowserMetrics").string();
    auto const target = (root / "shm" / "metrics-a").string();
    std::string error;
    // A leftover directory in the cache and an old file in the target are replaced.
    fs::create_directories(link);
    fs::create_directories(target);
    std::ofstream(target + "/old.pma") << "x";
    REQUIRE(linkToFreshDir(link, target, error));
    CHECK(error.empty());
    CHECK(fs::is_symlink(link));
    CHECK(fs::read_symlink(link) == target);
    CHECK(fs::is_empty(target));
    CHECK((fs::status(target).permissions() & fs::perms::all) == fs::perms::owner_all);
    // Again (a restart): the link is renewed.
    REQUIRE(linkToFreshDir(link, target, error));
    CHECK(fs::is_symlink(link));
    // A target that cannot be made says which step failed, and leaves the cache alone.
    std::ofstream((root / "blocker").string()) << "file";
    CHECK_FALSE(linkToFreshDir(link, (root / "blocker" / "sub").string(), error));
    CHECK(error.rfind("create ", 0) == 0);
    CHECK(fs::is_symlink(link));
    fs::remove_all(root);
}
