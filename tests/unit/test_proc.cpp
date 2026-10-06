// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>

#include "util/proc.hpp"

using namespace mbs::util;

TEST_CASE("CPU and memory of this process and of its children")
{
    auto const self = selfUsage();
    CHECK(self.residentBytes > 0);

    auto const before = descendantsUsage();
    pid_t const child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0)
    {
        // Burn about 0.3 s of CPU, then wait to be killed.
        auto const end = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        volatile unsigned long spin = 0;
        while (std::chrono::steady_clock::now() < end)
        {
            ++spin;
        }
        ::pause();
        ::_exit(0);
    }
    ::usleep(500'000);
    auto const running = descendantsUsage();
    CHECK(running.residentBytes > before.residentBytes);
    CHECK(running.cpuSeconds >= before.cpuSeconds + 0.2);
    ::kill(child, SIGKILL);
    ::waitpid(child, nullptr, 0);
    // Reaped: its time moves into our cutime, the counter does not drop.
    auto const after = descendantsUsage();
    CHECK(after.cpuSeconds >= running.cpuSeconds - 0.02);
}
