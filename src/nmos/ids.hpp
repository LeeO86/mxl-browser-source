// SPDX-License-Identifier: MIT
// NMOS and MXL ids (SPEC §8.1): UUIDv5 with the namespace UUIDv5(nil, "mxl-browser-source")
// and the name NMOS_SEED + "/" + item. A format or key-mode change gives new flow ids (the MXL
// flow definitions differ); senders keep theirs.
#pragma once

#include <string>

namespace mbs::nmos
{
    struct Ids
    {
        std::string node;
        std::string device;
        std::string videoSource;
        std::string keySource;
        std::string audioSource;
        std::string videoFlow;
        std::string keyFlow;
        std::string audioFlow;
        std::string videoSender;
        std::string keySender;
        std::string audioSender;
        std::string domain; // default MXL_OUTPUT_DOMAIN_ID
    };

    /// `format` is the format name ("1080p50"), `keyMode` "off", "v210a" or "fill_key".
    Ids makeIds(std::string const& seed, std::string const& format, std::string const& keyMode, int audioChannels);

    /// The uuid of one item name (for tests and the default domain id).
    std::string idFor(std::string const& seed, std::string const& item);
}
