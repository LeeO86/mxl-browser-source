// SPDX-License-Identifier: MIT
// The media engine (SPEC §2.2, §2.4, §5, §6). OnPaint copies into the frame store; the
// converter thread turns the newest paint into grain buffers (v210 fill, v210a alpha, key);
// the tick thread commits them, or repeats the last frame, once per grain index, writes the
// grain's audio through the drift-controlled resampler, and asks the page for the next frame.
// No CEF and no libmxl here: the clock, the flows and the BeginFrame request are interfaces,
// so the unit tests drive it with a fake clock.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "audio/fifo.hpp"
#include "audio/gaps.hpp"
#include "audio/resampler.hpp"
#include "engine/frameclock.hpp"
#include "engine/framestore.hpp"
#include "engine/tick.hpp"
#include "util/metrics.hpp"
#include "util/tracering.hpp"

namespace mbs::engine
{
    /// House time (TAI). The process implements it over MXL's time functions.
    class Clock
    {
    public:
        virtual ~Clock() = default;
        [[nodiscard]] virtual std::uint64_t nowNs() const = 0;
        [[nodiscard]] virtual std::uint64_t indexAt(std::uint64_t taiNs) const = 0;  // grain index at a TAI time
        [[nodiscard]] virtual std::uint64_t indexNs(std::uint64_t index) const = 0;  // TAI start of a grain index
        [[nodiscard]] virtual std::uint64_t sampleAt(std::uint64_t index) const = 0; // 48 kHz sample index at that start
        virtual void sleepUntil(std::uint64_t taiNs) = 0;
        /// TAI − CLOCK_MONOTONIC (Chromium's clock) now; the internal frame clock follows it.
        [[nodiscard]] virtual std::int64_t monotonicOffsetNs() const { return 0; }
    };

    /// The converted buffers of one frame (SPEC §5.3).
    struct GrainBuffers
    {
        std::vector<std::uint8_t> fill;  // v210
        std::vector<std::uint8_t> alpha; // v210a alpha plane, empty unless key mode v210a
        std::vector<std::uint8_t> key;   // key as v210 luma, empty unless key mode fill_key
    };

    /// The MXL flows (MXL writers in the process, a recorder in the tests). Called on the tick thread.
    class Outputs
    {
    public:
        virtual ~Outputs() = default;
        /// The video flow: fill, followed by the alpha plane in v210a.
        virtual void writeVideo(std::uint64_t index, GrainBuffers const& frame) = 0;
        /// The key flow (fill_key), same index as the fill.
        virtual void writeKey(std::uint64_t index, GrainBuffers const& frame) = 0;
        /// `frames` interleaved samples ending at sample index `endSample`.
        virtual void writeAudio(std::uint64_t endSample, float const* interleaved, std::size_t frames) = 0;
    };

    enum class Flow
    {
        Video,
        Key,
        Audio
    };

    /// What the output shows while the page is not ready (BROWSER_ON_PAGE_ERROR; `slate` is the
    /// app's error page, which paints like any page, and black while the renderer is gone).
    enum class Substitute
    {
        Hold,
        Transparent,
        Black
    };

    struct EngineSettings
    {
        std::uint32_t width = 1920;
        std::uint32_t height = 1080;
        bool alphaPlane = false;          // key mode v210a
        bool keyFlow = false;             // key mode fill_key
        bool straightFill = true;         // un-premultiply the fill (only with a key)
        std::uint32_t audioChannels = 2;  // 0: no audio flow
        std::uint32_t audioTargetFrames = 2880; // FIFO target (60 ms)
        std::uint32_t audioMaxTargetFrames = 0; // > audioTargetFrames: the target rises after underruns up to this (BROWSER_AUDIO_BUFFER_MAX_MS)
        std::uint32_t videoDelayGrains = 0;
        std::uint32_t videoDelayMaxGrains = 0;  // the video delay follows the audio target up to this (ring and pool sized for it)
        std::uint32_t pipelineGrains = 0; // 1: up to two converted frames wait and the oldest is committed, so a late paint is shown one tick later instead of dropped (BROWSER_FRAME_PIPELINE)
        std::uint64_t marginNs = 1'000'000; // wake this long after the grain start (ε)
        Substitute onPageError = Substitute::Hold;
        std::uint32_t previewWidth = 960; // 0: no preview
        unsigned convertThreads = 4;      // bands of rows converted in parallel (latency, not CPU)
        bool realtime = true;             // tick and converter threads SCHED_FIFO when RLIMIT_RTPRIO (or CAP_SYS_NICE) allows
        // BROWSER_FRAME_CLOCK=internal: Chromium's own BeginFrames at the house rate, phase-locked
        // to T(k) + framePhaseNs; the tick sends none. `framePeriodNs` is Chromium's period.
        bool internalFrameClock = false;
        std::uint64_t framePeriodNs = 20'000'000;
        std::uint64_t framePhaseNs = 0;
        std::uint64_t frameLockThresholdNs = 1'000'000;
    };

