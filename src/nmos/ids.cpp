// SPDX-License-Identifier: MIT
#include "nmos/ids.hpp"

#include "util/uuid.hpp"

namespace mbs::nmos
{
    std::string idFor(std::string const& seed, std::string const& item)
    {
        static util::Uuid const ns = util::uuidV5(util::Uuid{}, "mxl-browser-source");
        return util::uuidV5(ns, seed + "/" + item).toString();
    }

    Ids makeIds(std::string const& seed, std::string const& format, std::string const& keyMode, int audioChannels)
    {
        Ids ids;
        ids.node = idFor(seed, "node");
        ids.device = idFor(seed, "device");
        ids.videoSource = idFor(seed, "source/video");
        ids.keySource = idFor(seed, "source/key");
        ids.audioSource = idFor(seed, "source/audio");
        ids.videoFlow = idFor(seed, "flow/video/" + format + "/" + keyMode);
        ids.keyFlow = idFor(seed, "flow/key/" + format);
        ids.audioFlow = idFor(seed, "flow/audio/" + std::to_string(audioChannels));
        ids.videoSender = idFor(seed, "sender/video");
        ids.keySender = idFor(seed, "sender/key");
        ids.audioSender = idFor(seed, "sender/audio");
        ids.domain = idFor(seed, "domain");
        return ids;
    }
}
