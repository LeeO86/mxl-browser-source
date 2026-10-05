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
        _actions.commit(index, fresh);
        ++_counters.grains;
        if (!fresh)
        {
            ++(state == PageState::Ready ? _counters.repeated : _counters.repeatedHold);
        }
        _actions.writeAudio(index);
        if (state != PageState::Crashed)
        {
            _actions.requestFrame();
        }
    }
}
