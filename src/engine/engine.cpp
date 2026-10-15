// SPDX-License-Identifier: MIT
#include "engine/engine.hpp"

#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <chrono>
#include <ctime>
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
        constexpr int kTickPriority = 10;   // SCHED_FIFO, as the gateway's media threads
        constexpr int kConvertPriority = 9; // below the tick: the tick must never wait for a conversion

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
        _settings.pipelineGrains = std::min<std::uint32_t>(_settings.pipelineGrains, 1);
        _pool.resize(_settings.videoDelayGrains + _settings.pipelineGrains + 3);
        _pending.reserve(_settings.pipelineGrains + 2);
        _poolBeginFrame.assign(_pool.size(), 0);
        _trace.resize(kTraceEvents);
        _audioTrace.resize(kAudioTraceEvents);
        _store.setClock([this] { return _clock.nowNs(); });
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

        if (_settings.internalFrameClock)
        {
            _frameClock = std::make_unique<FrameClock>(_settings.framePeriodNs, _settings.framePhaseNs, _settings.frameLockThresholdNs);
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

    void Engine::setAnchorFrameClock(std::function<void(std::uint64_t)> anchor)
    {
        _anchorFrameClock = std::move(anchor);
    }

    void Engine::frameClockAnchored(std::uint64_t targetNs, std::uint64_t calledNs)
    {
        if (!_frameClock)
        {
            return;
        }
        _frameClock->anchored(targetNs, calledNs, _clock.monotonicOffsetNs());
        record(TraceKind::Anchor, calledNs != 0 ? calledNs : _clock.nowNs(), targetNs, calledNs);
    }

    void Engine::relockFrameClock(LockReason reason)
    {
        if (_frameClock)
        {
            _frameClock->relock(reason);
        }
    }

    std::uint64_t Engine::internalBeginFrameNs(std::uint64_t nowNs) const
    {
        // The newest of Chromium's BeginFrames at or before `nowNs`: T(n) + φ.
        auto const phase = _settings.framePhaseNs;
        if (nowNs < phase)
        {
            return 0;
        }
        return _clock.indexNs(_clock.indexAt(nowNs - phase)) + phase;
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
        // External: the newest BeginFrame dispatched before the paint. Internal: the newest of
        // Chromium's own BeginFrames, which the frame clock keeps at T(n) + φ.
        auto const sent = _frameClock ? internalBeginFrameNs(now) : _dispatchedNs.load();
        if (!_store.paint(bgra, stride, width, height, FrameStore::Stamp{sent, now, 0}))
        {
            std::lock_guard lock{_statsMutex};
            ++_stats.paintsRejected;
            return;
        }
        auto const stored = _clock.nowNs();
        _metrics.observe("paint_copy_seconds", {}, static_cast<double>(stored - now) / 1e9);
        record(TraceKind::Paint, stored, ++_paints, now); // the store's sequence: both count accepted paints
        // A paint carries no frame id, and a BeginFrame on an unchanged page gets no paint: the
        // latency is taken from the newest BeginFrame dispatched before the paint (same thread).
        // Paints that came too late show up as paints never committed (stats().latePaints).
        if (sent != 0 && now >= sent)
        {
            _metrics.observe("paint_latency_seconds", {}, static_cast<double>(now - sent) / 1e9);
        }
        {
            std::lock_guard lock{_wakeMutex};
            _paintPending = true;
        }
        _wake.notify_one();
    }

    std::uint64_t Engine::beginFrameSent()
    {
        auto const now = _clock.nowNs();
        _lastBeginFrameNs.store(now);
        record(TraceKind::BeginFramePosted, now);
        return now;
    }

    void Engine::beginFrameDispatched(std::uint64_t postedNs)
    {
        auto const now = _clock.nowNs();
        if (postedNs != 0 && now >= postedNs)
        {
            _metrics.observe("begin_frame_dispatch_seconds", {}, static_cast<double>(now - postedNs) / 1e9);
        }
        _dispatchedNs.store(postedNs);
        record(TraceKind::BeginFrameSent, now, 0, postedNs);
    }

    void Engine::record(TraceKind kind, std::uint64_t ns, std::uint64_t a, std::uint64_t b)
    {
        std::lock_guard lock{_traceMutex};
        _trace[_traceNext] = TraceEvent{ns, kind, a, b};
        if (++_traceNext == _trace.size())
        {
            _traceNext = 0;
            _traceFull = true;
        }
    }

    void Engine::recordAudio(AudioTraceEvent const& event)
    {
        std::lock_guard lock{_audioTraceMutex};
        _audioTrace[_audioTraceNext] = event;
        if (++_audioTraceNext == _audioTrace.size())
        {
            _audioTraceNext = 0;
            _audioTraceFull = true;
        }
    }

    std::vector<AudioTraceEvent> Engine::audioTrace() const
    {
        std::lock_guard lock{_audioTraceMutex};
        std::vector<AudioTraceEvent> out;
        if (_audioTraceFull)
        {
            out.assign(_audioTrace.begin() + static_cast<std::ptrdiff_t>(_audioTraceNext), _audioTrace.end());
        }
        out.insert(out.end(), _audioTrace.begin(), _audioTrace.begin() + static_cast<std::ptrdiff_t>(_audioTraceNext));
        return out;
    }

    std::vector<TraceEvent> Engine::trace() const
    {
        std::lock_guard lock{_traceMutex};
        std::vector<TraceEvent> out;
        if (_traceFull)
        {
            out.assign(_trace.begin() + static_cast<std::ptrdiff_t>(_traceNext), _trace.end());
        }
        out.insert(out.end(), _trace.begin(), _trace.begin() + static_cast<std::ptrdiff_t>(_traceNext));
        return out;
    }

    void Engine::setPageState(PageState state)
    {
        _pageState.store(state);
    }

    void Engine::holdAudio(std::uint32_t ms)
    {
        _holdUntilNs.store(_clock.nowNs() + static_cast<std::uint64_t>(ms) * 1'000'000ULL);
    }

    void Engine::pushAudio(float const* const* planes, int planeCount, std::size_t frames, std::int64_t ptsMs)
    {
        if (!_fifo)
        {
            return;
        }
        if (auto const until = _holdUntilNs.load(std::memory_order_relaxed); until != 0)
        {
            if (_clock.nowNs() < until)
            {
                HeldPacket held{std::vector<float>(static_cast<std::size_t>(planeCount) * frames, 0.f), planeCount, frames, ptsMs};
                for (int c = 0; c < planeCount; ++c)
                {
                    if (planes[c] != nullptr)
                    {
                        std::copy(planes[c], planes[c] + frames, held.samples.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(c) * frames));
                    }
                }
                _held.push_back(std::move(held));
                return;
            }
            _holdUntilNs.store(0, std::memory_order_relaxed);
            for (auto const& held : _held)
            {
                std::vector<float const*> heldPlanes;
                for (int c = 0; c < held.planeCount; ++c)
                {
                    heldPlanes.push_back(held.samples.data() + static_cast<std::size_t>(c) * held.frames);
                }
                pushPacket(heldPlanes.data(), held.planeCount, held.frames, held.ptsMs);
            }
            _held.clear();
        }
        pushPacket(planes, planeCount, frames, ptsMs);
    }

    void Engine::pushPacket(float const* const* planes, int planeCount, std::size_t frames, std::int64_t ptsMs)
    {
        if (auto const stream = _audioStream.load(); stream != _audioStreamProducer)
        {
            _audioStreamProducer = stream;
            _gaps.reset();
        }
        std::size_t lost = 0;
        if (ptsMs >= 0)
        {
            // An upstream loss: exactly its length of silence goes first (SPEC §6).
            if (lost = _gaps.onPacket(ptsMs, frames); lost > 0)
            {
                _fifo->pushSilence(lost);
            }
            _audioGaps.store(_gaps.gaps(), std::memory_order_relaxed);
            _audioGapFrames.store(_gaps.gapFrames(), std::memory_order_relaxed);
            _audioGapsUnfilled.store(_gaps.unfilled(), std::memory_order_relaxed);
        }
        // Page channels beyond the flow's count are dropped, missing ones are silent.
        auto const now = _clock.nowNs();
        _fifo->push(planes, planeCount, frames, now);
        timespec mono{};
        clock_gettime(CLOCK_MONOTONIC, &mono);
        recordAudio({now, AudioTraceKind::Packet, ptsMs, static_cast<std::int64_t>(frames), static_cast<std::int64_t>(_fifo->available()),
                     static_cast<std::int64_t>(lost), static_cast<std::int64_t>(mono.tv_sec) * 1'000'000'000LL + mono.tv_nsec});
    }

    void Engine::audioStreamStarted()
    {
        _audioStream.fetch_add(1);
    }

    void Engine::setAudioActive(bool active, bool drain)
    {
        _audioDrain.store(!active && drain);
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

    bool Engine::preview(std::uint64_t ticket, std::vector<std::uint8_t>& rgb, std::uint64_t& sequence) const
    {
        std::lock_guard lock{_previewMutex};
        if (_previewSequence == 0 || _previewServed < ticket)
        {
            return false;
        }
        rgb = _previewRgb;
        sequence = _previewSequence;
        return true;
    }

    std::uint64_t Engine::requestPreview()
    {
        if (_previewWidth == 0)
        {
            return 0;
        }
        std::uint64_t ticket = 0;
        {
            std::lock_guard lock{_wakeMutex};
            _previewRequested = true;
            ticket = ++_previewTicket;
        }
        _wake.notify_one();
        return ticket;
    }

    std::uint64_t Engine::nextVisibleIndex() const
    {
        // The BeginFrame of the current grain may still draw it; its paint is committed at the
        // next tick and leaves the delay ring videoDelayGrains later.
        // Internal frame clock: Chromium's next BeginFrame is at T(k) + φ, the same place.
        auto const now = _clock.nowNs();
        auto const base = _frameClock && now >= _settings.framePhaseNs ? now - _settings.framePhaseNs : now;
        return _clock.indexAt(base) + 1 + _settings.pipelineGrains + _settings.videoDelayGrains;
    }

    EngineStats Engine::stats() const
    {
        std::lock_guard lock{_statsMutex};
        EngineStats out = _stats;
        out.paintsDropped = _store.dropped();
        out.latePaints = out.paintsDropped + out.convertedUnused;
        out.audioGaps = _audioGaps.load(std::memory_order_relaxed);
        out.audioGapFrames = _audioGapFrames.load(std::memory_order_relaxed);
        out.audioGapsUnfilled = _audioGapsUnfilled.load(std::memory_order_relaxed);
        out.internalFrameClock = _frameClock != nullptr;
        if (_frameClock)
        {
            out.frameClock = _frameClock->stats();
        }
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
            if (std::find(_pending.begin(), _pending.end(), id) == _pending.end() && std::find(_ring.begin(), _ring.end(), id) == _ring.end())
            {
                return id;
            }
        }
        return kNone; // cannot happen: the ring and _pending hold at most delay+pipeline+2 of delay+pipeline+3 sets
    }

    void Engine::converterLoop()
    {
        // The converter and its helpers run SCHED_FIFO just below the tick when RLIMIT_RTPRIO allows
        // it: a paint converted late is a held frame (docs/performance.md).
        int const priority = _settings.realtime ? kConvertPriority : 0;
        if (priority > 0)
        {
            sched_param param{};
            param.sched_priority = priority;
            pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
        }
        util::TaskPool converters(std::max(1U, _settings.convertThreads) - 1, priority); // the converter thread helps
        std::uint8_t const* last = nullptr; // the newest paint, valid until the next take()
        while (true)
        {
            bool previewWanted = false;
            std::uint64_t ticket = 0; // the newest preview request; the frame is taken after it
            {
                std::unique_lock lock{_wakeMutex};
                _wake.wait(lock, [this] { return _paintPending || _previewRequested || _stopping; });
                if (_stopping)
                {
                    return;
                }
                _paintPending = false;
                previewWanted = std::exchange(_previewRequested, false);
                ticket = _previewTicket;
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
                auto const start = _clock.nowNs();
                record(TraceKind::ConvertStart, start, frame.sequence, frame.stamp.beginFrameNs);
                if (frame.stamp.storedNs != 0 && start >= frame.stamp.storedNs)
                {
                    _metrics.observe("convert_wait_seconds", {}, static_cast<double>(start - frame.stamp.storedNs) / 1e9);
                }
                auto const t0 = std::chrono::steady_clock::now();
                convertInto(_pool[static_cast<std::size_t>(id)], _settings, frame.bgra, &converters);
                _metrics.observe("convert_seconds", {}, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
                auto const end = _clock.nowNs();
                record(TraceKind::ConvertEnd, end, frame.sequence, static_cast<std::uint64_t>(id));
                if (frame.stamp.beginFrameNs != 0 && end >= frame.stamp.beginFrameNs)
                {
                    _metrics.observe("frame_ready_seconds", {}, static_cast<double>(end - frame.stamp.beginFrameNs) / 1e9);
                }
                bool replaced = false;
                {
                    std::lock_guard lock{_setsMutex};
                    _poolBeginFrame[static_cast<std::size_t>(id)] = frame.stamp.beginFrameNs;
                    if (_pending.size() > _settings.pipelineGrains)
                    {
                        _pending.erase(_pending.begin()); // the oldest waiting frame is never shown (late)
                        replaced = true;
                    }
                    _pending.push_back(id);
                }
                std::lock_guard lock{_statsMutex};
                ++_stats.converted;
                _stats.convertedUnused += replaced ? 1 : 0;
            }
            if (previewWanted && last != nullptr)
            {
                makePreview(last, ticket);
            }
        }
    }

    void Engine::makePreview(std::uint8_t const* bgra, std::uint64_t ticket)
    {
        convert::previewRgb(bgra, static_cast<std::size_t>(_settings.width) * 4, _settings.width, _settings.height, _previewWidth, _previewHeight,
            _previewScratch.data());
        std::lock_guard lock{_previewMutex};
        _previewRgb.swap(_previewScratch);
        _previewScratch.resize(_previewRgb.size());
        ++_previewSequence;
        _previewServed = ticket;
    }

    void Engine::tickLoop()
    {
        // The tick runs SCHED_FIFO when the container allows it (SPEC §2.2): RLIMIT_RTPRIO >= 10
        // (docker --ulimit rtprio=10) for the non-root user, or an effective CAP_SYS_NICE.
        sched_param param{};
        param.sched_priority = kTickPriority;
        if (!_settings.realtime)
        {
            log::info("tick_priority_normal", {{"reason", "BROWSER_REALTIME=false"}});
        }
        else if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0)
        {
            log::warn("tick_priority_normal", {{"reason", "SCHED_FIFO needs RLIMIT_RTPRIO >= 10 or CAP_SYS_NICE"}});
        }
        Tick tick(*this);
        tick.sendBeginFrames(!_frameClock); // internal: Chromium paces itself
        std::uint64_t next = _clock.indexAt(_clock.nowNs()) + 1;
        while (!_stop.load())
        {
            _clock.sleepUntil(_clock.indexNs(next) + _settings.marginNs);
            if (_stop.load())
            {
                break;
            }
            auto const wokeNs = _clock.nowNs();
            auto const now = _clock.indexAt(wokeNs);
            if (now < next)
            {
                continue; // woke early
            }
            record(TraceKind::Tick, wokeNs, now);
            tick.run(now, _pageState.load());
            {
                std::lock_guard lock{_statsMutex};
                _stats.tick = tick.counters();
            }
            if (_frameClock && _anchorFrameClock)
            {
                if (auto const target = _frameClock->poll(_clock.indexNs(now + 1), _clock.monotonicOffsetNs(), _clock.nowNs()); target != 0)
                {
                    _anchorFrameClock(target);
                }
            }
            next = now + 1;
        }
    }

    bool Engine::commit(std::uint64_t index, PageState state)
    {
        bool fresh = false;
        int out = kNone;
        std::uint64_t beginFrameNs = 0;
        {
            std::lock_guard lock{_setsMutex};
            if (state == PageState::Ready && !_pending.empty())
            {
                // The oldest converted frame (SPEC §2.4). Without the pipeline at most one waits,
                // the newest. With it up to two wait: a paint that missed its tick is shown at the
                // next one instead of being replaced, and the frame behind it waits one period.
                _current = _pending.front();
                _pending.erase(_pending.begin());
                fresh = true;
                beginFrameNs = _poolBeginFrame[static_cast<std::size_t>(_current)];
            }
            else if (_settings.onPageError == Substitute::Transparent)
            {
                _current = kTransparent;
            }
            else if (_settings.onPageError == Substitute::Black)
            {
                _current = kBlack;
            }
            // Hold keeps the last frame; converted paints wait in _pending for the page to be ready.
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
        record(TraceKind::Commit, now, index, fresh ? std::max<std::uint64_t>(1, beginFrameNs) : 0);
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
        auto const stream = _audioStream.load();
        float* out = _audioBuffer.data();
        if (active)
        {
            if (!_audioWasActive || stream != _audioStreamSeen)
            {
                _resampler->start(); // a stream starts at the target fill (its first sample is not cut)
            }
            _audioDraining = false;
            auto const now = _clock.nowNs();
            auto const before = _resampler->fill(*_fifo);
            _resampler->produce(*_fifo, out, frames, now);
            auto const& st = _resampler->stats();
            recordAudio({now, AudioTraceKind::Tick, static_cast<std::int64_t>(before), static_cast<std::int64_t>(_resampler->target()),
                         static_cast<std::int64_t>(st.underrunFrames), static_cast<std::int64_t>(st.realigned),
                         static_cast<std::int64_t>(_resampler->fill(*_fifo))});
        }
        else
        {
            if (_audioWasActive)
            {
                _audioDraining = _audioDrain.load(); // the stream ended: play out what the FIFO holds
            }
            if (_audioDraining)
            {
                _audioDraining = _resampler->drain(*_fifo, out, frames);
            }
            else
            {
                std::fill(out, out + frames * channels, 0.f); // the flow never stops (SPEC §6)
            }
        }
        _audioWasActive = active;
        _audioStreamSeen = stream;
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
