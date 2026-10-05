// SPDX-License-Identifier: MIT
// Prometheus text exposition (SPEC §12): counters, gauges and histograms with millisecond
// buckets, all under one prefix. Thread-safe; every call takes one short lock.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace mbs::util
{
    using Labels = std::vector<std::pair<std::string, std::string>>;

    class Metrics
    {
    public:
        explicit Metrics(std::string prefix);

        void inc(std::string const& name, Labels const& labels = {}, double value = 1);
        /// Sets a counter that is counted elsewhere (a snapshot of an atomic).
        void setCounter(std::string const& name, Labels const& labels, double value);
        void set(std::string const& name, Labels const& labels, double value);
        void observe(std::string const& name, Labels const& labels, double seconds);
        /// Removes every series of `name` (a gauge whose label set changes, such as `page_state`).
        void clear(std::string const& name);

        [[nodiscard]] std::string render() const;

    private:
        enum class Kind
        {
            Counter,
            Gauge
        };
        struct Series
        {
            Kind kind = Kind::Counter;
            std::map<Labels, double> values;
        };
        struct Histogram
        {
            std::map<Labels, std::pair<std::vector<std::uint64_t>, std::pair<std::uint64_t, double>>> values; // buckets, count, sum
        };

        static std::string labelText(Labels const& labels, std::string const& extra = {});

        std::string _prefix;
        mutable std::mutex _mutex;
        std::map<std::string, Series> _series;
        std::map<std::string, Histogram> _histograms;
        std::vector<double> _bounds{0.001, 0.002, 0.005, 0.01, 0.015, 0.02, 0.03, 0.04, 0.06, 0.1, 0.25, 1.0};
    };
}
