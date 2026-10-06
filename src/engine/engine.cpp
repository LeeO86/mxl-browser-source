// SPDX-License-Identifier: MIT
#include "engine/engine.hpp"

#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <cmath>
#include <utility>

#include "convert/convert.hpp"
#include "util/taskpool.hpp"
#include "util/logging.hpp"

namespace mbs::engine
{
    namespace
    {
        void allocate(GrainBuffers& set, EngineSettings const& s)
        {
            set.fill.assign(static_cast<std::size_t>(convert::v210RowBytes(s.width)) * s.height, 0);
            if (s.alphaPlane)
            {
                set.alpha.assign(static_cast<std::size_t>(convert::alphaRowBytes(s.width)) * s.height, 0);
            }
            if (s.keyFlow)
            {
                set.key.assign(static_cast<std::size_t>(convert::v210RowBytes(s.width)) * s.height, 0);
            }
        }

        // Converts a paint into a buffer set; with a pool, in bands of rows on its threads.
        void convertInto(GrainBuffers& set, EngineSettings const& s, std::uint8_t const* bgra, util::TaskPool* pool = nullptr)
        {
            convert::Request request;
            request.bgra = bgra;
            request.bgraStride = static_cast<std::size_t>(s.width) * 4;
            request.width = s.width;
            request.height = s.height;
            request.straight = (s.alphaPlane || s.keyFlow) && s.straightFill;
            request.fill = set.fill.data();
            request.alpha = set.alpha.empty() ? nullptr : set.alpha.data();
            request.key = set.key.empty() ? nullptr : set.key.data();
            if (pool == nullptr || s.convertThreads <= 1)
            {
                convert::convertFrame(request);
                return;
            }
            std::vector<std::function<void()>> bands;
            std::uint32_t const n = std::min<std::uint32_t>(s.convertThreads * 2, s.height);
            for (std::uint32_t b = 0; b < n; ++b)
            {
                std::uint32_t const first = s.height * b / n;
                std::uint32_t const end = s.height * (b + 1) / n;
                bands.emplace_back([request, first, end] { convert::convertRows(request, nullptr, first, end); });
            }
            pool->run(bands);
        }
    }

    Engine::Engine(EngineSettings settings, Clock& clock, Outputs& outputs, util::Metrics& metrics)
        : _settings(settings)
        , _clock(clock)
        , _outputs(outputs)
        , _metrics(metrics)
        , _store(settings.width, settings.height)
    {
        // Every buffer is allocated here; nothing is allocated per frame (SPEC §18 rule 14).
        _pool.resize(_settings.videoDelayGrains + 3);
        for (auto& set : _pool)
        {
            allocate(set, _settings);
        }
        allocate(_transparent, _settings);
        allocate(_black, _settings);
        std::vector<std::uint8_t> bgra(static_cast<std::size_t>(_settings.width) * _settings.height * 4, 0);
        convertInto(_transparent, _settings, bgra.data());
        for (std::size_t i = 3; i < bgra.size(); i += 4)
        {
            bgra[i] = 255;
        }
        convertInto(_black, _settings, bgra.data());
        _ring.assign(_settings.videoDelayGrains + 1, kTransparent);

        if (_settings.audioChannels > 0)
        {
            _fifo = std::make_unique<audio::Fifo>(_settings.audioChannels, 48000);
            _resampler = std::make_unique<audio::Resampler>(_settings.audioChannels, _settings.audioTargetFrames);
            _audioBuffer.assign(static_cast<std::size_t>(4800) * _settings.audioChannels, 0.f);
            _stats.audioPeakDbfs.assign(_settings.audioChannels, -120.0);
        }

        if (_settings.previewWidth > 0)
        {
            _previewWidth = std::max<std::uint32_t>(2, std::min(_settings.previewWidth, _settings.width) & ~1U);
            auto const h = static_cast<std::uint32_t>(std::lround(static_cast<double>(_previewWidth) * _settings.height / _settings.width));
            _previewHeight = std::max<std::uint32_t>(2, h & ~1U);
            _previewScratch.assign(static_cast<std::size_t>(_previewWidth) * _previewHeight * 3, 0);
        }
    }

    Engine::~Engine()
    {
        stop();
    }

    void Engine::setRequestFrame(std::function<void()> request)
    {
        _requestFrame = std::move(request);
    }

    void Engine::start()
    {
        if (_started)
        {
            return;
        }
        _started = true;
        _converter = std::thread([this] { converterLoop(); });
        _ticker = std::thread([this] { tickLoop(); });
    }

    void Engine::stop()
    {
        _stop.store(true);
        {
            std::lock_guard lock{_wakeMutex};
            _stopping = true;
        }
        _wake.notify_all();
        if (_ticker.joinable())
        {
            _ticker.join();
        }
        if (_converter.joinable())
        {
            _converter.join();
        }
    }

