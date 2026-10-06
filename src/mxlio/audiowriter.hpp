// SPDX-License-Identifier: MIT
// Audio sample writing (SPEC §6): 48 kHz float32, planar in MXL, addressed by sample index.
#pragma once

#include <cstdint>
#include <string>

#include <mxl/flow.h>
#include <mxl/mxl.h>
#include <mxl/rational.h>

#include "mxlio/domain.hpp"
#include "mxlio/flowdef.hpp"

namespace mbs::mxlio
{
    class AudioWriter
    {
    public:
        /// Creates (or opens) the flow and its writer. Throws on failure.
        AudioWriter(Domain& domain, AudioFlowParams const& params, int commitBatchHint);
        ~AudioWriter();

        AudioWriter(AudioWriter const&) = delete;
        AudioWriter& operator=(AudioWriter const&) = delete;

        /// Writes `frames` samples per channel so that the batch ENDS at sample index `endIndex`
        /// (MXL addresses `count` samples ending at the index). `interleaved` holds
        /// frames × channelCount() floats; nullptr writes silence. Batches longer than
        /// maxWriteLength() are split.
        mxlStatus write(std::uint64_t endIndex, float const* interleaved, std::size_t frames);

        [[nodiscard]] std::uint32_t channelCount() const
        {
            return _configInfo.continuous.channelCount;
        }

        [[nodiscard]] std::size_t maxWriteLength() const
        {
            return _maxWriteLength;
        }

        [[nodiscard]] std::string const& flowId() const
        {
            return _flowIdString;
        }

    private:
        Domain& _domain;
        mxlFlowWriter _writer = nullptr;
        mxlFlowConfigInfo _configInfo{};
        std::size_t _maxWriteLength = 0;
        std::string _flowIdString;
    };
}
