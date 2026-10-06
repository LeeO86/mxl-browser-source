// SPDX-License-Identifier: MIT
#include "util/metrics.hpp"

#include <algorithm>
#include <sstream>

namespace mbs::util
{
    namespace
    {
        Labels sorted(Labels labels)
        {
            std::sort(labels.begin(), labels.end());
            return labels;
        }

        std::string escape(std::string const& value)
        {
            std::string out;
            for (char c : value)
            {
                if (c == '\\' || c == '"')
                {
                    out += '\\';
                    out += c;
                }
                else if (c == '\n')
                {
                    out += "\\n";
                }
                else
                {
                    out += c;
                }
            }
            return out;
        }
    }

    Metrics::Metrics(std::string prefix)
        : _prefix(std::move(prefix))
    {
    }

    std::string Metrics::labelText(Labels const& labels, std::string const& extra)
    {
        if (labels.empty() && extra.empty())
        {
            return {};
        }
        std::string out = "{";
        for (std::size_t i = 0; i < labels.size(); ++i)
        {
            if (i != 0)
            {
                out += ',';
            }
            out += labels[i].first + "=\"" + escape(labels[i].second) + '"';
        }
        if (!extra.empty())
        {
            if (!labels.empty())
            {
                out += ',';
            }
            out += extra;
        }
        return out + '}';
    }

    void Metrics::inc(std::string const& name, Labels const& labels, double value)
    {
        std::lock_guard lock{_mutex};
        _series[name].values[sorted(labels)] += value;
    }

    void Metrics::setCounter(std::string const& name, Labels const& labels, double value)
    {
        std::lock_guard lock{_mutex};
        _series[name].values[sorted(labels)] = value;
    }

    void Metrics::set(std::string const& name, Labels const& labels, double value)
    {
        std::lock_guard lock{_mutex};
        auto& series = _series[name];
        series.kind = Kind::Gauge;
        series.values[sorted(labels)] = value;
    }

    void Metrics::observe(std::string const& name, Labels const& labels, double seconds)
    {
        std::lock_guard lock{_mutex};
        auto& [buckets, total] = _histograms[name].values[sorted(labels)];
        if (buckets.empty())
        {
            buckets.assign(_bounds.size(), 0);
        }
        for (std::size_t i = 0; i < _bounds.size(); ++i)
        {
            if (seconds <= _bounds[i])
            {
                ++buckets[i]; // each bucket counts every sample <= its bound (cumulative)
            }
        }
        ++total.first;
        total.second += seconds;
    }

    void Metrics::clear(std::string const& name)
    {
        std::lock_guard lock{_mutex};
        _series.erase(name);
    }

    std::string Metrics::render() const
    {
        std::lock_guard lock{_mutex};
        std::ostringstream out;
        for (auto const& [name, series] : _series)
        {
            auto const full = _prefix + name;
            out << "# TYPE " << full << (series.kind == Kind::Gauge ? " gauge" : " counter") << '\n';
            for (auto const& [labels, value] : series.values)
            {
                out << full << labelText(labels) << ' ' << value << '\n';
            }
        }
        for (auto const& [name, histogram] : _histograms)
        {
            auto const full = _prefix + name;
            out << "# TYPE " << full << " histogram\n";
            for (auto const& [labels, data] : histogram.values)
            {
                auto const& [buckets, total] = data;
                for (std::size_t i = 0; i < _bounds.size(); ++i)
                {
                    std::ostringstream bound;
                    bound << _bounds[i];
                    out << full << "_bucket" << labelText(labels, "le=\"" + bound.str() + '"') << ' ' << buckets[i] << '\n';
                }
                out << full << "_bucket" << labelText(labels, "le=\"+Inf\"") << ' ' << total.first << '\n';
                out << full << "_sum" << labelText(labels) << ' ' << total.second << '\n';
                out << full << "_count" << labelText(labels) << ' ' << total.first << '\n';
            }
        }
        return out.str();
    }
}