    /// One event of the frame-loop trace (docs/performance.md, "Where the time goes"). `ns` is TAI.
    enum class TraceKind : std::uint8_t
    {
        Tick,             // a: the grain index the tick thread woke for
        Commit,           // a: grain index, b: the BeginFrame time of the frame written (0: a repeat)
        BeginFramePosted, // tick thread posted the BeginFrame
        BeginFrameSent,   // UI thread called SendExternalBeginFrame
        Paint,            // OnPaint copied; a: paint sequence, b: OnPaint entry time
        ConvertStart,     // a: paint sequence, b: its BeginFrame time
        ConvertEnd,       // a: paint sequence, b: buffer set
        Anchor,           // internal frame clock anchored; a: target time, b: call time (0: too late, not anchored)
    };

    struct TraceEvent
    {
        std::uint64_t ns = 0;
        TraceKind kind = TraceKind::Tick;
        std::uint64_t a = 0;
        std::uint64_t b = 0;
    };

    /// One event of the audio trace (docs/api.md, /api/v1/debug/audio-trace). `ns` is TAI.
    enum class AudioTraceKind : std::uint8_t
    {
        Packet, // OnAudioStreamPacket; a: pts (ms, wall time as CEF 144 gives it: TAI − pts ≈ 37 s + latency), b: frames, c: FIFO frames after it, d: silence put before it (gap fill), e: arrival (CLOCK_MONOTONIC ns)
        Tick,   // the tick wrote a grain; a: FIFO frames before it, b: target frames, c: underrun frames (total), d: frames dropped as late (total), e: FIFO frames after it
    };

    struct AudioTraceEvent
    {
        std::uint64_t ns = 0;
        AudioTraceKind kind = AudioTraceKind::Packet;
        std::int64_t a = 0;
        std::int64_t b = 0;
        std::int64_t c = 0;
        std::int64_t d = 0;
        std::int64_t e = 0;
    };

    struct EngineStats
    {
        TickCounters tick;
        std::uint64_t converted = 0;       // paints converted into grain buffers
        std::uint64_t convertedUnused = 0; // replaced by a newer conversion before a tick took them
        std::uint64_t paintsDropped = 0;   // replaced in the frame store before conversion
        std::uint64_t paintsRejected = 0;  // wrong size (a resize in flight)
        std::uint64_t latePaints = 0;      // paints never committed: a newer one arrived before the tick (late or superseded)
        audio::ResamplerStats audio;
        std::uint64_t audioGaps = 0;         // upstream losses found in the packet times
        std::uint64_t audioGapFrames = 0;    // silence put in their place
        std::uint64_t audioGapsUnfilled = 0; // jumps too long to fill (left to the FIFO)
        std::uint64_t audioFifoOverflows = 0;      // packets that did not fit into the FIFO (cannot happen at its size)
        std::uint64_t audioFifoOverflowFrames = 0; // frames dropped for them
        std::size_t audioFifoFrames = 0;           // FIFO capacity
        std::vector<double> audioPeakDbfs; // per flow channel, last grain
        bool internalFrameClock = false;
        FrameClockStats frameClock;
        std::uint32_t videoDelayGrains = 0; // now (the base plus what the adaptive audio target added)
    };

    /// BROWSER_AV_OFFSET_MS is at most 500 ms (config), in frames: it adds to the FIFO target.
    inline constexpr std::uint32_t kMaxAvOffsetFrames = 500 * 48;

    /// The video delay for what the adaptive audio target adds: whole grains of the nominal grain
    /// length `grainFrames` (48000·den/num), rounded.
    std::uint32_t videoDelayExtraGrains(double extraFrames, double grainFrames);

    /// The page audio FIFO's capacity: room for the largest target up to the overrun cut.
    std::size_t audioFifoFrames(EngineSettings const& settings);

    class Engine final : TickActions
    {
    public:
        Engine(EngineSettings settings, Clock& clock, Outputs& outputs, util::Metrics& metrics);
        ~Engine() override;

        Engine(Engine const&) = delete;
        Engine& operator=(Engine const&) = delete;

