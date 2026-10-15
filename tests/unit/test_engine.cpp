// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "convert/convert.hpp"
#include "engine/engine.hpp"

using namespace mbs;
using engine::Engine;
using engine::EngineSettings;
using engine::GrainBuffers;
using engine::PageState;

namespace
{
    constexpr std::uint64_t kPeriod = 20'000'000; // 50 Hz
    constexpr std::uint64_t kMargin = 1'000'000;

    // Time moves only when the test says so; the tick thread blocks in sleepUntil() until then,
    // and the test can wait until the thread sleeps again (its tick is done).
    class ManualClock : public engine::Clock
    {
    public:
        explicit ManualClock(std::uint64_t now)
            : _now(now)
        {
        }
        std::uint64_t nowNs() const override
        {
            std::lock_guard lock{_mutex};
            return _now;
        }
        std::uint64_t indexAt(std::uint64_t taiNs) const override { return taiNs / kPeriod; }
        std::uint64_t indexNs(std::uint64_t index) const override { return index * kPeriod; }
        std::uint64_t sampleAt(std::uint64_t index) const override { return index * 960; }
        void sleepUntil(std::uint64_t taiNs) override
        {
            std::unique_lock lock{_mutex};
            _sleeper = taiNs;
            _changed.notify_all();
            _changed.wait(lock, [&] { return _now >= taiNs || _released; });
            _sleeper = 0;
        }

        /// Moves to the start of grain `index` (plus the margin) and waits until the tick for it ran.
        void tick(std::uint64_t index)
        {
            {
                std::unique_lock lock{_mutex};
                REQUIRE(_changed.wait_for(lock, std::chrono::seconds(5), [&] { return _sleeper != 0; }));
                _now = index * kPeriod + kMargin;
                _changed.notify_all();
            }
            std::unique_lock lock{_mutex};
            REQUIRE(_changed.wait_for(lock, std::chrono::seconds(5), [&] { return _sleeper > _now; }));
        }
        /// Moves the time without waking the tick (paints and audio between ticks).
        void set(std::uint64_t ns)
        {
            std::lock_guard lock{_mutex};
            _now = ns;
        }
        void release()
        {
            std::lock_guard lock{_mutex};
            _released = true;
            _changed.notify_all();
        }

    private:
        mutable std::mutex _mutex;
        std::condition_variable _changed;
        std::uint64_t _now;
        std::uint64_t _sleeper = 0;
        bool _released = false;
    };

    struct Recorder : engine::Outputs
    {
        struct Grain
        {
            std::uint64_t index;
            std::vector<std::uint8_t> fill;
            std::vector<std::uint8_t> alpha;
        };
        std::mutex mutex;
        std::vector<Grain> video;
        std::vector<std::uint64_t> key;
        std::vector<std::pair<std::uint64_t, std::vector<float>>> audio;

        void writeVideo(std::uint64_t index, GrainBuffers const& frame) override
        {
            std::lock_guard lock{mutex};
            video.push_back({index, frame.fill, frame.alpha});
        }
        void writeKey(std::uint64_t index, GrainBuffers const&) override
        {
            std::lock_guard lock{mutex};
            key.push_back(index);
        }
        void writeAudio(std::uint64_t endSample, float const* interleaved, std::size_t frames) override
        {
            std::lock_guard lock{mutex};
            audio.emplace_back(endSample, std::vector<float>(interleaved, interleaved + frames * 2));
        }
    };

    constexpr std::uint32_t kWidth = 48;
    constexpr std::uint32_t kHeight = 4;

    std::vector<std::uint8_t> solid(std::uint8_t b, std::uint8_t g, std::uint8_t r, std::uint8_t a)
    {
        std::vector<std::uint8_t> bgra(static_cast<std::size_t>(kWidth) * kHeight * 4);
        for (std::size_t i = 0; i < bgra.size(); i += 4)
        {
            bgra[i] = b;
            bgra[i + 1] = g;
            bgra[i + 2] = r;
            bgra[i + 3] = a;
        }
        return bgra;
    }

    std::vector<std::uint8_t> fillOf(std::vector<std::uint8_t> const& bgra, bool straight)
    {
        std::vector<std::uint8_t> fill(static_cast<std::size_t>(convert::v210RowBytes(kWidth)) * kHeight);
        std::vector<std::uint8_t> alpha(static_cast<std::size_t>(convert::alphaRowBytes(kWidth)) * kHeight);
        convert::Request request;
        request.bgra = bgra.data();
        request.bgraStride = kWidth * 4;
        request.width = kWidth;
        request.height = kHeight;
        request.straight = straight;
        request.fill = fill.data();
        request.alpha = alpha.data();
        convert::convertFrame(request);
        return fill;
    }

