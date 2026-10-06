// SPDX-License-Identifier: MIT
// mxl-browser-source: a web page rendered offscreen with CEF, written as MXL flows (SPEC.md).
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

#include "app/application.hpp"
#include "cef/runtime.hpp"
#include "config/config.hpp"
#include "config/document.hpp"
#include "util/logging.hpp"

namespace
{
    char const* kHelp = R"(mxl-browser-source — a web page rendered offscreen with CEF and written as MXL flows.

Configuration is environment, then the config file (BROWSER_CONFIG_FILE, default
<BROWSER_STATE_DIR>/config.json), then defaults; see README.md for every setting.

  --help      this text
  --version   version

Exit codes: 0 help/version, 75 a port, the state directory, CEF or MXL cannot start,
78 invalid configuration or MXL root not a tmpfs, 143 SIGTERM/SIGINT.
)";

    std::string hostname()
    {
        char buf[256] = {};
        return ::gethostname(buf, sizeof(buf) - 1) == 0 ? std::string(buf) : std::string("mxl-browser-source");
    }
}

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0)
        {
            std::fputs(kHelp, stdout);
            return 0;
        }
        if (std::strcmp(argv[i], "--version") == 0)
        {
            std::printf("mxl-browser-source %s\n", MBS_VERSION);
            return 0;
        }
    }
    // CEF subprocesses use the helper; this returns -1 in the browser process.
    if (int const code = mbs::cef::executeSubprocess(argc, argv); code >= 0)
    {
        return code;
    }

    auto env = mbs::config::processEnvironment();
    std::unique_ptr<mbs::config::Document> document;
    mbs::config::Loaded loaded;
    try
    {
        // The file's own location comes from the environment only.
        auto const stateDir = env.count("BROWSER_STATE_DIR") != 0 ? env["BROWSER_STATE_DIR"] : std::string("/config");
        auto const file = env.count("BROWSER_CONFIG_FILE") != 0 && !env["BROWSER_CONFIG_FILE"].empty() ? env["BROWSER_CONFIG_FILE"] : stateDir + "/config.json";
        document = std::make_unique<mbs::config::Document>(file);
        document->load();
        loaded = mbs::config::load(env, document->settings(), hostname());
        mbs::log::configure(mbs::log::parseLevel(loaded.config.logLevel), mbs::log::parseFormat(loaded.config.logFormat));
    }
    catch (mbs::config::ConfigError const& ex)
    {
        mbs::log::error("config_invalid", {{"key", ex.key()}, {"error", ex.what()}});
        return 78;
    }
    catch (std::exception const& ex)
    {
        mbs::log::error("config_invalid", {{"error", ex.what()}});
        return 78;
    }

    try
    {
        mbs::app::Application app(std::move(loaded), std::move(document), argc, argv);
        return app.run();
    }
    catch (mbs::app::StartupError const& ex)
    {
        mbs::log::error("startup_failed", {{"exit_code", ex.exitCode}, {"error", ex.what()}});
        std::fflush(nullptr);
        // CEF may already run threads that would block a normal exit.
        ::_exit(ex.exitCode);
    }
}
