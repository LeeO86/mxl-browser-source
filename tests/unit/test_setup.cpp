// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <unistd.h>

#include <filesystem>
#include <string>

#include "mxlio/setup.hpp"

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
