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
#include "audio/resampler.hpp"
#include "engine/framestore.hpp"
#include "engine/tick.hpp"
#include "util/metrics.hpp"

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
        std::uint32_t videoDelayGrains = 0;
        std::uint64_t marginNs = 1'000'000; // wake this long after the grain start (ε)
        Substitute onPageError = Substitute::Hold;
        std::uint32_t previewWidth = 960; // 0: no preview
        unsigned convertThreads = 4;      // bands of rows converted in parallel (latency, not CPU)
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
        std::vector<double> audioPeakDbfs; // per flow channel, last grain
    };

    class Engine final : TickActions
    {
    public:
        Engine(EngineSettings settings, Clock& clock, Outputs& outputs, util::Metrics& metrics);
        ~Engine() override;

        Engine(Engine const&) = delete;
        Engine& operator=(Engine const&) = delete;

        /// Posts one BeginFrame to the UI thread. Set before start().
        void setRequestFrame(std::function<void()> request);
        /// Starts the converter and the tick thread.
        void start();
        /// Stops both after the current grain. Idempotent.
        void stop();

        /// UI thread (OnPaint): copies the paint; the converter takes it.
        void paint(std::uint8_t const* bgra, std::size_t stride, std::uint32_t width, std::uint32_t height);
        /// UI thread: a BeginFrame was sent (paint latency is measured from here).
        void beginFrameSent();
        void setPageState(PageState state);
        [[nodiscard]] PageState pageState() const { return _pageState.load(); }

        /// CEF's audio thread: one packet of planar float samples.
        void pushAudio(float const* const* planes, int planeCount, std::size_t frames);
        /// No page audio (no stream, or the source has `audio: false`): the flow carries silence.
        void setAudioActive(bool active);
        void setAvOffsetMs(int offsetMs);

        void setEnabled(Flow flow, bool enabled);
        [[nodiscard]] bool enabled(Flow flow) const;

        /// The newest preview (RGB24, previewWidth() × previewHeight()); false when there is none
        /// newer than `sequence`, which is updated otherwise.
        bool preview(std::vector<std::uint8_t>& rgb, std::uint64_t& sequence) const;
        /// Asks the converter for a preview of the newest frame, also when the page does not paint.
        void requestPreview();
        [[nodiscard]] std::uint32_t previewWidth() const { return _previewWidth; }
        [[nodiscard]] std::uint32_t previewHeight() const { return _previewHeight; }

        /// The first grain index that can show something the page does now.
        [[nodiscard]] std::uint64_t nextVisibleIndex() const;

        [[nodiscard]] EngineStats stats() const;
        [[nodiscard]] EngineSettings const& settings() const { return _settings; }

    private:
        static constexpr int kNone = -1;
        static constexpr int kTransparent = -2;
        static constexpr int kBlack = -3;

        // TickActions, on the tick thread.
        bool commit(std::uint64_t index, PageState state) override;
        void writeAudio(std::uint64_t index) override;
        void requestFrame() override;

        void converterLoop();
        void tickLoop();
        GrainBuffers const& buffers(int id) const;
        int freeSetLocked() const;
        void makePreview(std::uint8_t const* bgra);

        EngineSettings _settings;
        Clock& _clock;
        Outputs& _outputs;
        util::Metrics& _metrics;
        std::function<void()> _requestFrame;

        FrameStore _store;

        // Grain buffer sets: the pool, the transparent and black frames, and who holds which.
        // The delay ring holds the set that was current at each of the last delay+1 ticks; the
        // tick writes the oldest. A set is free when neither the ring nor `_ready` holds it.
        std::vector<GrainBuffers> _pool;
        GrainBuffers _transparent;
        GrainBuffers _black;
        mutable std::mutex _setsMutex;
        int _ready = kNone;
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
        bool _stopping = false;
        std::atomic<bool> _stop{false};

        // When the newest BeginFrame was sent (TAI ns), for the paint latency.
        std::atomic<std::uint64_t> _lastBeginFrameNs{0};

        // Audio, on the tick thread (the FIFO's producer is CEF's audio thread).
        std::unique_ptr<audio::Fifo> _fifo;
        std::unique_ptr<audio::Resampler> _resampler;
        std::vector<float> _audioBuffer;
        std::atomic<bool> _audioActive{false};
        bool _audioWasActive = false;
        std::atomic<int> _avOffsetMs{0};
        int _appliedAvOffsetMs = 0;

        // Preview.
        std::uint32_t _previewWidth = 0;
        std::uint32_t _previewHeight = 0;
        mutable std::mutex _previewMutex;
        std::vector<std::uint8_t> _previewRgb;
        std::uint64_t _previewSequence = 0;
        std::vector<std::uint8_t> _previewScratch;

        mutable std::mutex _statsMutex;
        EngineStats _stats;

        std::thread _converter;
        std::thread _ticker;
        bool _started = false;
    };
}
