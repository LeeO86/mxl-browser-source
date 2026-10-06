// SPDX-License-Identifier: MIT
#include "audiowriter.hpp"

#include <cstring>
#include <stdexcept>

#include "util/logging.hpp"

namespace mbs::mxlio
{
    AudioWriter::AudioWriter(Domain& domain, AudioFlowParams const& params, int commitBatchHint)
        : _domain(domain)
        , _flowIdString(params.id.toString())
    {
        auto const flowDef = buildAudioFlowDef(params);
        auto const options = buildWriterOptions(commitBatchHint);
        bool created = false;
        auto const status = ::mxlCreateFlowWriter(domain.instance(), flowDef.c_str(), options.c_str(), &_writer, &_configInfo, &created);
        if (status != MXL_STATUS_OK)
        {
            throw std::runtime_error("mxlCreateFlowWriter (audio " + _flowIdString + ") failed with status " + std::to_string(status));
        }
        std::size_t maxWrite = 0;
        if (::mxlFlowWriterGetMaxWriteLengthSamples(_writer, &maxWrite) == MXL_STATUS_OK && maxWrite > 0)
        {
            _maxWriteLength = maxWrite;
        }
        else
        {
            _maxWriteLength = _configInfo.continuous.bufferLength / 2;
        }
        log::info("mxl_audio_flow_writer_created",
            {
                {"flow_id", _flowIdString},
                {"created", created},
                {"channel_count", _configInfo.continuous.channelCount},
                {"buffer_length", _configInfo.continuous.bufferLength},
                {"max_write_samples", static_cast<std::uint64_t>(_maxWriteLength)},
            });
    }

    AudioWriter::~AudioWriter()
    {
        if (_writer != nullptr)
        {
            ::mxlReleaseFlowWriter(_domain.instance(), _writer);
        }
    }

    mxlStatus AudioWriter::write(std::uint64_t endIndex, float const* interleaved, std::size_t frames)
    {
        if (_maxWriteLength == 0 || frames > endIndex)
        {
            return MXL_ERR_INVALID_ARG;
        }
        std::size_t const channels = channelCount();
        std::uint64_t const start = endIndex - frames;
        std::size_t done = 0;
        while (done < frames)
        {
            auto const count = frames - done < _maxWriteLength ? frames - done : _maxWriteLength;
            mxlMutableWrappedMultiBufferSlice slices{};
            auto const status = ::mxlFlowWriterOpenSamples(_writer, start + done + count, count, &slices);
            if (status != MXL_STATUS_OK)
            {
                return status;
            }
            // One ring per channel, `stride` bytes apart; a ring wrap splits the range in two fragments.
            for (std::size_t channel = 0; channel < slices.count && channel < channels; ++channel)
            {
                std::size_t sample = done;
                for (auto const& fragment : slices.base.fragments)
                {
                    auto* out = reinterpret_cast<float*>(static_cast<std::uint8_t*>(fragment.pointer) + channel * slices.stride);
                    std::size_t const n = fragment.size / sizeof(float);
                    if (interleaved == nullptr)
                    {
                        std::memset(out, 0, n * sizeof(float));
                        sample += n;
                        continue;
                    }
                    for (std::size_t i = 0; i < n && sample < done + count; ++i, ++sample)
                    {
                        out[i] = interleaved[sample * channels + channel];
                    }
                }
            }
            auto const committed = ::mxlFlowWriterCommitSamples(_writer);
            if (committed != MXL_STATUS_OK)
            {
                return committed;
            }
            done += count;
        }
        return MXL_STATUS_OK;
    }
}
