// SPDX-License-Identifier: MIT
// CPU time and resident memory of this process and of the CEF processes below it, from /proc
// (SPEC §12: process_* and cef_processes_*).
#pragma once

#include <cstdint>

namespace mbs::util
{
    struct ProcUsage
    {
        double cpuSeconds = 0; // user + system
        std::uint64_t residentBytes = 0;
    };

    /// This process alone.
    ProcUsage selfUsage();

    /// Every process below this one (zygotes, renderers, GPU and utility processes). CPU includes
    /// the time of exited processes their parents have reaped, so it does not drop after a crash.
    ProcUsage descendantsUsage();
}
