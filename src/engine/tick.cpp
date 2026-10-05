// SPDX-License-Identifier: MIT
#include "engine/tick.hpp"

namespace mbs::engine
{
    void Tick::framePainted()
    {
        _painted.store(true, std::memory_order_release);
    }

    void Tick::run(std::uint64_t index, PageState state)
    {
        if (_started && index <= _last)
        {
            return; // woke twice within one grain
        }
        // The writer never leaves a gap in the ring: grains the thread woke too late for are
        // committed as repeats, with their audio.
        if (_started)
        {
            for (std::uint64_t k = _last + 1; k < index; ++k)
            {
                _actions.commit(k, false);
                _actions.writeAudio(k);
                ++_counters.missed;
                ++_counters.grains;
            }
        }
        _started = true;
        _last = index;

        bool const fresh = _painted.exchange(false, std::memory_order_acq_rel);
        if (fresh)
        {
            _outstanding = false;
        }
        _actions.commit(index, fresh);
        ++_counters.grains;
        if (!fresh)
        {
            if (state == PageState::Ready && _outstanding)
            {
                ++_counters.repeatedLate;
            }
            else
            {
                ++_counters.repeatedHold;
            }
        }
        _actions.writeAudio(index);

        if (state == PageState::Crashed)
        {
            _outstanding = false; // no renderer: nothing will paint until the reload
            return;
        }
        if (_outstanding)
        {
            // A late page skips a BeginFrame instead of falling behind.
            ++_counters.beginFramesSkipped;
            return;
        }
        _actions.requestFrame();
        _outstanding = true;
    }
}
