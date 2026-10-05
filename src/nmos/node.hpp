// SPDX-License-Identifier: MIT
// The NMOS node (SPEC §8) through nmos-cpp: one device with the sr-ctrl control, the video
// sender, the key sender (fill_key) and the audio sender (audio channels > 0), transport
// urn:x-nmos:transport:mxl (BCP-007-03). Static registry, DNS-SD off unless asked for.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "nmos/ids.hpp"

namespace mbs::nmos
{
    struct NodeSettings
    {
        Ids ids;
        int port = 3312;
        std::string label = "MXL Browser Source"; // node and device
        std::string senderLabel = "Browser";      // "<label> Video|Key|Audio", group hint "<label>:<role>"
        std::map<std::string, std::vector<std::string>> tags;
        std::string hostAddress;
        std::string registryAddress;
        int registryPort = 3210;
        bool dnsSd = false;
        std::string domainId; // the output domain's id (announced in IS-05)
        std::uint32_t width = 1920;
        std::uint32_t height = 1080;
        std::int64_t rateNum = 50;
        std::int64_t rateDen = 1;
        bool v210a = false;
        bool keyFlow = false;
        int audioChannels = 2;
        std::map<std::string, bool> enabled; // "video", "key", "audio": restored master_enable
        int shutdownTimeoutS = 10;
    };

    class Node
    {
    public:
        /// `onEnable(sender, enabled)`: IS-05 activated master_enable for "video", "key" or "audio".
        Node(NodeSettings settings, std::function<void(std::string const&, bool)> onEnable);
        ~Node();
        Node(Node const&) = delete;
        Node& operator=(Node const&) = delete;

        /// Starts the node and checks that this process listens on the NMOS port. Throws
        /// std::runtime_error (exit 75) when it cannot.
        void start();
        /// Deletes the resources (senders first, node last) so the registry gets the DELETEs.
        /// False when that did not finish within the shutdown budget.
        bool stop();

        [[nodiscard]] bool registered() const; // the registration API accepted the node
        [[nodiscard]] NodeSettings const& settings() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
    };
}
