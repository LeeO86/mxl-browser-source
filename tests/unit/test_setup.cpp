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
