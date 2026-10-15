// SPDX-License-Identifier: MIT
#include "ograf/zip.hpp"

#include <cstdio>
#include <filesystem>
#include <memory>

#include "miniz/miniz.h"

namespace fs = std::filesystem;

namespace mbs::ograf
{
    namespace
    {
        // Normalises an entry name to a safe relative path; empty = refuse, "-" = skip.
        std::string entryPath(std::string name, bool& skip)
        {
            skip = false;
            for (auto& c : name)
            {
                if (c == '\\')
                {
                    c = '/'; // some Windows tools write backslashes
                }
            }
            if (name.empty() || name.front() == '/' || name.find(':') != std::string::npos)
            {
                return {};
            }
            std::string out;
            std::size_t start = 0;
            while (start < name.size())
            {
                auto end = name.find('/', start);
                if (end == std::string::npos)
                {
                    end = name.size();
                }
                auto const part = name.substr(start, end - start);
                if (part == "..")
                {
                    return {};
                }
                if (!part.empty() && part != ".")
                {
                    if (part.front() == '.' || part == "__MACOSX")
                    {
                        skip = true;
                    }
                    out += (out.empty() ? "" : "/") + part;
                }
                start = end + 1;
            }
            return out;
        }

        struct Sink
        {
            std::FILE* file = nullptr;
            std::uint64_t written = 0;
            std::uint64_t limit = 0;
        };

        std::size_t write(void* opaque, mz_uint64, void const* data, std::size_t n)
        {
            auto& sink = *static_cast<Sink*>(opaque);
            if (sink.written + n > sink.limit)
            {
                return 0; // stops the extraction
            }
            sink.written += n;
            return std::fwrite(data, 1, n, sink.file);
        }
    }

    std::optional<std::vector<std::string>> unpackZip(std::string const& zipPath, std::string const& dest, ZipLimits const& limits, std::string& error)
    {
        mz_zip_archive zip{};
        if (!mz_zip_reader_init_file(&zip, zipPath.c_str(), 0))
        {
            error = "not a ZIP file";
            return std::nullopt;
        }
        std::unique_ptr<mz_zip_archive, void (*)(mz_zip_archive*)> guard(&zip, [](mz_zip_archive* z) { mz_zip_reader_end(z); });
        auto const count = mz_zip_reader_get_num_files(&zip);
        if (count > limits.maxEntries)
        {
            error = "too many entries (" + std::to_string(count) + ", at most " + std::to_string(limits.maxEntries) + ")";
            return std::nullopt;
        }
        std::uint64_t total = 0;
        std::vector<std::string> files;
        for (mz_uint i = 0; i < count; ++i)
        {
            mz_zip_archive_file_stat st{};
            if (!mz_zip_reader_file_stat(&zip, i, &st))
            {
                error = "unreadable entry " + std::to_string(i);
                return std::nullopt;
            }
            bool skip = false;
            auto const rel = entryPath(st.m_filename, skip);
            if (rel.empty() && !st.m_is_directory)
            {
                error = std::string("unsafe path '") + st.m_filename + "'";
                return std::nullopt;
            }
            if (skip || rel.empty())
            {
                continue;
            }
            // Unix mode in the high 16 bits when the entry was made on Unix: only files and folders.
            unsigned const mode = (st.m_external_attr >> 16) & 0170000u;
            if (mode != 0 && mode != 0100000u && mode != 0040000u)
            {
                error = "'" + rel + "' is not a regular file (symlinks and devices are refused)";
                return std::nullopt;
            }
            if (st.m_is_encrypted || (!st.m_is_directory && !st.m_is_supported))
            {
                error = "'" + rel + "' is encrypted or uses an unsupported compression";
                return std::nullopt;
            }
            auto const target = fs::path(dest) / rel;
            std::error_code ec;
            if (st.m_is_directory)
            {
                fs::create_directories(target, ec);
                continue;
            }
            total += st.m_uncomp_size;
            if (total > limits.maxUnpackedBytes)
            {
                error = "unpacked size over " + std::to_string(limits.maxUnpackedBytes >> 20) + " MiB";
                return std::nullopt;
            }
            fs::create_directories(target.parent_path(), ec);
            if (fs::exists(target, ec))
            {
                error = "'" + rel + "' appears twice";
                return std::nullopt;
            }
            Sink sink{std::fopen(target.c_str(), "wb"), 0, st.m_uncomp_size};
            if (sink.file == nullptr)
            {
                error = "cannot write '" + rel + "'";
                return std::nullopt;
            }
            // The declared size is the limit: a lying header cannot unpack more (CRC is checked too).
            bool const ok = mz_zip_reader_extract_to_callback(&zip, i, write, &sink, 0) && sink.written == st.m_uncomp_size;
            bool const closed = std::fclose(sink.file) == 0;
            if (!ok || !closed)
            {
                error = "'" + rel + "' is damaged";
                return std::nullopt;
            }
            files.push_back(rel);
        }
        return files;
    }
}
