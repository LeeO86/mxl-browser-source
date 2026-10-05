cmake_minimum_required(VERSION 3.24 FATAL_ERROR)

project(mxl-browser-source
    VERSION 0.1.0
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
    src/mxlio/setup.cpp
    src/util/logging.cpp
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
        tests/unit/test_convert.cpp
    )
    target_link_libraries(unit-tests PRIVATE mbs-core)
    add_test(NAME unit-tests COMMAND unit-tests)
endif()
