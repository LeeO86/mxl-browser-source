# Embeds the OGraf JSON schemas (third_party/ograf/json-schemas) into one header, so manifest
# validation needs no network (docs/ograf.md §4). Run at configure time.
function(embed_ograf_schemas root output)
    file(GLOB_RECURSE files RELATIVE "${root}" "${root}/*.json")
    list(SORT files)
    set(arrays "")
    set(entries "")
    set(i 0)
    foreach(rel IN LISTS files)
        file(READ "${root}/${rel}" content HEX)
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${content}")
        string(APPEND arrays "    inline constexpr unsigned char kSchema${i}[] = {${bytes}};\n")
        string(APPEND entries "        {\"${rel}\", {reinterpret_cast<char const*>(kSchema${i}), sizeof(kSchema${i})}},\n")
        math(EXPR i "${i} + 1")
    endforeach()
    file(WRITE "${output}.tmp" "// Generated from third_party/ograf/json-schemas by cmake/EmbedSchemas.cmake — do not edit.
#pragma once
#include <string_view>
#include <utility>

namespace mbs::ograf
{
${arrays}
    /// A schema file by its path below json-schemas/ (graphics/schema.json); empty when unknown.
    inline std::string_view schemaFile(std::string_view rel)
    {
        static std::pair<std::string_view, std::string_view> const kFiles[] = {
${entries}        };
        for (auto const& [name, text] : kFiles)
        {
            if (name == rel)
            {
                return text;
            }
        }
        return {};
    }
}
")
    file(COPY_FILE "${output}.tmp" "${output}" ONLY_IF_DIFFERENT)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${files})
endfunction()
