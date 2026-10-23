// SPDX-License-Identifier: MIT
// The event counters of SPEC §12 at 0 from the start, with every label value the code can
// produce, so /metrics has their series before the first event (1.3.3). The snapshot counters
// (grains, audio, frame clock) are set on every scrape and need no entry here.
#pragma once

#include "util/metrics.hpp"

namespace mbs::ops
{
    /// `audio`: the audio counters too (BROWSER_AUDIO_CHANNELS > 0).
    void startCounters(util::Metrics& metrics, bool audio);
}
