// SPDX-License-Identifier: MIT
// The engine's clock and outputs over libmxl: TAI from MXL's time functions, grains and
// samples through the flow writers.
#pragma once

#include <time.h>

#include <atomic>
#include <cstdint>
#include <memory>

#include <mxl/time.h>

#include "engine/engine.hpp"
#include "mxlio/audiowriter.hpp"
#include "mxlio/videowriter.hpp"
#include "util/logging.hpp"

namespace mbs::app
{
    class MxlClock final : public engine::Clock
    {
    public:
        MxlClock(std::int64_t rateNum, std::int64_t rateDen)
            : _rate{rateNum, rateDen}
        {
        }

        [[nodiscard]] std::uint64_t nowNs() const override { return mxlGetTime(); }
        [[nodiscard]] std::uint64_t indexAt(std::uint64_t taiNs) const override { return mxlTimestampToIndex(&_rate, taiNs); }
        [[nodiscard]] std::uint64_t indexNs(std::uint64_t index) const override { return mxlIndexToTimestamp(&_rate, index); }

        /// 48 kHz sample index at the start of grain `index`: index·48000·den/num, exact.
        [[nodiscard]] std::uint64_t sampleAt(std::uint64_t index) const override
        {
            __extension__ using U128 = unsigned __int128;
            return static_cast<std::uint64_t>(U128{index} * 48000U * static_cast<std::uint64_t>(_rate.denominator) / static_cast<std::uint64_t>(_rate.numerator));
        }

        void sleepUntil(std::uint64_t taiNs) override
        {
            timespec ts{};
            ts.tv_sec = static_cast<time_t>(taiNs / 1'000'000'000ULL);
            ts.tv_nsec = static_cast<long>(taiNs % 1'000'000'000ULL);
            while (clock_nanosleep(CLOCK_TAI, TIMER_ABSTIME, &ts, nullptr) == EINTR)
            {
            }
        }

    private:
        mxlRational _rate;
    };

    class MxlOutputs final : public engine::Outputs
    {
    public:
        MxlOutputs(std::unique_ptr<mxlio::VideoWriter> video, std::unique_ptr<mxlio::VideoWriter> key, std::unique_ptr<mxlio::AudioWriter> audio)
            : _video(std::move(video))
            , _key(std::move(key))
            , _audio(std::move(audio))
        {
        }

        void writeVideo(std::uint64_t index, engine::GrainBuffers const& frame) override
        {
            auto const status = frame.alpha.empty() ? _video->writeFrame(index, frame.fill.data(), frame.fill.size())
                                                    : _video->writeFrameWithKey(index, frame.fill.data(), frame.fill.size(), frame.alpha.data(), frame.alpha.size());
            failed(status, "video");
        }

        void writeKey(std::uint64_t index, engine::GrainBuffers const& frame) override
        {
            if (_key)
            {
                failed(_key->writeFrame(index, frame.key.data(), frame.key.size()), "key");
            }
        }

        void writeAudio(std::uint64_t endSample, float const* interleaved, std::size_t frames) override
        {
            if (_audio)
            {
                failed(_audio->write(endSample, interleaved, frames), "audio");
            }
        }

        [[nodiscard]] std::uint64_t failures() const { return _failures.load(); }
        [[nodiscard]] mxlio::VideoWriter const& video() const { return *_video; }

    private:
        void failed(mxlStatus status, char const* flow)
        {
            if (status == MXL_STATUS_OK)
            {
                return;
            }
            // Logged at the first failure and then every 500th, not per grain.
            if (_failures.fetch_add(1) % 500 == 0)
            {
                log::error("mxl_write_failed", {{"flow", flow}, {"status", static_cast<int>(status)}});
            }
        }

        std::unique_ptr<mxlio::VideoWriter> _video;
        std::unique_ptr<mxlio::VideoWriter> _key;
        std::unique_ptr<mxlio::AudioWriter> _audio;
        std::atomic<std::uint64_t> _failures{0};
    };
}
