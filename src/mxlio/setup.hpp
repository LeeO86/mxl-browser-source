// SPDX-License-Identifier: MIT
// Output domain preparation (SPEC §9): tmpfs check, domain files written once,
// an existing different id kept. No libmxl here, so unit tests can use it.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace mbs::mxlio
{
    /// The MXL root is missing, or neither it nor the domain directory is on a tmpfs (exit 78).
    class RootError : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    [[nodiscard]] bool isTmpfs(std::string const& path);

    struct DomainSetup
    {
        std::string id;      // the id the domain really has
        bool created = false;
        bool mismatch = false; // an existing domain_def.json had another id (kept)
    };

    /// Checks the root or `dir` is a tmpfs (when requireTmpfs), creates `dir` with domain_def.json and options.json
    /// when missing, never rewrites them. Throws RootError, or std::runtime_error on I/O errors.
    DomainSetup prepareDomain(std::string const& root, std::string const& dir, std::string const& wantedId, std::string const& label,
        std::uint64_t historyNs, bool requireTmpfs);

    /// Removes the own domain directory; refuses the root and mirror domains. Returns false when refused.
    bool removeDomain(std::string const& root, std::string const& dir);

    /// Raises RLIMIT_NOFILE to its hard limit (MXL keeps one descriptor per grain).
    void raiseFileLimit();
}