    void Engine::paint(std::uint8_t const* bgra, std::size_t stride, std::uint32_t width, std::uint32_t height)
    {
        auto const now = _clock.nowNs();
        if (!_store.paint(bgra, stride, width, height))
        {
            std::lock_guard lock{_statsMutex};
            ++_stats.paintsRejected;
            return;
        }
        // A paint carries no frame id, and a BeginFrame on an unchanged page gets no paint, so a
        // paint cannot be paired with "its" BeginFrame: the latency is taken from the newest one.
        // Paints that came too late show up as paints never committed (stats().latePaints).
        if (auto const sent = _lastBeginFrameNs.load(); sent != 0 && now >= sent)
        {
            _metrics.observe("paint_latency_seconds", {}, static_cast<double>(now - sent) / 1e9);
        }
        {
            std::lock_guard lock{_wakeMutex};
            _paintPending = true;
        }
        _wake.notify_one();
    }

    void Engine::beginFrameSent()
    {
        _lastBeginFrameNs.store(_clock.nowNs());
    }

    void Engine::setPageState(PageState state)
    {
        _pageState.store(state);
    }

    void Engine::pushAudio(float const* const* planes, int planeCount, std::size_t frames)
    {
        if (_fifo)
        {
            // Page channels beyond the flow's count are dropped, missing ones are silent.
            _fifo->push(planes, planeCount, frames, _clock.nowNs());
        }
    }

    void Engine::setAudioActive(bool active)
    {
        _audioActive.store(active);
    }

    void Engine::setAvOffsetMs(int offsetMs)
    {
        _avOffsetMs.store(offsetMs);
    }

    void Engine::setEnabled(Flow flow, bool enabled)
    {
        (flow == Flow::Video ? _videoEnabled : flow == Flow::Key ? _keyEnabled : _audioEnabled).store(enabled);
    }

    bool Engine::enabled(Flow flow) const
    {
        return (flow == Flow::Video ? _videoEnabled : flow == Flow::Key ? _keyEnabled : _audioEnabled).load();
    }

    bool Engine::preview(std::vector<std::uint8_t>& rgb, std::uint64_t& sequence) const
    {
        std::lock_guard lock{_previewMutex};
        if (_previewSequence == 0 || _previewSequence == sequence)
        {
            return false;
        }
        rgb = _previewRgb;
        sequence = _previewSequence;
        return true;
    }

    void Engine::requestPreview()
    {
        if (_previewWidth == 0)
        {
            return;
        }
        {
            std::lock_guard lock{_wakeMutex};
            _previewRequested = true;
        }
        _wake.notify_one();
    }

    std::uint64_t Engine::nextVisibleIndex() const
    {
        // The BeginFrame of the current grain may still draw it; its paint is committed at the
        // next tick and leaves the delay ring videoDelayGrains later.
        return _clock.indexAt(_clock.nowNs()) + 1 + _settings.videoDelayGrains;
    }

    EngineStats Engine::stats() const
    {
        std::lock_guard lock{_statsMutex};
        EngineStats out = _stats;
        out.paintsDropped = _store.dropped();
        out.latePaints = out.paintsDropped + out.convertedUnused;
        return out;
    }

    GrainBuffers const& Engine::buffers(int id) const
    {
        if (id == kBlack)
        {
            return _black;
        }
        if (id < 0)
        {
            return _transparent;
        }
        return _pool[static_cast<std::size_t>(id)];
    }

    int Engine::freeSetLocked() const
    {
        for (int id = 0; id < static_cast<int>(_pool.size()); ++id)
        {
            if (id != _ready && std::find(_ring.begin(), _ring.end(), id) == _ring.end())
            {
                return id;
            }
        }
        return kNone; // cannot happen: the ring and _ready hold at most delay+2 of delay+3 sets
    }