        /// Posts one BeginFrame to the UI thread. Set before start().
        void setRequestFrame(std::function<void()> request);
        /// Internal frame clock: asks the UI thread to call SetWindowlessFrameRate at TAI `targetNs`
        /// and to report with frameClockAnchored(). Set before start().
        void setAnchorFrameClock(std::function<void(std::uint64_t targetNs)> anchor);
        /// UI thread: the anchor for `targetNs` ran at TAI `calledNs` (0: too late, not run).
        void frameClockAnchored(std::uint64_t targetNs, std::uint64_t calledNs);
        /// The anchor could not run because there is no browser yet: not counted as late, asked again.
        void frameClockSkipped();
        /// A new compositor timebase may have come with a new renderer view: anchor again.
        void relockFrameClock(LockReason reason);
        /// Starts the converter and the tick thread.
        void start();
        /// Stops both after the current grain. Idempotent.
        void stop();

        /// UI thread (OnPaint): copies the paint; the converter takes it.
        void paint(std::uint8_t const* bgra, std::size_t stride, std::uint32_t width, std::uint32_t height);
        /// Tick thread: a BeginFrame was posted to the UI thread. Returns its time (TAI ns), the
        /// BeginFrame's id for beginFrameDispatched().
        std::uint64_t beginFrameSent();
        /// UI thread: the BeginFrame posted at `postedNs` reaches CEF now (SendExternalBeginFrame).
        /// A paint belongs to the newest BeginFrame dispatched before it: both run on the UI thread,
        /// so a paint that arrives while a newer BeginFrame waits in the queue is not counted as its.
        void beginFrameDispatched(std::uint64_t postedNs);
        void setPageState(PageState state);
        [[nodiscard]] PageState pageState() const { return _pageState.load(); }

        /// CEF's audio thread: one packet of planar float samples, captured at `ptsMs` (CLOCK_MONOTONIC
        /// in ms; < 0: unknown, no gap detection).
        void pushAudio(float const* const* planes, int planeCount, std::size_t frames, std::int64_t ptsMs = -1);
        /// Diagnosis (POST /api/v1/debug/audio-hold): packets arriving in the next `ms` are held
        /// and then delivered at once with their own times, as Chromium's late bursts do.
        void holdAudio(std::uint32_t ms);
        /// A new audio stream begins (its packet times start a new timeline).
        void audioStreamStarted();
        /// No page audio (no stream, or the source has `audio: false`): the flow carries silence.
        /// `drain`: the stream ended, so what the FIFO still holds is played out first.
        void setAudioActive(bool active, bool drain = false);
        void setAvOffsetMs(int offsetMs);

        void setEnabled(Flow flow, bool enabled);
        [[nodiscard]] bool enabled(Flow flow) const;

        /// Asks the converter for a preview of the newest frame, also when the page does not paint.
        /// Returns the request's ticket (0 when there is no preview).
        std::uint64_t requestPreview();
        /// The preview made for request `ticket` or a later one (RGB24, previewWidth() ×
        /// previewHeight()); false while there is none. A preview made for an earlier request may
        /// show an older frame. `sequence` is the preview's own number (for an encoding cache).
        bool preview(std::uint64_t ticket, std::vector<std::uint8_t>& rgb, std::uint64_t& sequence) const;
        [[nodiscard]] std::uint32_t previewWidth() const { return _previewWidth; }
        [[nodiscard]] std::uint32_t previewHeight() const { return _previewHeight; }

        /// The first grain index that can show something the page does now.
        [[nodiscard]] std::uint64_t nextVisibleIndex() const;

        [[nodiscard]] EngineStats stats() const;
        /// The newest frame-loop events (about the last 40 s at 50 Hz), oldest first.
        [[nodiscard]] std::vector<TraceEvent> trace() const;
        /// The newest audio packets and grains (about the last 100 s), oldest first.
        [[nodiscard]] std::vector<AudioTraceEvent> audioTrace() const;
        [[nodiscard]] EngineSettings const& settings() const { return _settings; }

    private:
        static constexpr int kNone = -1;
        static constexpr int kTransparent = -2;
        static constexpr int kBlack = -3;

        // TickActions, on the tick thread.
        bool commit(std::uint64_t index, PageState state) override;
        void writeAudio(std::uint64_t index) override;
        void requestFrame() override;
        [[nodiscard]] std::uint64_t internalBeginFrameNs(std::uint64_t nowNs) const;

        void converterLoop();
        void tickLoop();
        GrainBuffers const& buffers(int id) const;
        int freeSetLocked() const;
        void makePreview(std::uint8_t const* bgra, std::uint64_t ticket);
        void record(TraceKind kind, std::uint64_t ns, std::uint64_t a = 0, std::uint64_t b = 0);
        void recordAudio(AudioTraceEvent const& event);

        EngineSettings _settings;
        Clock& _clock;
        Outputs& _outputs;
        util::Metrics& _metrics;
        std::function<void()> _requestFrame;

        FrameStore _store;

