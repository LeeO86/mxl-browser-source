// SPDX-License-Identifier: MIT
// The OGraf renderer page behind ograf::Renderer (docs/ograf.md §3): each call is a DevTools
// Runtime.evaluate of window.ografRenderer.call(…) with awaitPromise, so the answer is the
// graphic's ReturnPayload. One HTTP thread waits per call; calls on different layers never wait
// for each other.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "ograf/layers.hpp"
#include "util/metrics.hpp"

namespace mbs::cef
{
    class Browser;
}

namespace mbs::app
{
    class OgrafBridge final : public ograf::Renderer
    {
    public:
        OgrafBridge(cef::Browser& browser, std::chrono::milliseconds timeout, util::Metrics& metrics);

        ograf::CallResult call(int layer, std::string const& method, picojson::value const& args) override;

        /// From PageObserver::onDevToolsResult; true when the id was one of ours.
        bool deliver(int id, bool success, std::string const& text);
        /// The renderer page is (not) there: calls answer Unavailable while it is not.
        void setAvailable(bool available);
        [[nodiscard]] bool available() const { return _available.load(); }

    private:
        struct Pending
        {
            bool done = false;
            ograf::CallResult result;
        };

        ograf::CallResult callOnce(int layer, std::string const& method, picojson::value const& args);

        cef::Browser& _browser;
        std::chrono::milliseconds _timeout;
        util::Metrics& _metrics;
        std::atomic<bool> _available{false};
        std::mutex _mu;
        std::condition_variable _cv;
        std::map<int, std::shared_ptr<Pending>> _pending;
    };
}
