// SPDX-License-Identifier: MIT
// Operator-screen widgets (SPEC §10.6): GET /widgets and GET /widget/<id>. No CEF here, so the
// unit tests reach it.
#pragma once

#include <string>
#include <string_view>

#include "ops/httpserver.hpp"

namespace mbs::ops
{
    /// True for /widgets and /widget/<id>.
    bool isWidgetPath(std::string_view path);

    /// The list (CORS only for the origins in `frameAncestors`, the WIDGET_FRAME_ANCESTORS source list)
    /// or the widget page `page`, which only those origins may frame.
    void widgetResponse(std::string const& frameAncestors, HttpRequest const& req, std::string_view page, HttpResponse& res);
}
