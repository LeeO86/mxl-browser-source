// SPDX-License-Identifier: MIT
// The OGraf Server API v1 at /ograf/v1 (docs/ograf.md §6) and our own routes /api/v1/ograf/…
// (§7): one renderer "main", render targets {"layer": 1…8}. No CEF here.
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "ograf/layers.hpp"
#include "ograf/library.hpp"
#include "ops/httpserver.hpp"
#include "util/metrics.hpp"

namespace mbs::ograf
{
    struct ApiContext
    {
        Library& library;
        LayerSet& layers;
        std::string name;    // BROWSER_LABEL
        std::string version; // the function's
        std::vector<std::string> corsOrigins;
        /// "" when the renderer page runs, else why not (shown as WARNING; actions answer 409).
        std::function<std::string()> rendererProblem;
        util::Metrics* metrics = nullptr; // ograf_uploads_total
    };

    /// /ograf/v1[/…] and /api/v1/ograf/…
    bool isOgrafPath(std::string_view path);
    /// True for the upload route, whose body the HTTP server streams to a file.
    bool isUploadRequest(ops::HttpRequest const& req);

    /// Answers one request. Authorization is the caller's (the same token rule as /api/v1).
    void handleOgraf(ApiContext& ctx, ops::HttpRequest const& req, ops::HttpResponse& res);
}
