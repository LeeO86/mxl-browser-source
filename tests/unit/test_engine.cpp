// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <chrono>
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
    CHECK_FALSE(engine.preview(rgb, sequence));
    engine.requestPreview();
    for (int i = 0; i < 500 && !engine.preview(rgb, sequence); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(sequence == 1);
    REQUIRE(rgb.size() == 24 * 2 * 3);
    CHECK(rgb[0] == 255);
    clock.release();
    engine.stop();
}
