cmake_minimum_required(VERSION 3.24 FATAL_ERROR)

project(mxl-browser-source
    VERSION 1.1.0
    DESCRIPTION "MXL media function: a web page rendered offscreen with CEF"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(MBS_BUILD_TESTS "Build unit tests" ON)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SAMPLERATE REQUIRED IMPORTED_TARGET samplerate)

add_compile_options(-Wall -Wextra)

# ------------------------------------------------------------------
# Core: everything without CEF and libmxl, so unit tests run anywhere
# (SPEC §17). The binary targets x86-64-v2; only src/convert/avx2.cpp
# uses AVX2, through a target attribute, chosen at runtime.
# ------------------------------------------------------------------
add_library(mbs-core STATIC
    src/audio/fifo.cpp
    src/audio/resampler.cpp
    src/config/config.cpp
    src/config/document.cpp
    src/convert/avx2.cpp
    src/convert/convert.cpp
    src/engine/engine.cpp
    src/engine/framestore.cpp
    src/engine/tick.cpp
    src/mxlio/setup.cpp
    src/nmos/ids.cpp
    src/ops/httpserver.cpp
    src/ops/interact.cpp
    src/ops/keymap.cpp
    src/ops/urlpolicy.cpp
    src/util/logging.cpp
    src/util/metrics.cpp
    src/util/net.cpp
    src/util/proc.cpp
    src/util/uuid.cpp
)
target_include_directories(mbs-core PUBLIC src third_party)
target_link_libraries(mbs-core PUBLIC PkgConfig::SAMPLERATE Threads::Threads)
target_compile_definitions(mbs-core PUBLIC MBS_VERSION="${PROJECT_VERSION}")

if(MBS_BUILD_TESTS)
    enable_testing()
    add_executable(unit-tests
        tests/unit/test_main.cpp
        tests/unit/test_audio.cpp
        tests/unit/test_config.cpp
        tests/unit/test_convert.cpp
        tests/unit/test_engine.cpp
        tests/unit/test_framestore.cpp
        tests/unit/test_httpserver.cpp
        tests/unit/test_ids.cpp
        tests/unit/test_interact.cpp
        tests/unit/test_keymap.cpp
        tests/unit/test_proc.cpp
        tests/unit/test_setup.cpp
        tests/unit/test_tick.cpp
        tests/unit/test_urlpolicy.cpp
    )
    target_link_libraries(unit-tests PRIVATE mbs-core)
    add_test(NAME unit-tests COMMAND unit-tests)
endif()

# ------------------------------------------------------------------
# The process: libmxl, nmos-cpp and CEF (SPEC §3). Built in docker/Dockerfile,
# which provides CEF_ROOT (the minimal distribution with libcef_dll_wrapper
# built in build/), MXL in CMAKE_PREFIX_PATH and NMOS_CPP_DIR.
# ------------------------------------------------------------------
option(MBS_WITH_CEF "Build mxl-browser-source and its helper (needs CEF_ROOT, MXL, nmos-cpp)" OFF)
set(CEF_ROOT "" CACHE PATH "CEF binary distribution (include/, Release/, build/libcef_dll_wrapper/)")
set(NMOS_CPP_DIR "" CACHE PATH "sony/nmos-cpp Development/")
set(MBS_MXL_REVISION "unknown" CACHE STRING "MXL commit, reported in /api/v1/info")
set(MBS_NMOS_CPP_REF "unknown" CACHE STRING "nmos-cpp commit, reported in /api/v1/info")

if(MBS_WITH_CEF)
    if(NOT CEF_ROOT OR NOT NMOS_CPP_DIR)
        message(FATAL_ERROR "MBS_WITH_CEF=ON needs -DCEF_ROOT and -DNMOS_CPP_DIR")
    endif()
    find_package(mxl CONFIG REQUIRED)

    add_library(cef_wrapper STATIC IMPORTED)
    set_target_properties(cef_wrapper PROPERTIES IMPORTED_LOCATION ${CEF_ROOT}/build/libcef_dll_wrapper/libcef_dll_wrapper.a)
    add_library(cef_lib SHARED IMPORTED)
    set_target_properties(cef_lib PROPERTIES IMPORTED_LOCATION ${CEF_ROOT}/Release/libcef.so)
    add_library(mbs-cef STATIC src/cef/runtime.cpp)
    target_include_directories(mbs-cef PUBLIC ${CEF_ROOT})
    target_link_libraries(mbs-cef PUBLIC mbs-core cef_wrapper cef_lib ${CMAKE_DL_LIBS})

    add_library(mbs-mxl STATIC
        src/mxlio/audiowriter.cpp
        src/mxlio/domain.cpp
        src/mxlio/flowdef.cpp
        src/mxlio/videowriter.cpp
    )
    target_link_libraries(mbs-mxl PUBLIC mbs-core mxl::mxl)

    # The web UI, prebuilt by the Dockerfile's webui stage, embedded as one page.
    set(WEBUI_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/webui_index.hpp)
    add_custom_command(
        OUTPUT ${WEBUI_HEADER}
        COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/web/dist/index.html -DOUTPUT=${WEBUI_HEADER} -DVAR=IndexHtml
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake
        DEPENDS web/dist/index.html cmake/EmbedFile.cmake
        COMMENT "Embedding the web UI"
    )

    # websocketpp (pulled in by nmos-cpp) does not parse as C++20.
    set(NMOS_CPP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(NMOS_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(NMOS_CPP_USE_SUPPLIED_JSON_SCHEMA_VALIDATOR ON CACHE BOOL "" FORCE)
    set(NMOS_CPP_USE_SUPPLIED_JWT_CPP ON CACHE BOOL "" FORCE)
    set(NMOS_CPP_USE_AVAHI OFF CACHE BOOL "" FORCE)
    set(CMAKE_CXX_STANDARD 17)
    add_subdirectory(${NMOS_CPP_DIR} ${CMAKE_BINARY_DIR}/nmos-cpp EXCLUDE_FROM_ALL)
    set(CMAKE_CXX_STANDARD 20)

    add_executable(mxl-browser-source
        src/main.cpp
        src/app/api.cpp
        src/app/application.cpp
        src/nmos/node.cpp
        ${WEBUI_HEADER}
    )
    target_include_directories(mxl-browser-source PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_compile_definitions(mxl-browser-source PRIVATE MBS_MXL_REVISION="${MBS_MXL_REVISION}" MBS_NMOS_CPP_REF="${MBS_NMOS_CPP_REF}")
    target_link_libraries(mxl-browser-source PRIVATE mbs-cef mbs-mxl nmos-cpp::nmos-cpp)
    # nmos-cpp's headers warn a lot under -Wall.
    set_source_files_properties(src/nmos/node.cpp PROPERTIES COMPILE_OPTIONS "-w")
    set_target_properties(mxl-browser-source PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")

    add_executable(grain_reader tests/tools/grain_reader.cpp)
    target_link_libraries(grain_reader PRIVATE mxl::mxl Threads::Threads)

    add_executable(mxl-browser-source-helper helper/main.cpp)
    target_link_libraries(mxl-browser-source-helper PRIVATE mbs-cef)
    set_target_properties(mxl-browser-source-helper PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
endif()