    void waitConverted(Engine const& engine, std::uint64_t count)
    {
        for (int i = 0; i < 500 && engine.stats().converted < count; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        REQUIRE(engine.stats().converted >= count);
    }

    EngineSettings small()
    {
        EngineSettings s;
        s.width = kWidth;
        s.height = kHeight;
        s.alphaPlane = true;
        s.audioChannels = 2;
        s.marginNs = kMargin;
        s.previewWidth = 24;
        return s;
    }
}

TEST_CASE("the engine commits each paint once, repeats otherwise, and fills missed grains")
{
    ManualClock clock(1000 * kPeriod);
    Recorder outputs;
    util::Metrics metrics("test_");
    Engine engine(small(), clock, outputs, metrics);
    int requests = 0;
    engine.setRequestFrame([&] { ++requests; });
    engine.setPageState(PageState::Ready);
    engine.start();

    auto const red = solid(0, 0, 255, 255);
    engine.paint(red.data(), kWidth * 4, kWidth, kHeight);
    waitConverted(engine, 1);
    clock.tick(1001);
    clock.tick(1002); // no paint: a repeat
    clock.tick(1005); // woke late: 1003 and 1004 are filled
    clock.release();
    engine.stop();

    REQUIRE(outputs.video.size() == 5);
    for (std::size_t i = 0; i < outputs.video.size(); ++i)
    {
        CHECK(outputs.video[i].index == 1001 + i);
        CHECK(outputs.video[i].fill == fillOf(red, true));
    }
    auto const stats = engine.stats();
    CHECK(stats.tick.grains == 5);
    CHECK(stats.tick.repeated == 2);
    CHECK(stats.tick.missed == 2);
    CHECK(requests == 3);
    // Audio: 960 samples per grain ending at the next grain's first sample, silence while no
    // stream plays.
    REQUIRE(outputs.audio.size() == 5);
    CHECK(outputs.audio[0].first == 1002 * 960);
    CHECK(outputs.audio[4].first == 1006 * 960);
    CHECK(outputs.audio[0].second.size() == 960 * 2);
    CHECK(outputs.audio[0].second[0] == 0.f);
}

TEST_CASE("the video delay holds frames back by whole grains")
{
    ManualClock clock(2000 * kPeriod);
    Recorder outputs;
    util::Metrics metrics("test_");
    auto settings = small();
    settings.videoDelayGrains = 2;
    Engine engine(settings, clock, outputs, metrics);
    engine.setPageState(PageState::Ready);
    engine.start();

    auto const red = solid(0, 0, 255, 255);
    auto const blue = solid(255, 0, 0, 255);
    engine.paint(red.data(), kWidth * 4, kWidth, kHeight);
    waitConverted(engine, 1);
    clock.tick(2001);
    engine.paint(blue.data(), kWidth * 4, kWidth, kHeight);
    waitConverted(engine, 2);
    clock.tick(2002);
    clock.tick(2003);
    clock.tick(2004);
    clock.release();
    engine.stop();

    auto const transparent = fillOf(solid(0, 0, 0, 0), true);
    REQUIRE(outputs.video.size() == 4);
    CHECK(outputs.video[0].fill == transparent);
    CHECK(outputs.video[1].fill == transparent);
    CHECK(outputs.video[2].fill == fillOf(red, true));
    CHECK(outputs.video[3].fill == fillOf(blue, true));
}

TEST_CASE("a page that is not ready holds, or shows the substitute; its paint waits")
{
    ManualClock clock(3000 * kPeriod);
    Recorder outputs;
    util::Metrics metrics("test_");
    auto settings = small();
    settings.onPageError = engine::Substitute::Black;
    Engine engine(settings, clock, outputs, metrics);
    engine.setPageState(PageState::Ready);
    engine.start();

    auto const red = solid(0, 0, 255, 255);
    engine.paint(red.data(), kWidth * 4, kWidth, kHeight);
    waitConverted(engine, 1);
    clock.tick(3001);
    engine.setPageState(PageState::Loading);
    auto const green = solid(0, 255, 0, 255);
    engine.paint(green.data(), kWidth * 4, kWidth, kHeight);
    waitConverted(engine, 2);
    clock.tick(3002); // loading: black, the green paint is kept
    engine.setPageState(PageState::Ready);
    clock.tick(3003); // ready: the kept paint
    clock.release();
    engine.stop();

    REQUIRE(outputs.video.size() == 3);
    CHECK(outputs.video[0].fill == fillOf(red, true));
    CHECK(outputs.video[1].fill == fillOf(solid(0, 0, 0, 255), true));
    CHECK(outputs.video[2].fill == fillOf(green, true));
    CHECK(engine.stats().tick.repeatedHold == 1);
}

TEST_CASE("a ready page without a new paint holds its last frame, whatever on_page_error says")
{
    for (auto const substitute : {engine::Substitute::Black, engine::Substitute::Transparent, engine::Substitute::Hold})
    {
        CAPTURE(static_cast<int>(substitute));
        ManualClock clock(3100 * kPeriod);
        Recorder outputs;
        util::Metrics metrics("test_");
        auto settings = small();
        settings.onPageError = substitute;
        Engine engine(settings, clock, outputs, metrics);
        engine.setPageState(PageState::Ready);
        engine.start();

        auto const red = solid(0, 0, 255, 255);
        engine.paint(red.data(), kWidth * 4, kWidth, kHeight);
        waitConverted(engine, 1);
        clock.tick(3101); // the paint
        clock.tick(3102); // no paint: a static page, a held lower third or a late paint
        clock.tick(3103);
        clock.release();
        engine.stop();

        REQUIRE(outputs.video.size() == 3);
        for (auto const& grain : outputs.video)
        {
            CHECK(grain.fill == fillOf(red, true));
        }
    }
}

TEST_CASE("disabled senders write nothing; the key flow gets the fill's index")
{
    ManualClock clock(4000 * kPeriod);
    Recorder outputs;
    util::Metrics metrics("test_");
    auto settings = small();
    settings.alphaPlane = false;
    settings.keyFlow = true;
    Engine engine(settings, clock, outputs, metrics);
    engine.setPageState(PageState::Ready);
    engine.start();
    clock.tick(4001);
    engine.setEnabled(engine::Flow::Video, false);
    engine.setEnabled(engine::Flow::Audio, false);
    clock.tick(4002);
    clock.release();
    engine.stop();

    REQUIRE(outputs.video.size() == 1);
    CHECK(outputs.video[0].index == 4001);
    CHECK(outputs.key == std::vector<std::uint64_t>{4001, 4002});
    CHECK(outputs.audio.size() == 1);
}

TEST_CASE("page audio reaches the flow once the FIFO holds its target")
{
    ManualClock clock(5000 * kPeriod);
    Recorder outputs;
    util::Metrics metrics("test_");
    Engine engine(small(), clock, outputs, metrics); // FIFO target 2880 frames (60 ms)
    engine.setPageState(PageState::Ready);
    engine.setAudioActive(true);
    engine.start();
    clock.tick(5001); // the stream starts from an empty FIFO: silence

    std::vector<float> left(3840, 0.5f);
    std::vector<float> right(3840, -0.25f);
    float const* planes[2] = {left.data(), right.data()};
    engine.pushAudio(planes, 2, 3840);
    clock.tick(5002);
    clock.tick(5003);
    clock.release();
    engine.stop();

    REQUIRE(outputs.audio.size() == 3);
    CHECK(outputs.audio[0].second[0] == 0.f);
    auto const& last = outputs.audio.back().second;
    CHECK(last[960] == doctest::Approx(0.5f).epsilon(0.01));
    CHECK(last[961] == doctest::Approx(-0.25f).epsilon(0.01));
    auto const stats = engine.stats();
    CHECK(stats.audioPeakDbfs[0] == doctest::Approx(-6.02).epsilon(0.01));
    CHECK(stats.audio.underruns == 0);
}

TEST_CASE("a preview is made on request from the newest paint")
{
    ManualClock clock(6000 * kPeriod);
    Recorder outputs;
    util::Metrics metrics("test_");
    Engine engine(small(), clock, outputs, metrics);
    engine.start();
    CHECK(engine.previewWidth() == 24);
    CHECK(engine.previewHeight() == 2);
    auto const white = solid(255, 255, 255, 255);
    engine.paint(white.data(), kWidth * 4, kWidth, kHeight);
    waitConverted(engine, 1);
    std::vector<std::uint8_t> rgb;
    std::uint64_t sequence = 0;
    CHECK_FALSE(engine.preview(1, rgb, sequence));
    auto const first = engine.requestPreview();
    for (int i = 0; i < 500 && !engine.preview(first, rgb, sequence); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(sequence == 1);
    REQUIRE(rgb.size() == 24 * 2 * 3);
    CHECK(rgb[0] == 255);

    // A new paint, then a request: its first answer is that paint, not the preview made before
    // it (1.1.0 answered /api/v1/preview.jpg with the previous request's picture).
    auto const red = solid(0, 0, 255, 255);
    engine.paint(red.data(), kWidth * 4, kWidth, kHeight);
    waitConverted(engine, 2);
    auto const second = engine.requestPreview();
    CHECK(second == first + 1);
    for (int i = 0; i < 500 && !engine.preview(second, rgb, sequence); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(sequence == 2);
    CHECK(rgb[0] == 255);
    CHECK(rgb[1] == 0);
    CHECK(rgb[2] == 0);
    clock.release();
    engine.stop();
}

TEST_CASE("with the frame pipeline a late paint is shown one grain later instead of being replaced")
{
    for (std::uint32_t pipeline : {0U, 1U})
    {
        CAPTURE(pipeline);
        ManualClock clock(3000 * kPeriod);
        Recorder outputs;
        util::Metrics metrics("test_");
        auto settings = small();
        settings.pipelineGrains = pipeline;
        Engine engine(settings, clock, outputs, metrics);
        engine.setRequestFrame([&] { engine.beginFrameDispatched(engine.beginFrameSent()); });
        engine.setPageState(PageState::Ready);
        engine.start();

        clock.tick(3001); // BeginFrame posted and sent at 3001
        auto const red = solid(0, 0, 255, 255);
        auto const blue = solid(255, 0, 0, 255);
        engine.paint(red.data(), kWidth * 4, kWidth, kHeight); // a late paint of the BeginFrame before
        waitConverted(engine, 1);
        engine.paint(blue.data(), kWidth * 4, kWidth, kHeight); // the paint of 3001's BeginFrame
        waitConverted(engine, 2);
        clock.tick(3002);
        clock.tick(3003);
        CHECK(engine.nextVisibleIndex() == 3004 + pipeline);
        clock.release();
        engine.stop();

        REQUIRE(outputs.video.size() == 3); // 3001 has no paint yet
        if (pipeline == 0)
        {
            CHECK(outputs.video[1].fill == fillOf(blue, true)); // red was replaced
            CHECK(outputs.video[2].fill == fillOf(blue, true)); // a held frame
            CHECK(engine.stats().latePaints == 1);
            CHECK(engine.stats().tick.repeated == 2);
        }
        else
        {
            CHECK(outputs.video[1].fill == fillOf(red, true));
            CHECK(outputs.video[2].fill == fillOf(blue, true));
            CHECK(engine.stats().latePaints == 0);
            CHECK(engine.stats().tick.repeated == 1);
        }

        // The trace: the BeginFrame posted and sent, the paints, their conversions, and commits
        // that carry the BeginFrame's time.
        std::uint64_t posted = 0;
        int paints = 0;
        int converted = 0;
        int fresh = 0;
        for (auto const& e : engine.trace())
        {
            if (e.kind == engine::TraceKind::BeginFramePosted && posted == 0)
            {
                posted = e.ns;
            }
            paints += e.kind == engine::TraceKind::Paint ? 1 : 0;
            converted += e.kind == engine::TraceKind::ConvertEnd ? 1 : 0;
            if (e.kind == engine::TraceKind::Commit && e.b != 0)
            {
                ++fresh;
                CHECK(e.b == posted);
            }
        }
        CHECK(posted == 3001 * kPeriod + kMargin);
        CHECK(paints == 2);
        CHECK(converted == 2);
        CHECK(fresh == (pipeline == 0 ? 1 : 2));
        CHECK(metrics.render().find("test_frame_ready_seconds_count 2") != std::string::npos);
    }
}

TEST_CASE("a paint belongs to the newest BeginFrame dispatched before it, not to one still queued")
{
    ManualClock clock(4000 * kPeriod);
    Recorder outputs;
    util::Metrics metrics("test_");
    Engine engine(small(), clock, outputs, metrics);
    std::mutex mutex;
    std::vector<std::uint64_t> posted; // posted by the tick, dispatched by the test (the "UI thread")
    engine.setRequestFrame([&] {
        auto const t = engine.beginFrameSent();
        std::lock_guard lock{mutex};
        posted.push_back(t);
    });
    engine.setPageState(PageState::Ready);
    engine.start();

    auto const red = solid(0, 0, 255, 255);
    clock.tick(4001);
    engine.beginFrameDispatched(posted.at(0));
    clock.tick(4002); // 4002's BeginFrame is posted but still queued when the late paint of 4001 arrives
    engine.paint(red.data(), kWidth * 4, kWidth, kHeight);
    waitConverted(engine, 1);
    engine.beginFrameDispatched(posted.at(1));
    engine.paint(red.data(), kWidth * 4, kWidth, kHeight);
    waitConverted(engine, 2);
    clock.release();
    engine.stop();

    std::vector<std::uint64_t> owners;
    for (auto const& e : engine.trace())
    {
        if (e.kind == engine::TraceKind::ConvertStart)
        {
            owners.push_back(e.b);
        }
    }
    REQUIRE(owners.size() == 2);
    CHECK(owners[0] == posted.at(0));
    CHECK(owners[1] == posted.at(1));
}

TEST_CASE("with the internal frame clock the tick sends no BeginFrames and anchors Chromium's clock at T(k+1) + phase")
{
    ManualClock clock(6000 * kPeriod);
    Recorder outputs;
    util::Metrics metrics("test_");
    auto settings = small();
    settings.internalFrameClock = true;
    settings.framePeriodNs = kPeriod;
    settings.framePhaseNs = 5'000'000;
    settings.pipelineGrains = 1;
    Engine engine(settings, clock, outputs, metrics);
    int requested = 0;
    std::mutex mutex;
    std::vector<std::uint64_t> anchors;
    engine.setRequestFrame([&] { ++requested; });
    engine.setAnchorFrameClock([&](std::uint64_t target) {
        std::lock_guard lock{mutex};
        anchors.push_back(target);
    });
    engine.setPageState(PageState::Ready);
    engine.start();

    clock.tick(6001);
    clock.tick(6002); // one anchor in flight: no second one
    {
        std::lock_guard lock{mutex};
        REQUIRE(anchors.size() == 1);
        CHECK(anchors[0] == 6002 * kPeriod + 5'000'000);
    }
    engine.frameClockAnchored(anchors[0], anchors[0] + 30'000);
    CHECK(engine.stats().frameClock.locked);
    CHECK(engine.stats().frameClock.phaseErrorNs == 30'000);

    // A paint 12 ms after T(6002): it belongs to Chromium's BeginFrame at T(6002) + 5 ms.
    clock.set(6002 * kPeriod + 12'000'000);
    auto const red = solid(0, 0, 255, 255);
    engine.paint(red.data(), kWidth * 4, kWidth, kHeight);
    waitConverted(engine, 1);
    clock.tick(6003);
    clock.tick(6004);
    // A load locks again.
    engine.relockFrameClock(engine::LockReason::Load);
    clock.tick(6005);
    // Chromium's newest BeginFrame (T(6004) + 5 ms) may still draw a change, as the external
    // BeginFrame of the current grain does; the pipeline adds one.
    CHECK(engine.nextVisibleIndex() == 6004 + 1 + 1);
    clock.release();
    engine.stop();

    CHECK(requested == 0);
    CHECK(engine.stats().tick.beginFrames == 0);
    {
        std::lock_guard lock{mutex};
        REQUIRE(anchors.size() == 2);
        CHECK(anchors[1] == 6006 * kPeriod + 5'000'000);
    }
    REQUIRE(outputs.video.size() == 5);
    CHECK(outputs.video[2].fill == fillOf(red, true)); // committed at 6003
    bool anchored = false;
    for (auto const& e : engine.trace())
    {
        if (e.kind == engine::TraceKind::ConvertStart)
        {
            CHECK(e.b == 6002 * kPeriod + 5'000'000);
        }
        anchored = anchored || (e.kind == engine::TraceKind::Anchor && e.a == 6002 * kPeriod + 5'000'000 && e.b == e.a + 30'000);
    }
    CHECK(anchored);
    CHECK(metrics.render().find("test_paint_latency_seconds_count 1") != std::string::npos);
}

namespace
{
    // Two 10 ms packets of 0.5 per call, stamped with their capture time in ms.
    struct PacketSource
    {
        std::int64_t pts = 100'000;
        std::vector<float> data = std::vector<float>(480, 0.5f);
        void push(Engine& engine, int packets = 2)
        {
            float const* planes[2] = {data.data(), data.data()};
            for (int i = 0; i < packets; ++i)
            {
                engine.pushAudio(planes, 2, 480, pts);
                pts += 10;
            }
        }
    };

    // Frames of channel 1 below 0.01 between the first and the last loud one.
    std::size_t quietInside(Recorder const& outputs)
    {
        std::vector<float> left;
        for (auto const& [end, samples] : outputs.audio)
        {
            for (std::size_t i = 0; i < samples.size(); i += 2)
            {
                left.push_back(samples[i]);
            }
        }
        std::size_t first = left.size();
        std::size_t last = 0;
        for (std::size_t i = 0; i < left.size(); ++i)
        {
            if (std::fabs(left[i]) > 0.25f)
            {
                first = std::min(first, i);
                last = i;
            }
        }
        std::size_t quiet = 0;
        for (std::size_t i = first; i < last; ++i)
        {
            quiet += std::fabs(left[i]) < 0.01f ? 1 : 0;
        }
        return quiet;
    }
}

TEST_CASE("an upstream loss in the packet times becomes a gap of exactly its length, not an underrun")
{
    ManualClock clock(7000 * kPeriod);
    Recorder outputs;
    util::Metrics metrics("test_");
    Engine engine(small(), clock, outputs, metrics);
    engine.setPageState(PageState::Ready);
    engine.audioStreamStarted();
    engine.setAudioActive(true);
    engine.start();
    PacketSource source;
    std::uint64_t k = 7001;
    for (; k < 7021; ++k)
    {
        source.push(engine);
        clock.tick(k);
    }
    // The loopback falls behind and skips ahead by 20 ms: this tick gets nothing, the next the
    // packets after the skip.
    clock.tick(k++);
    source.pts += 20;
    for (; k < 7040; ++k)
    {
        source.push(engine);
        clock.tick(k);
    }
    clock.release();
    engine.stop();

    auto const stats = engine.stats();
    CHECK(stats.audioGaps == 1);
    CHECK(stats.audioGapFrames == 960);
    CHECK(stats.audio.underruns == 0);
    auto const quiet = quietInside(outputs);
    CHECK(quiet >= 900);
    CHECK(quiet <= 1000);
    CHECK(metrics.render().find("test_audio_gaps_total") == std::string::npos); // the app exports them
}

TEST_CASE("when the stream stops the queued audio is played out; a new stream is not cut")
{
    ManualClock clock(8000 * kPeriod);
    Recorder outputs;
    util::Metrics metrics("test_");
    Engine engine(small(), clock, outputs, metrics);
    engine.setPageState(PageState::Ready);
    engine.audioStreamStarted();
    engine.setAudioActive(true);
    engine.start();
    PacketSource source;
    std::uint64_t k = 8001;
    for (; k < 8011; ++k)
    {
        source.push(engine);
        clock.tick(k);
    }
    auto const before = outputs.audio.size();
    engine.setAudioActive(false, true); // CEF stopped the stream: about 60 ms are still queued
    for (int i = 0; i < 5; ++i)
    {
        clock.tick(k++);
    }
    std::size_t loudTicks = 0;
    for (std::size_t i = before; i < outputs.audio.size(); ++i)
    {
        loudTicks += std::fabs(outputs.audio[i].second[2 * 480]) > 0.25f ? 1 : 0;
    }
    CHECK(loudTicks >= 2);
    CHECK(loudTicks <= 3);
    CHECK(engine.stats().audio.underruns == 0);
    // A new stream (pts far later): its first sample plays one target after it arrived.
    source.pts += 5000;
    engine.audioStreamStarted();
    engine.setAudioActive(true);
    auto const restart = outputs.audio.size();
    for (int i = 0; i < 5; ++i)
    {
        source.push(engine);
        clock.tick(k++);
    }
    clock.release();
    engine.stop();
    CHECK(std::fabs(outputs.audio[restart + 1].second[2 * 959]) < 0.01f);
    CHECK(std::fabs(outputs.audio[restart + 2].second[2 * 480]) > 0.25f);
    CHECK(engine.stats().audio.underruns == 0);
    CHECK(engine.stats().audio.realigned == 0);
    CHECK(engine.stats().audioGaps == 0); // a new stream is a new timeline
}

TEST_CASE("the video delay follows the adaptive audio target one grain per tick, so lip sync holds")
{
    ManualClock clock(9000 * kPeriod);
    Recorder outputs;
    util::Metrics metrics("test_");
    auto settings = small();
    settings.videoDelayGrains = 1;
    settings.audioMaxTargetFrames = 14400; // 300 ms
    settings.videoDelayMaxGrains = 1 + 12;
    Engine engine(settings, clock, outputs, metrics);
    engine.setPageState(PageState::Ready);
    engine.audioStreamStarted();
    engine.setAudioActive(true);
    engine.start();
    PacketSource source;
    // Every grain a new frame, numbered by its red value.
    std::vector<std::vector<std::uint8_t>> fills;
    std::uint64_t k = 9001;
    std::uint64_t painted = 0;
    auto tick = [&](int packets) {
        auto const frame = solid(0, 0, static_cast<std::uint8_t>(10 + fills.size()), 255);
        fills.push_back(fillOf(frame, true));
        engine.paint(frame.data(), kWidth * 4, kWidth, kHeight);
        waitConverted(engine, ++painted);
        source.push(engine, packets);
        clock.tick(k++);
    };
    for (int i = 0; i < 50; ++i)
    {
        tick(2);
    }
    CHECK(engine.stats().videoDelayGrains == 1);
    // Chromium holds its audio for 200 ms, then delivers it at once.
    for (int i = 0; i < 10; ++i)
    {
        tick(0);
    }
    source.push(engine, 20);
    for (int i = 0; i < 30; ++i)
    {
        tick(2);
    }
    clock.release();
    engine.stop();

    auto const stats = engine.stats();
    CHECK(stats.audio.underruns == 1);
    CHECK(stats.audio.realigned == 0);
    // The target rose by the silence (about 150 ms): the video waits as many grains more.
    auto const extraGrains = std::lround((stats.audio.targetSeconds - 0.06) / 0.02);
    CHECK(extraGrains >= 6);
    CHECK(stats.videoDelayGrains == 1 + static_cast<std::uint32_t>(extraGrains));
    CHECK(engine.nextVisibleIndex() >= k + stats.videoDelayGrains);
    // The video held (repeats) while the delay grew; it never stepped back in time.
    std::vector<int> shown;
    for (auto const& grain : outputs.video)
    {
        auto const it = std::find(fills.begin(), fills.end(), grain.fill);
        shown.push_back(it == fills.end() ? -1 : static_cast<int>(it - fills.begin()));
    }
    int repeats = 0;
    for (std::size_t i = 1; i < shown.size(); ++i)
    {
        if (shown[i - 1] < 0)
        {
            continue;
        }
        CHECK(shown[i] >= shown[i - 1]);
        CHECK(shown[i] <= shown[i - 1] + 1);
        repeats += shown[i] == shown[i - 1] ? 1 : 0;
    }
    CHECK(repeats == extraGrains);
}

TEST_CASE("the video delay counts nominal grains: at 59.94 a constant audio extra never flips between ticks of 800 and 801 frames")
{
    // 59.94: 48000 · 1001 / 60000 = 800.8 frames per grain; the ticks alternate 800 and 801.
    double const nominal = 48000.0 * 1001.0 / 60000.0;
    std::vector<std::size_t> tickFrames;
    for (std::uint64_t i = 0; i < 10; ++i)
    {
        tickFrames.push_back(static_cast<std::size_t>((i + 1) * 48000 * 1001 / 60000 - i * 48000 * 1001 / 60000));
    }
    CHECK(std::count(tickFrames.begin(), tickFrames.end(), 800) == 2);
    CHECK(std::count(tickFrames.begin(), tickFrames.end(), 801) == 8);
    // 4402 frames lies between 5.5 grains of 800 (4400) and of 801 (4405.5): dividing by the
    // tick's frames wanted 6 and 5 by turns, a repeat and a skip about 12 times a second.
    int flips = 0;
    std::uint32_t last = engine::videoDelayExtraGrains(4402, nominal);
    for (int k = 0; k < 600; ++k)
    {
        auto const grains = engine::videoDelayExtraGrains(4402, nominal);
        flips += grains != last ? 1 : 0;
        last = grains;
    }
    CHECK(flips == 0);
    CHECK(last == 5);
    CHECK(std::lround(4402.0 / 800) != std::lround(4402.0 / 801)); // the old divisor did flip
    // Whole grains, rounded; nothing for no extra.
    CHECK(engine::videoDelayExtraGrains(0, nominal) == 0);
    CHECK(engine::videoDelayExtraGrains(-100, nominal) == 0);
    CHECK(engine::videoDelayExtraGrains(14400, 960) == 15);
    CHECK(engine::videoDelayExtraGrains(14400 - 481, 960) == 14);
}

TEST_CASE("the audio FIFO holds the largest target up to the overrun cut")
{
    EngineSettings s;
    s.audioTargetFrames = 2880;
    s.audioMaxTargetFrames = 0;
    CHECK(engine::audioFifoFrames(s) == 3 * (2880 + 24000) + 4800);
    s.audioTargetFrames = 500 * 48;
    s.audioMaxTargetFrames = 500 * 48; // the largest settings: 500 ms target, +500 ms AV offset
    CHECK(engine::audioFifoFrames(s) == 3 * 48000 + 4800);
    s.audioTargetFrames = 960;
    s.audioMaxTargetFrames = 14400;
    CHECK(engine::audioFifoFrames(s) >= static_cast<std::size_t>(audio::Resampler::kOverrunFactor * (14400 + engine::kMaxAvOffsetFrames)));
}

TEST_CASE("the trace rings are copied while writers keep writing, without a lock")
{
    util::TraceRing<engine::AudioTraceEvent> ring(256);
    std::atomic<bool> stop{false};
    std::vector<std::thread> writers;
    for (int w = 0; w < 2; ++w)
    {
        writers.emplace_back([&, w] {
            std::int64_t n = 0;
            while (!stop.load())
            {
                // Every field carries the same number, so a torn copy would show.
                ring.record({static_cast<std::uint64_t>(n + 1), engine::AudioTraceKind::Packet, w, n, n, n, n});
                ++n;
            }
        });
    }
    std::size_t copied = 0;
    for (int i = 0; i < 200; ++i)
    {
        auto const events = ring.snapshot();
        CHECK(events.size() <= 256);
        for (auto const& e : events)
        {
            CHECK(e.b == e.c);
            CHECK(e.c == e.d);
            CHECK(e.d == e.e);
            CHECK(e.ns == static_cast<std::uint64_t>(e.b + 1));
        }
        copied += events.size();
    }
    stop = true;
    for (auto& t : writers)
    {
        t.join();
    }
    CHECK(copied > 0);
    auto const last = ring.snapshot();
    CHECK(last.size() == 256); // quiet: every slot complete
}

TEST_CASE("when the stream stops, a raised audio target and the video delay go back after the hold")
{
    ManualClock clock(20000 * kPeriod);
    Recorder outputs;
    util::Metrics metrics("test_");
    auto settings = small();
    settings.videoDelayGrains = 1;
    settings.audioMaxTargetFrames = 14400;
    settings.videoDelayMaxGrains = 1 + 12;
    Engine engine(settings, clock, outputs, metrics);
    engine.setPageState(PageState::Ready);
    engine.audioStreamStarted();
    engine.setAudioActive(true);
    engine.start();
    PacketSource source;
    std::uint64_t k = 20001;
    for (int i = 0; i < 50; ++i)
    {
        source.push(engine);
        clock.tick(k++);
    }
    for (int i = 0; i < 10; ++i) // 200 ms held, then delivered at once
    {
        clock.tick(k++);
    }
    source.push(engine, 20);
    for (int i = 0; i < 30; ++i)
    {
        source.push(engine);
        clock.tick(k++);
    }
    auto const raised = engine.stats().videoDelayGrains;
    REQUIRE(raised > 1);
    engine.setAudioActive(false); // the clip ended; a graphics page follows
    for (int i = 0; i < 400; ++i) // 8 s: within the hold
    {
        clock.tick(k++);
    }
    CHECK(engine.stats().videoDelayGrains == raised);
    // Past 10 s since the underrun the target goes back in one step; the video delay follows by
    // at most one grain (one skipped frame) per 25 ticks, never two skips in a row.
    std::vector<std::uint32_t> delays{raised};
    for (int i = 0; i < 150 + 25 * static_cast<int>(raised); ++i)
    {
        clock.tick(k++);
        delays.push_back(engine.stats().videoDelayGrains);
    }
    clock.release();
    engine.stop();
    int lastSkip = -1000;
    for (std::size_t i = 1; i < delays.size(); ++i)
    {
        CHECK(delays[i] <= delays[i - 1]);
        CHECK(delays[i - 1] - delays[i] <= 1);
        if (delays[i] < delays[i - 1])
        {
            CHECK(static_cast<int>(i) - lastSkip >= 25);
            lastSkip = static_cast<int>(i);
        }
    }
    CHECK(engine.stats().videoDelayGrains == 1);
    CHECK(engine.stats().audio.targetSeconds == doctest::Approx(0.06));
    CHECK(engine.stats().audioFifoOverflows == 0);
}