        // Grain buffer sets: the pool, the transparent and black frames, and who holds which.
        // The delay ring holds the set that was current at each of the last delay+1 ticks; the
        // tick writes the oldest. Converted sets wait in `_pending` (oldest first, at most
        // pipeline+1). A set is free when neither the ring nor `_pending` holds it.
        std::vector<GrainBuffers> _pool;
        std::vector<std::uint64_t> _poolBeginFrame; // the BeginFrame time of each set's paint
        GrainBuffers _transparent;
        GrainBuffers _black;
        mutable std::mutex _setsMutex;
        std::vector<int> _pending;                 // capacity reserved: no allocation per frame
        int _current = kTransparent;
        std::vector<int> _ring;

        std::atomic<PageState> _pageState{PageState::Loading};
        std::atomic<bool> _videoEnabled{true};
        std::atomic<bool> _keyEnabled{true};
        std::atomic<bool> _audioEnabled{true};

        // Converter wake-up.
        std::mutex _wakeMutex;
        std::condition_variable _wake;
        bool _paintPending = false;
        bool _previewRequested = false;
        std::uint64_t _previewTicket = 0; // the newest request
        bool _stopping = false;
        std::atomic<bool> _stop{false};

        // When the newest BeginFrame was sent (TAI ns), for the paint latency.
        std::atomic<std::uint64_t> _lastBeginFrameNs{0};
        std::uint64_t _paints = 0;       // UI thread: paints accepted by the store (its sequence)
        std::atomic<std::uint64_t> _dispatchedNs{0}; // the posted time of the newest BeginFrame sent (UI thread)

        // Audio, on the tick thread (the FIFO's producer is CEF's audio thread).
        std::unique_ptr<audio::Fifo> _fifo;
        std::unique_ptr<audio::Resampler> _resampler;
        std::vector<float> _audioBuffer;
        std::atomic<bool> _audioActive{false};
        std::atomic<bool> _audioDrain{false};
        std::atomic<std::uint64_t> _audioStream{0}; // stream generation, counted up at each start
        bool _audioWasActive = false;
        bool _audioDraining = false;
        std::uint64_t _audioStreamSeen = 0;         // tick thread
        std::uint64_t _audioStreamProducer = 0;     // audio thread
        audio::GapDetector _gaps;                   // audio thread
        void pushPacket(float const* const* planes, int planeCount, std::size_t frames, std::int64_t ptsMs);
        struct HeldPacket
        {
            std::vector<float> samples; // planeCount × frames, planar
            int planeCount = 0;
            std::size_t frames = 0;
            std::int64_t ptsMs = -1;
        };
        std::atomic<std::uint64_t> _holdUntilNs{0};
        std::vector<HeldPacket> _held;              // audio thread (diagnosis only)
        std::atomic<std::uint64_t> _audioGaps{0};
        std::atomic<std::uint64_t> _audioGapFrames{0};
        std::atomic<std::uint64_t> _audioGapsUnfilled{0};
        std::atomic<bool> _audioLost{false}; // audio thread → tick: a pts jump too long to fill
        std::atomic<std::uint64_t> _audioOverflows{0};
        std::atomic<std::uint64_t> _audioOverflowFrames{0};

        // Internal frame clock.
        std::unique_ptr<FrameClock> _frameClock;
        std::function<void(std::uint64_t)> _anchorFrameClock;
        std::atomic<int> _avOffsetMs{0};
        int _appliedAvOffsetMs = 0;
        // The video delay follows the adaptive audio target (tick thread): writeAudio sets the
        // wanted delay, commit moves towards it by one grain per tick (a repeat or a skip).
        std::uint32_t _videoDelayWanted = 0;
        std::uint32_t _videoDelay = 0;
        static constexpr std::uint32_t kDelayDecreaseTicks = 25; // 0.5 s at 50p, 1 s at 25p
        std::uint32_t _ticksSinceDecrease = kDelayDecreaseTicks;
        std::atomic<std::uint32_t> _videoDelayNow{0};

        // Preview.
        std::uint32_t _previewWidth = 0;
        std::uint32_t _previewHeight = 0;
        mutable std::mutex _previewMutex;
        std::vector<std::uint8_t> _previewRgb;
        std::uint64_t _previewSequence = 0;
        std::uint64_t _previewServed = 0; // the request ticket the newest preview answered
        std::vector<std::uint8_t> _previewScratch;

        // Frame-loop and audio traces (packets about 100/s, and grains): lock-free rings, so a
        // reader never holds up the tick or CEF's threads.
        static constexpr std::size_t kTraceEvents = 12288;
        util::TraceRing<TraceEvent> _trace{kTraceEvents};
        static constexpr std::size_t kAudioTraceEvents = 16384;
        util::TraceRing<AudioTraceEvent> _audioTrace{kAudioTraceEvents};

        mutable std::mutex _statsMutex;
        EngineStats _stats;

        std::thread _converter;
        std::thread _ticker;
        bool _started = false;
    };
}