    void Engine::converterLoop()
    {
        util::TaskPool converters(std::max(1U, _settings.convertThreads) - 1); // the converter thread helps
        std::uint8_t const* last = nullptr; // the newest paint, valid until the next take()
        while (true)
        {
            bool previewWanted = false;
            {
                std::unique_lock lock{_wakeMutex};
                _wake.wait(lock, [this] { return _paintPending || _previewRequested || _stopping; });
                if (_stopping)
                {
                    return;
                }
                _paintPending = false;
                previewWanted = std::exchange(_previewRequested, false);
            }
            auto const frame = _store.take();
            if (frame.bgra != nullptr)
            {
                last = frame.bgra;
                int id = kNone;
                {
                    std::lock_guard lock{_setsMutex};
                    id = freeSetLocked();
                }
                if (id == kNone)
                {
                    continue;
                }
                auto const t0 = std::chrono::steady_clock::now();
                convertInto(_pool[static_cast<std::size_t>(id)], _settings, frame.bgra, &converters);
                _metrics.observe("convert_seconds", {}, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
                bool replaced = false;
                {
                    std::lock_guard lock{_setsMutex};
                    replaced = _ready != kNone;
                    _ready = id;
                }
                std::lock_guard lock{_statsMutex};
                ++_stats.converted;
                _stats.convertedUnused += replaced ? 1 : 0;
            }
            if (previewWanted && last != nullptr)
            {
                makePreview(last);
            }
        }
    }

    void Engine::makePreview(std::uint8_t const* bgra)
    {
        convert::previewRgb(bgra, static_cast<std::size_t>(_settings.width) * 4, _settings.width, _settings.height, _previewWidth, _previewHeight,
            _previewScratch.data());
        std::lock_guard lock{_previewMutex};
        _previewRgb.swap(_previewScratch);
        _previewScratch.resize(_previewRgb.size());
        ++_previewSequence;
    }

    void Engine::tickLoop()
    {
        // The tick runs SCHED_FIFO when the container grants CAP_SYS_NICE (SPEC §2.2).
        sched_param param{};
        param.sched_priority = 10;
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0)
        {
            log::warn("tick_priority_normal", {{"reason", "SCHED_FIFO needs CAP_SYS_NICE"}});
        }
        Tick tick(*this);
        std::uint64_t next = _clock.indexAt(_clock.nowNs()) + 1;
        while (!_stop.load())
        {
            _clock.sleepUntil(_clock.indexNs(next) + _settings.marginNs);
            if (_stop.load())
            {
                break;
            }
            auto const now = _clock.indexAt(_clock.nowNs());
            if (now < next)
            {
                continue; // woke early
            }
            tick.run(now, _pageState.load());
            {
                std::lock_guard lock{_statsMutex};
                _stats.tick = tick.counters();
            }
            next = now + 1;
        }
    }

    bool Engine::commit(std::uint64_t index, PageState state)
    {
        bool fresh = false;
        int out = kNone;
        {
            std::lock_guard lock{_setsMutex};
            if (state == PageState::Ready)
            {
                if (_ready != kNone)
                {
                    _current = _ready;
                    _ready = kNone;
                    fresh = true;
                }
            }
            else if (_settings.onPageError == Substitute::Transparent)
            {
                _current = kTransparent;
            }
            else if (_settings.onPageError == Substitute::Black)
            {
                _current = kBlack;
            }
            // Hold keeps the last frame; a paint kept in _ready waits for the page to be ready.
            _ring[index % _ring.size()] = _current;
            out = _ring[(index + 1) % _ring.size()]; // what was current videoDelayGrains ticks ago
        }
        // `out` stays in the ring until the next tick, so the converter does not reuse it.
        auto const& frame = buffers(out);
        if (_videoEnabled.load())
        {
            _outputs.writeVideo(index, frame);
        }
        if (_settings.keyFlow && _keyEnabled.load())
        {
            _outputs.writeKey(index, frame);
        }
        auto const now = _clock.nowNs();
        auto const start = _clock.indexNs(index);
        _metrics.observe("commit_lateness_seconds", {}, now > start ? static_cast<double>(now - start) / 1e9 : 0.0);
        return fresh;
    }

    void Engine::writeAudio(std::uint64_t index)
    {
        if (!_resampler)
        {
            return;
        }
        auto const first = _clock.sampleAt(index);
        auto const end = _clock.sampleAt(index + 1);
        auto const frames = static_cast<std::size_t>(end - first);
        auto const channels = static_cast<std::size_t>(_settings.audioChannels);
        if (frames == 0 || frames * channels > _audioBuffer.size())
        {
            return;
        }
        int const offset = _avOffsetMs.load();
        if (offset != _appliedAvOffsetMs)
        {
            _appliedAvOffsetMs = offset;
            // A positive offset delays the audio: a larger FIFO target.
            auto const target = std::max<long long>(480, static_cast<long long>(_settings.audioTargetFrames) + 48LL * offset);
            _resampler->setTarget(*_fifo, static_cast<std::size_t>(target));
        }
        bool const active = _audioActive.load();
        float* out = _audioBuffer.data();
        if (active)
        {
            if (!_audioWasActive)
            {
                _resampler->reset(*_fifo); // a new stream starts from the target fill
            }
            _resampler->produce(*_fifo, out, frames, _clock.nowNs());
        }
        else
        {
            std::fill(out, out + frames * channels, 0.f); // the flow never stops (SPEC §6)
        }
        _audioWasActive = active;
        if (_audioEnabled.load())
        {
            _outputs.writeAudio(end, out, frames);
        }
        std::lock_guard lock{_statsMutex};
        _stats.audio = _resampler->stats();
        for (std::size_t c = 0; c < channels; ++c)
        {
            float peak = 0.f;
            for (std::size_t i = 0; i < frames; ++i)
            {
                peak = std::max(peak, std::fabs(out[i * channels + c]));
            }
            _stats.audioPeakDbfs[c] = peak > 1e-6f ? 20.0 * std::log10(static_cast<double>(peak)) : -120.0;
        }
    }

    void Engine::requestFrame()
    {
        if (_requestFrame)
        {
            _requestFrame();
        }
    }
}
