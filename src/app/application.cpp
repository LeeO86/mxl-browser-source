// SPDX-License-Identifier: MIT
#include "app/application.hpp"

#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "app/mxladapters.hpp"
#include "mxlio/domain.hpp"
#include "mxlio/flowdef.hpp"
#include "mxlio/setup.hpp"
#include "nmos/node.hpp"
#include "picojson/picojson.h"
#include "util/logging.hpp"
#include "util/net.hpp"
#include "util/uuid.hpp"

namespace mbs::app
{
    namespace
    {
        namespace fs = std::filesystem;
        using Clock = std::chrono::steady_clock;

        int gSignalPipe[2] = {-1, -1};

        void onSignal(int)
        {
            char const byte = 1;
            [[maybe_unused]] auto const n = ::write(gSignalPipe[1], &byte, 1);
        }

        std::string json(std::string const& text)
        {
            return picojson::value(text).serialize();
        }

        std::string percentEncode(std::string const& text)
        {
            static char const* hex = "0123456789ABCDEF";
            std::string out;
            for (unsigned char c : text)
            {
                if (std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == '~')
                {
                    out.push_back(static_cast<char>(c));
                }
                else
                {
                    out += '%';
                    out += hex[c >> 4];
                    out += hex[c & 15];
                }
            }
            return out;
        }

        util::Uuid uuid(std::string const& text)
        {
            return util::parseUuid(text).value_or(util::Uuid{});
        }

        // Runs a program without a shell; its exit code, or -1.
        int runProgram(std::vector<std::string> const& args)
        {
            pid_t const pid = ::fork();
            if (pid == 0)
            {
                std::vector<char*> argv;
                for (auto const& a : args)
                {
                    argv.push_back(const_cast<char*>(a.c_str()));
                }
                argv.push_back(nullptr);
                int const devnull = ::open("/dev/null", O_RDWR);
                ::dup2(devnull, 1);
                ::dup2(devnull, 2);
                ::execvp(argv[0], argv.data());
                ::_exit(127);
            }
            if (pid < 0)
            {
                return -1;
            }
            int status = 0;
            ::waitpid(pid, &status, 0);
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }

        // A GPU the NVIDIA driver serves to this container (SPEC §5.5 auto).
        bool gpuVisible()
        {
            if (!fs::exists("/dev/nvidia0") && !fs::exists("/dev/nvidiactl"))
            {
                return false;
            }
            void* lib = ::dlopen("libEGL_nvidia.so.0", RTLD_NOW | RTLD_LOCAL);
            if (lib == nullptr)
            {
                return false;
            }
            ::dlclose(lib);
            return true;
        }

        constexpr std::chrono::seconds kCrashBackoff[] = {std::chrono::seconds(1), std::chrono::seconds(2), std::chrono::seconds(5),
            std::chrono::seconds(10), std::chrono::seconds(30)};
        constexpr std::size_t kMaxCrashes = 5;
        constexpr auto kCrashWindow = std::chrono::minutes(10);
        constexpr char const* kSlate = "https://templates.local/error.html";
    }

    Application::Application(config::Loaded loaded, std::unique_ptr<config::Document> document, int argc, char** argv)
        : _cfg(std::move(loaded.config))
        , _table(std::move(loaded.table))
        , _doc(std::move(document))
        , _argc(argc)
        , _argv(argv)
    {
    }

    Application::~Application()
    {
        for (auto& t : _threads)
        {
            if (t.joinable())
            {
                t.join();
            }
        }
    }

    char const* Application::statusName(PageStatus status)
    {
        switch (status)
        {
        case PageStatus::Loading: return "loading";
        case PageStatus::Loaded: return "loaded";
        case PageStatus::Error: return "error";
        case PageStatus::Crashed: return "crashed";
        case PageStatus::Hung: return "hung";
        }
        return "loading";
    }

    // ------------------------------------------------------------------------------------
    // Startup (SPEC §13): settings → tmpfs → state dir → own domain → RLIMIT_NOFILE → CA,
    // cache → CefInitialize → browser → writers → ports and NMOS listener → tick → register.

    int Application::run()
    {
        ::pipe2(gSignalPipe, O_CLOEXEC);
        struct sigaction action{};
        action.sa_handler = onSignal;
        sigemptyset(&action.sa_mask);
        ::sigaction(SIGTERM, &action, nullptr);
        ::sigaction(SIGINT, &action, nullptr);

        prepareState();
        prepareMxl();
        prepareBrowserEnvironment();
        startCef();
        startHttp();
        startNmos();
        startThreads();
        log::info("started", {{"web_port", _cfg.webPort}, {"nmos_port", _cfg.nmosPort}, {"format", _cfg.format.name},
                                 {"key_mode", config::toString(_cfg.keyMode)}, {"node_id", _ids.node}});

        // SIGTERM/SIGINT: the shutdown starts on its own thread while the loop keeps running,
        // because closing the browser needs the UI thread.
        _threads.emplace_back([this] {
            char byte = 0;
            while (::read(gSignalPipe[0], &byte, 1) < 0 && errno == EINTR)
            {
            }
            shutdown();
        });
        cef::runMessageLoop();

        // The loop ended: the browser is closed (or the close timed out).
        cef::shutdown();
        if (_node)
        {
            _node->stop(); // the registry gets the DELETEs, node last
        }
        if (_cfg.mxlCleanupOnExit)
        {
            _outputs.reset();
            _domain.reset();
            if (mxlio::removeDomain(_cfg.mxlScanPath, _domainDir))
            {
                log::info("domain_removed", {{"path", _domainDir}});
            }
        }
        log::info("stopped", {{"exit_code", 143}});
        return 143;
    }

    void Application::prepareState()
    {
        std::error_code ec;
        for (auto const& dir : {_cfg.stateDir, _cfg.templatesDir})
        {
            fs::create_directories(dir, ec);
            if (ec || !fs::is_directory(dir))
            {
                throw StartupError(75, "state directory " + dir + " cannot be created: " + ec.message());
            }
        }
        _ids = nmos::makeIds(_cfg.nmosSeed, _cfg.format.name, config::toString(_cfg.keyMode), _cfg.audioChannels);
        _domainDir = _cfg.mxlOutputDomainDir.empty() ? _cfg.mxlScanPath + "/browser-source-" + _cfg.nmosSeed : _cfg.mxlOutputDomainDir;
        _hostAddress = _cfg.nmosHostAddress.empty() ? util::primaryIpv4() : _cfg.nmosHostAddress;
        if (_hostAddress.empty())
        {
            throw StartupError(78, "NMOS_HOST_ADDRESS: no non-loopback IPv4 address found, set it");
        }
    }

    void Application::prepareMxl()
    {
        mxlio::DomainSetup setup;
        try
        {
            setup = mxlio::prepareDomain(_cfg.mxlScanPath, _domainDir, _cfg.mxlOutputDomainId.empty() ? _ids.domain : _cfg.mxlOutputDomainId,
                _cfg.label, _cfg.mxlHistoryNs, _cfg.requireTmpfs);
        }
        catch (mxlio::RootError const& ex)
        {
            log::error("mxl_root_not_tmpfs",
                {{"path", _cfg.mxlScanPath}, {"error", ex.what()},
                    {"fix", "mount a tmpfs at " + _cfg.mxlScanPath + " or set BROWSER_REQUIRE_TMPFS=false for tests"}});
            throw StartupError(78, ex.what());
        }
        catch (std::exception const& ex)
        {
            throw StartupError(78, std::string("own MXL domain: ") + ex.what());
        }
        _domainId = setup.id;
        if (setup.mismatch)
        {
            log::warn("domain_id_mismatch", {{"path", _domainDir}, {"kept", setup.id}});
        }
        mxlio::raiseFileLimit();

        try
        {
            _domain = std::make_unique<mxlio::Domain>(_domainDir);
            _domain->garbageCollect();
            auto const label = _cfg.label;
            mxlio::VideoFlowParams video;
            video.id = uuid(_ids.videoFlow);
            video.label = label + " Video";
            video.description = "mxl-browser-source video";
            video.groupHint = label + ":Video";
            video.sourceId = uuid(_ids.videoSource);
            video.deviceId = uuid(_ids.device);
            video.width = _cfg.format.width;
            video.height = _cfg.format.height;
            video.rateNumerator = _cfg.format.rateNum;
            video.rateDenominator = _cfg.format.rateDen;
            video.withAlpha = _cfg.keyMode == config::KeyMode::V210a;
            auto videoWriter = std::make_unique<mxlio::VideoWriter>(*_domain, video, 1);
            std::unique_ptr<mxlio::VideoWriter> keyWriter;
            if (_cfg.keyMode == config::KeyMode::FillKey)
            {
                mxlio::VideoFlowParams key = video;
                key.id = uuid(_ids.keyFlow);
                key.label = label + " Key";
                key.description = "mxl-browser-source key (alpha as luma)";
                key.groupHint = label + ":Key";
                key.sourceId = uuid(_ids.keySource);
                key.withAlpha = false;
                keyWriter = std::make_unique<mxlio::VideoWriter>(*_domain, key, 1);
            }
            std::unique_ptr<mxlio::AudioWriter> audioWriter;
            if (_cfg.audioChannels > 0)
            {
                mxlio::AudioFlowParams audio;
                audio.id = uuid(_ids.audioFlow);
                audio.label = label + " Audio";
                audio.description = "mxl-browser-source audio";
                audio.groupHint = label + ":Audio";
                audio.sourceId = uuid(_ids.audioSource);
                audio.deviceId = uuid(_ids.device);
                audio.channelCount = static_cast<std::uint32_t>(_cfg.audioChannels);
                audioWriter = std::make_unique<mxlio::AudioWriter>(*_domain, audio, 1);
            }
            _outputs = std::make_unique<MxlOutputs>(std::move(videoWriter), std::move(keyWriter), std::move(audioWriter));
            _writersOpen.store(true);
        }
        catch (std::exception const& ex)
        {
            throw StartupError(75, std::string("MXL writers: ") + ex.what());
        }

        _clock = std::make_unique<MxlClock>(_cfg.format.rateNum, _cfg.format.rateDen);
        engine::EngineSettings es;
        es.width = _cfg.format.width;
        es.height = _cfg.format.height;
        es.alphaPlane = _cfg.keyMode == config::KeyMode::V210a;
        es.keyFlow = _cfg.keyMode == config::KeyMode::FillKey;
        es.straightFill = _cfg.fill == config::Fill::Straight;
        es.audioChannels = static_cast<std::uint32_t>(_cfg.audioChannels);
        es.audioTargetFrames = static_cast<std::uint32_t>(_cfg.audioBufferMs * 48);
        double const periodMs = 1000.0 * static_cast<double>(_cfg.format.rateDen) / static_cast<double>(_cfg.format.rateNum);
        if (_cfg.videoDelayGrains >= 0)
        {
            es.videoDelayGrains = static_cast<std::uint32_t>(_cfg.videoDelayGrains);
        }
        else
        {
            // auto (SPEC §6): audio reaches MXL after the FIFO target plus Chromium's 10 ms
            // packet, video after the BeginFrame lead; delay video by the difference.
            double const audioMs = _cfg.audioChannels > 0 ? _cfg.audioBufferMs + 10.0 : 0.0;
            double const videoMs = periodMs * _cfg.frameLead;
            es.videoDelayGrains = static_cast<std::uint32_t>(std::max(0L, std::lround((audioMs - videoMs) / periodMs)));
        }
        es.lateNs = static_cast<std::uint64_t>(periodMs * 1e6);
        switch (_cfg.onPageError)
        {
        case config::PageErrorMode::Transparent: es.onPageError = engine::Substitute::Transparent; break;
        case config::PageErrorMode::Black:
        case config::PageErrorMode::Slate: es.onPageError = engine::Substitute::Black; break;
        default: es.onPageError = engine::Substitute::Hold; break;
        }
        es.previewWidth = static_cast<std::uint32_t>(_cfg.previewWidth);
        _engine = std::make_unique<engine::Engine>(es, *_clock, *_outputs, _metrics);
        _engine->setAvOffsetMs(_cfg.avOffsetMs);
        for (auto const& [name, enabled] : _doc->senders())
        {
            _engine->setEnabled(name == "key" ? engine::Flow::Key : name == "audio" ? engine::Flow::Audio : engine::Flow::Video, enabled);
        }
        log::info("mxl_ready", {{"domain", _domainDir}, {"domain_id", _domainId}, {"video_delay_grains", es.videoDelayGrains}});
    }

    void Application::prepareBrowserEnvironment()
    {
        std::error_code ec;
        std::string home = std::getenv("HOME") != nullptr ? std::getenv("HOME") : "/tmp/home";
        if (home.empty() || home == "/" || ::access(home.c_str(), W_OK) != 0)
        {
            home = "/tmp/home";
            ::setenv("HOME", home.c_str(), 1);
        }
        fs::create_directories(home, ec);

        // Chromium trusts its NSS database, not SSL_CERT_FILE (SPEC §14.5).
        if (fs::is_directory(_cfg.caDir, ec))
        {
            auto const db = "sql:" + home + "/.pki/nssdb";
            fs::create_directories(home + "/.pki/nssdb", ec);
            if (!fs::exists(home + "/.pki/nssdb/cert9.db"))
            {
                runProgram({"certutil", "-N", "--empty-password", "-d", db});
            }
            for (auto const& entry : fs::directory_iterator(_cfg.caDir, ec))
            {
                auto const ext = entry.path().extension().string();
                if (!entry.is_regular_file() || (ext != ".pem" && ext != ".crt"))
                {
                    continue;
                }
                int const rc = runProgram({"certutil", "-A", "-d", db, "-t", "C,,", "-n", entry.path().stem().string(), "-i", entry.path().string()});
                log::info("ca_imported", {{"file", entry.path().string()}, {"ok", rc == 0}});
            }
        }
        // Extra fonts: fontconfig reads $XDG_DATA_HOME/fonts (~/.local/share/fonts).
        if (fs::is_directory(_cfg.fontsDir, ec))
        {
            fs::create_directories(home + "/.local/share", ec);
            fs::remove(home + "/.local/share/fonts", ec);
            fs::create_directory_symlink(_cfg.fontsDir, home + "/.local/share/fonts", ec);
        }
        // Profile (SPEC §13): ephemeral is cleared at start; persistent loses stale locks.
        if (_cfg.persistentProfile)
        {
            _cachePath = _cfg.stateDir + "/profile";
            fs::create_directories(_cachePath, ec);
            for (auto const* lock : {"SingletonLock", "SingletonSocket", "SingletonCookie"})
            {
                fs::remove(_cachePath + "/" + lock, ec);
            }
        }
        else
        {
            _cachePath = "/tmp/mxl-browser-source-cache";
            fs::remove_all(_cachePath, ec);
            fs::create_directories(_cachePath, ec);
        }
    }

    void Application::startCef()
    {
        if (_cfg.render == config::Render::Gpu)
        {
            _renderRequested = cef::RenderMode::Gpu;
        }
        else if (_cfg.render == config::Render::Auto)
        {
            _renderRequested = gpuVisible() ? cef::RenderMode::Gpu : cef::RenderMode::Software;
            if (_renderRequested == cef::RenderMode::Software)
            {
                log::warn("render_software_fallback", {{"reason", "no NVIDIA GPU visible (/dev/nvidia*, libEGL_nvidia.so.0)"}});
            }
        }
        cef::RuntimeSettings rs;
        rs.argc = _argc;
        rs.argv = _argv;
        rs.render = _renderRequested;
        auto const self = fs::read_symlink("/proc/self/exe").parent_path();
        rs.helperPath = (self / "mxl-browser-source-helper").string();
        rs.resourcesDir = self.string();
        rs.cachePath = _cachePath;
        auto const source = _doc->source();
        rs.userAgentSuffix = source.userAgentSuffix.empty() ? std::string("mxl-browser-source/") + MBS_VERSION : source.userAgentSuffix;
        rs.proxyServer = _cfg.httpsProxy;
        rs.proxyBypass = _cfg.noProxy;
        if (_cfg.devtools)
        {
            rs.devtoolsPort = _cfg.devtoolsPort;
            rs.devtoolsOrigin = "http://127.0.0.1:" + std::to_string(_cfg.devtoolsPort); // the tunnel rewrites Origin to this
        }
        rs.extraFlags = _cfg.chromiumFlagsAppend;
        rs.templatesDir = _cfg.templatesDir;
        std::string switches;
        for (auto const& s : cef::switchesFor(rs))
        {
            switches += (switches.empty() ? "--" : " --") + s;
        }
        log::info("cef_switches", {{"render", _renderRequested == cef::RenderMode::Gpu ? "gpu" : "software"}, {"switches", switches}});
        if (!cef::initialize(rs))
        {
            throw StartupError(75, "CefInitialize failed");
        }

        ops::UrlPolicySettings policy;
        policy.allow = _cfg.urlAllow;
        policy.deny = _cfg.urlDeny;
        policy.registryHost = _cfg.nmosRegistryAddress;
        policy.ownHost = _hostAddress;
        policy.ownPorts = {_cfg.webPort, _cfg.nmosPort, _cfg.nmosPort + 1, _cfg.devtoolsPort};
        cef::BrowserSettings bs;
        bs.url = source.url;
        bs.width = static_cast<int>(_cfg.format.width);
        bs.height = static_cast<int>(_cfg.format.height);
        bs.deviceScaleFactor = source.deviceScaleFactor;
        bs.frameRate = static_cast<int>(std::ceil(static_cast<double>(_cfg.format.rateNum) / static_cast<double>(_cfg.format.rateDen)));
        bs.background = config::parseBackground(source.background).value_or(0);
        bs.popupsSameWindow = _cfg.popupsSameWindow;
        bs.confirmAccept = _cfg.confirmAccept;
        _browser = std::make_unique<cef::Browser>(bs, *this, ops::UrlPolicy(policy));
        {
            std::lock_guard lock{_pageMutex};
            _url = source.url;
        }
        _engine->setRequestFrame([this] {
            if (_browserReady.load())
            {
                _engine->beginFrameSent();
                _browser->beginFrame();
            }
        });
        _engine->setAudioActive(false);
        _browser->create();

        ops::InteractSettings is;
        is.viewWidth = static_cast<int>(std::lround(_cfg.format.width / std::max(0.25, source.deviceScaleFactor)));
        is.viewHeight = static_cast<int>(std::lround(_cfg.format.height / std::max(0.25, source.deviceScaleFactor)));
        is.timeoutS = _cfg.interactTimeoutS;
        is.previewFps = _cfg.previewFps;
        _interact = std::make_unique<ops::InteractHub>(is, *_browser);
        _interact->setStateParts([this] { return pageJson(); }, [this] { return renderJson(); });
        _interact->setGrainSource([this] { return _engine->nextVisibleIndex(); });
    }

    void Application::startHttp()
    {
        _http = std::make_unique<ops::HttpServer>([this](ops::HttpRequest const& req, ops::HttpResponse& res) { handle(req, res); },
            [this](ops::HttpRequest const& req, ops::HttpResponse& reject) { return upgrade(req, reject); },
            [this](int fd, ops::HttpRequest const& req) { return devtoolsTunnel(fd, req); });
        std::string error;
        if (!_http->start(_cfg.webPort, error))
        {
            throw StartupError(75, "WEB_PORT: " + error);
        }
    }

    void Application::startNmos()
    {
        nmos::NodeSettings ns;
        ns.ids = _ids;
        ns.port = _cfg.nmosPort;
        ns.label = _cfg.nmosLabel;
        ns.senderLabel = _cfg.label;
        ns.tags = _cfg.nmosTags;
        ns.hostAddress = _hostAddress;
        ns.registryAddress = _cfg.nmosRegistryAddress;
        ns.registryPort = _cfg.nmosRegistryPort;
        ns.dnsSd = _cfg.nmosDnsSd;
        ns.domainId = _domainId;
        ns.width = _cfg.format.width;
        ns.height = _cfg.format.height;
        ns.rateNum = _cfg.format.rateNum;
        ns.rateDen = _cfg.format.rateDen;
        ns.v210a = _cfg.keyMode == config::KeyMode::V210a;
        ns.keyFlow = _cfg.keyMode == config::KeyMode::FillKey;
        ns.audioChannels = _cfg.audioChannels;
        ns.enabled = _doc->senders();
        ns.shutdownTimeoutS = _cfg.shutdownTimeoutS;
        _node = std::make_unique<nmos::Node>(ns, [this](std::string const& sender, bool enabled) {
            _engine->setEnabled(sender == "key" ? engine::Flow::Key : sender == "audio" ? engine::Flow::Audio : engine::Flow::Video, enabled);
            _doc->setSender(sender, enabled); // IS-05 state survives a restart (G9)
            _metrics.set("sender_enabled", {{"sender", sender}}, enabled ? 1 : 0);
        });
        try
        {
            _node->start();
        }
        catch (std::exception const& ex)
        {
            throw StartupError(75, ex.what());
        }
    }

    void Application::startThreads()
    {
        _engine->start();
        _threads.emplace_back([this] { supervisorLoop(); });
        _threads.emplace_back([this] { eventsLoop(); });
        _threads.emplace_back([this] { previewLoop(); });
        _threads.emplace_back([this] { readinessLoop(); });
    }

    // SIGTERM/SIGINT (SPEC §13): API off → tick off after the current grain → writers closed →
    // CloseBrowser → (main thread) CefShutdown → NMOS DELETEs → own domain → 143. A watchdog
    // ends the process at SHUTDOWN_TIMEOUT_S, still with 143.
    void Application::shutdown()
    {
        if (_stopping.exchange(true))
        {
            return;
        }
        log::info("shutdown", {{"timeout_s", _cfg.shutdownTimeoutS}});
        std::thread([seconds = _cfg.shutdownTimeoutS] {
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            log::error("shutdown_timeout", {{"seconds", seconds}});
            ::_exit(143);
        }).detach();
        if (_http)
        {
            _http->stop();
        }
        if (_engine)
        {
            _engine->stop();
        }
        _writersOpen.store(false);
        _outputs.reset(); // the writers close; readers see no new grains
        if (_browser && _browser->exists())
        {
            _browser->close();
            std::unique_lock lock{_closeMutex};
            _closed.wait_for(lock, std::chrono::seconds(2), [this] { return _browserClosed; });
        }
        cef::quit();
    }

    // ------------------------------------------------------------------------------------
    // The page

    void Application::onBrowserCreated()
    {
        _browserReady.store(true);
        _browser->invalidate();
        log::info("browser_created", {});
    }

    void Application::onBrowserClosed()
    {
        _browserReady.store(false);
        {
            std::lock_guard lock{_closeMutex};
            _browserClosed = true;
        }
        _closed.notify_all();
        if (!_stopping.load())
        {
            // Closed by the hang recovery: open the current page in a new browser.
            {
                std::lock_guard lock{_closeMutex};
                _browserClosed = false;
            }
            std::string url;
            {
                std::lock_guard lock{_pageMutex};
                url = _url;
            }
            _browser->setStartUrl(url);
            _browser->create();
        }
    }

    void Application::onPaint(void const* bgra, int stride, int width, int height)
    {
        _engine->paint(static_cast<std::uint8_t const*>(bgra), static_cast<std::size_t>(stride), static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
    }

    void Application::onAudioStarted(int channels, int sampleRate)
    {
        if (sampleRate != 48000)
        {
            log::warn("audio_rate_unexpected", {{"rate", sampleRate}});
        }
        bool active = false;
        {
            std::lock_guard lock{_pageMutex};
            _audioStream = true;
            active = _doc->source().audio;
        }
        _engine->setAudioActive(active);
        log::info("audio_stream_started", {{"channels", channels}});
    }

    void Application::onAudioPacket(float const* const* planes, int channels, int frames)
    {
        _engine->pushAudio(planes, channels, static_cast<std::size_t>(frames));
    }

    void Application::onAudioStopped()
    {
        {
            std::lock_guard lock{_pageMutex};
            _audioStream = false;
        }
        _engine->setAudioActive(false);
    }

    void Application::onLoadStart(std::string const& url)
    {
        bool const slate = url.rfind(kSlate, 0) == 0;
        {
            std::lock_guard lock{_pageMutex};
            if (slate)
            {
                return; // the error page keeps the error state
            }
            _url = url;
            _slateShown = false;
            _loadStarted = Clock::now();
        }
        setPage(PageStatus::Loading, "");
    }

    void Application::onLoadEnd(std::string const& url, int httpStatus)
    {
        if (url.rfind(kSlate, 0) == 0)
        {
            {
                std::lock_guard lock{_pageMutex};
                _slateShown = true;
            }
            updateEngineState();
            _browser->invalidate();
            return;
        }
        double seconds = 0;
        std::string title;
        bool probeRenderer = false;
        {
            std::lock_guard lock{_pageMutex};
            seconds = std::chrono::duration<double>(Clock::now() - _loadStarted).count();
            ++_loads;
            _url = url;
            title = _title;
            _lastPeriodicReload = Clock::now(); // reload_interval_s counts from the load
            probeRenderer = _renderActual == "unknown";
        }
        _metrics.inc("page_loads_total", {{"result", httpStatus >= 400 ? "error" : "ok"}});
        _metrics.observe("page_load_seconds", {}, seconds);
        setPage(PageStatus::Loaded, "");
        injectAfterLoad();
        _browser->invalidate(); // spike S2: one paint of the loaded page even if it is static
        if (probeRenderer)
        {
            // The renderer the page really gets (SPEC §5.5): WebGL's unmasked renderer string.
            std::lock_guard lock{_probeMutex};
            _rendererProbeId = _browser->devToolsMethod("Runtime.evaluate",
                R"js({"returnByValue":true,"expression":"(()=>{try{const g=document.createElement('canvas').getContext('webgl');if(!g)return 'none';const e=g.getExtension('WEBGL_debug_renderer_info');return String(e?g.getParameter(e.UNMASKED_RENDERER_WEBGL):g.getParameter(g.RENDERER));}catch(x){return 'error';}})()"})js");
        }
        broadcastEvent(R"({"type":"page","state":"loaded","url":)" + json(url) + R"(,"title":)" + json(title) + "}");
    }

    void Application::onLoadError(std::string const& url, int code, std::string const& text)
    {
        _metrics.inc("page_loads_total", {{"result", "error"}});
        log::warn("page_load_error", {{"url", url}, {"code", code}, {"error", text}});
        setPage(PageStatus::Error, text.empty() ? "error " + std::to_string(code) : text);
        if (_cfg.onPageError == config::PageErrorMode::Slate && url.rfind(kSlate, 0) != 0)
        {
            _browser->navigate(std::string(kSlate) + "#" + percentEncode(text + " (" + url + ")"));
        }
        broadcastEvent(R"({"type":"page","state":"error","url":)" + json(url) + R"(,"title":)" + json(text) + "}");
    }

    void Application::onTitle(std::string const& title)
    {
        std::lock_guard lock{_pageMutex};
        _title = title;
    }

    void Application::onAddress(std::string const& url)
    {
        if (url.rfind(kSlate, 0) == 0)
        {
            return;
        }
        std::lock_guard lock{_pageMutex};
        _url = url;
    }

    void Application::onConsole(std::string const& level, std::string const& message, std::string const& source, int line)
    {
        _metrics.inc("console_messages_total", {{"level", level}});
        auto const now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        char time[32] = {};
        std::strftime(time, sizeof(time), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now));
        std::string const entry = std::string(R"({"type":"console","time":")") + time + R"(","level":)" + json(level) + R"(,"message":)" +
                                  json(message.substr(0, 4096)) + R"(,"source":)" + json(source) + R"(,"line":)" + std::to_string(line) + "}";
        {
            std::lock_guard lock{_pageMutex};
            _console.push_back(entry);
            while (_console.size() > 200)
            {
                _console.pop_front();
            }
        }
        broadcastEvent(entry);
        _interact->broadcast(entry);
    }

    void Application::onCursor(std::string const& cursor)
    {
        _interact->broadcast(R"({"type":"cursor","cursor":)" + json(cursor) + "}");
    }

    void Application::onDialog(std::string const& kind, std::string const& message, std::string const& result)
    {
        _metrics.inc("js_dialogs_total", {{"type", kind}});
        auto const event = R"({"type":"dialog","kind":)" + json(kind) + R"(,"message":)" + json(message.substr(0, 4096)) + R"(,"result":)" + json(result) + "}";
        broadcastEvent(event);
        _interact->broadcast(event);
    }

    void Application::onPopup(std::string const& url, bool opened)
    {
        if (!opened)
        {
            _metrics.inc("popups_blocked_total");
        }
        broadcastEvent(R"({"type":"event","data":{"popup":)" + json(url) + R"(,"opened":)" + (opened ? "true" : "false") + "}}");
    }

    void Application::onDownloadBlocked(std::string const& url)
    {
        _metrics.inc("downloads_blocked_total");
        broadcastEvent(R"({"type":"event","data":{"download_blocked":)" + json(url) + "}}");
    }

    void Application::onPermissionDenied(std::string const& type)
    {
        _metrics.inc("permission_denied_total", {{"type", type}});
        broadcastEvent(R"({"type":"event","data":{"permission_denied":)" + json(type) + "}}");
    }

    void Application::onFileDialogCancelled()
    {
        broadcastEvent(R"({"type":"event","data":{"file_dialog":"cancelled"}})");
    }

    void Application::onNavigationBlocked(std::string const& url, std::string const& reason)
    {
        _metrics.inc("navigation_blocked_total", {{"reason", reason}});
        log::warn("navigation_blocked", {{"url", url.substr(0, 512)}, {"reason", reason}});
        broadcastEvent(R"({"type":"event","data":{"navigation_blocked":)" + json(url) + R"(,"reason":)" + json(reason) + "}}");
    }

    void Application::onRendererTerminated(std::string const& reason)
    {
        if (_stopping.load())
        {
            return;
        }
        _metrics.inc("renderer_crashes_total", {{"reason", reason}});
        auto const now = Clock::now();
        std::size_t crashes = 0;
        {
            std::lock_guard lock{_pageMutex};
            _crashes.push_back(now);
            while (!_crashes.empty() && now - _crashes.front() > kCrashWindow)
            {
                _crashes.pop_front();
            }
            crashes = _crashes.size();
        }
        {
            std::lock_guard lock{_probeMutex};
            _probeId = 0;
        }
        log::error("renderer_terminated", {{"reason", reason}, {"crashes_in_10_min", static_cast<int>(crashes)}});
        if (crashes >= kMaxCrashes)
        {
            {
                std::lock_guard lock{_pageMutex};
                _autoReloadBlocked = true;
            }
            setPage(PageStatus::Error, "renderer crashed " + std::to_string(crashes) + " times in 10 minutes; reload to try again");
            return;
        }
        setPage(PageStatus::Crashed, reason);
        scheduleReload(kCrashBackoff[std::min(crashes, std::size(kCrashBackoff)) - 1]);
    }

    void Application::onUnresponsive()
    {
        _metrics.inc("page_hangs_total");
        setPage(PageStatus::Hung, "renderer unresponsive");
        _browser->terminateRenderer();
    }

    void Application::onPagePost(std::string const& text)
    {
        picojson::value data;
        if (!picojson::parse(data, text).empty())
        {
            data = picojson::value(text);
        }
        broadcastEvent(R"({"type":"event","data":)" + data.serialize() + "}");
    }

    void Application::onDevToolsResult(int id, bool success, std::string const& text)
    {
        {
            std::lock_guard lock{_probeMutex};
            if (id == _probeId)
            {
                _probeId = 0; // the renderer answered: not hung
                return;
            }
            if (id != _rendererProbeId)
            {
                return;
            }
            _rendererProbeId = 0;
        }
        std::string name = "none";
        picojson::value doc;
        if (success && picojson::parse(doc, text).empty() && doc.is<picojson::object>())
        {
            auto const& result = doc.get<picojson::object>();
            if (auto r = result.find("result"); r != result.end() && r->second.is<picojson::object>())
            {
                auto const& value = r->second.get<picojson::object>();
                if (auto v = value.find("value"); v != value.end() && v->second.is<std::string>())
                {
                    name = v->second.get<std::string>();
                }
            }
        }
        std::string lowerName = name;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        bool const software = name == "none" || name == "error" || lowerName.find("swiftshader") != std::string::npos ||
                              lowerName.find("llvmpipe") != std::string::npos || lowerName.find("software") != std::string::npos;
        std::string const mode = software ? "software" : "gpu";
        {
            std::lock_guard lock{_pageMutex};
            _renderActual = mode;
            _rendererName = name;
        }
        _renderDegraded.store(_renderRequested == cef::RenderMode::Gpu && software);
        _metrics.clear("render_mode");
        _metrics.set("render_mode", {{"mode", mode}}, 1);
        _metrics.set("render_degraded", {}, _renderDegraded.load() ? 1 : 0);
        if (_renderDegraded.load())
        {
            log::error("render_degraded", {{"requested", "gpu"}, {"renderer", name}});
        }
        else
        {
            log::info("render_mode", {{"mode", mode}, {"renderer", name}});
        }
    }

    void Application::setPage(PageStatus status, std::string const& reason)
    {
        {
            std::lock_guard lock{_pageMutex};
            _page = status;
            _pageReason = reason;
        }
        _metrics.clear("page_state");
        _metrics.set("page_state", {{"state", statusName(status)}}, 1);
        updateEngineState();
        _interact->broadcastState();
    }

    void Application::updateEngineState()
    {
        engine::PageState state = engine::PageState::Loading;
        {
            std::lock_guard lock{_pageMutex};
            switch (_page)
            {
            case PageStatus::Loaded: state = engine::PageState::Ready; break;
            case PageStatus::Crashed: state = engine::PageState::Crashed; break;
            case PageStatus::Hung: state = engine::PageState::Hung; break;
            case PageStatus::Error: state = _slateShown ? engine::PageState::Ready : engine::PageState::Loading; break;
            default: state = engine::PageState::Loading; break;
            }
        }
        _engine->setPageState(state);
    }

    void Application::scheduleReload(std::chrono::milliseconds delay)
    {
        std::lock_guard lock{_pageMutex};
        _reloadAt = Clock::now() + delay;
    }

    // CSS and the background as style elements (replaced, never stacked), then the source's
    // JavaScript, then the template calls queued while the page loaded (CasparCG order).
    void Application::injectAfterLoad()
    {
        auto const source = _doc->source();
        std::string background;
        if (auto const argb = config::parseBackground(source.background); argb && (*argb >> 24) != 0)
        {
            char hex[8];
            std::snprintf(hex, sizeof(hex), "#%06X", *argb & 0xFFFFFFU);
            background = std::string("html,body{background:") + hex + " !important}";
        }
        _browser->executeJavaScript("(function(){var d=document;function s(id,css){var e=d.getElementById(id);if(!css){if(e)e.remove();return;}"
                                    "if(!e){e=d.createElement('style');e.id=id;(d.head||d.documentElement).appendChild(e);}e.textContent=css;}"
                                    "s('mxl-browser-source-background'," +
                                    json(background) + ");s('mxl-browser-source-css'," + json(source.css) + ");})();");
        if (!source.js.empty())
        {
            _browser->executeJavaScript(source.js);
        }
        std::deque<std::string> queued;
        {
            std::lock_guard lock{_pageMutex};
            queued.swap(_templateQueue);
        }
        for (auto const& call : queued)
        {
            _browser->executeJavaScript(call);
        }
    }

    void Application::queueTemplateCall(std::string const& js)
    {
        bool loaded = false;
        {
            std::lock_guard lock{_pageMutex};
            loaded = _page == PageStatus::Loaded;
            if (!loaded)
            {
                _templateQueue.push_back(js);
                while (_templateQueue.size() > 100)
                {
                    _templateQueue.pop_front();
                }
            }
        }
        if (loaded)
        {
            _browser->executeJavaScript(js);
        }
    }

    void Application::applySource(config::Source const& previous, config::Source const& next, bool navigate)
    {
        if (navigate || next.url != previous.url)
        {
            {
                std::lock_guard lock{_pageMutex};
                _autoReloadBlocked = false;
            }
            _browser->navigate(next.url);
        }
        if (next.zoom != previous.zoom)
        {
            _browser->setZoom(next.zoom);
        }
        if (next.deviceScaleFactor != previous.deviceScaleFactor)
        {
            _browser->setDeviceScaleFactor(next.deviceScaleFactor);
            double const dsf = std::max(0.25, next.deviceScaleFactor);
            _interact->setViewSize(static_cast<int>(std::lround(_cfg.format.width / dsf)), static_cast<int>(std::lround(_cfg.format.height / dsf)));
        }
        if (next.css != previous.css || next.background != previous.background || next.js != previous.js)
        {
            bool loaded = false;
            {
                std::lock_guard lock{_pageMutex};
                loaded = _page == PageStatus::Loaded;
            }
            if (loaded)
            {
                injectAfterLoad();
            }
        }
        if (next.audio != previous.audio)
        {
            bool stream = false;
            {
                std::lock_guard lock{_pageMutex};
                stream = _audioStream;
            }
            _engine->setAudioActive(stream && next.audio);
        }
        if (next.userAgentSuffix != previous.userAgentSuffix)
        {
            log::info("restart_required", {{"field", "user_agent_suffix"}});
        }
    }

    // Once a second: reload timers (crash backoff, reload_interval_s), the renderer liveness
    // probe, the interaction timeout, and the CEF processes' memory.
    void Application::supervisorLoop()
    {
        while (!_stopping.load())
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (_stopping.load())
            {
                break;
            }
            auto const now = Clock::now();
            auto const source = _doc->source();
            bool reload = false;
            PageStatus page = PageStatus::Loading;
            {
                std::lock_guard lock{_pageMutex};
                page = _page;
                if (_reloadAt && now >= *_reloadAt && !_autoReloadBlocked)
                {
                    _reloadAt.reset();
                    reload = true;
                }
                // reload_interval_s counts from the last completed load (onLoadEnd sets it).
                if (source.reloadIntervalS > 0 && page == PageStatus::Loaded && now - _lastPeriodicReload >= std::chrono::seconds(source.reloadIntervalS))
                {
                    _lastPeriodicReload = now;
                    reload = true;
                }
            }
            if (reload && _browserReady.load())
            {
                log::info("page_reload", {{"state", statusName(page)}});
                _browser->reload(false);
            }

            // Hang: the renderer's main thread did not answer a trivial DevTools evaluation
            // within BROWSER_HANG_TIMEOUT_MS (a static page paints nothing, so paints cannot tell).
            if (page == PageStatus::Loaded && _browserReady.load())
            {
                bool hung = false;
                {
                    std::lock_guard lock{_probeMutex};
                    if (_probeId != 0 && now - _probeSent > std::chrono::milliseconds(_cfg.hangTimeoutMs))
                    {
                        hung = true;
                        _probeId = 0;
                    }
                    else if (_probeId == 0)
                    {
                        _probeSent = now;
                        _probeId = _browser->devToolsMethod("Runtime.evaluate", R"({"expression":"1","returnByValue":true})");
                    }
                }
                if (hung)
                {
                    log::error("page_hung", {{"timeout_ms", _cfg.hangTimeoutMs}});
                    onUnresponsive();
                }
            }
            _interact->tick();
        }
    }

    // Four times a second: the status on /api/v1/events.
    void Application::eventsLoop()
    {
        while (!_stopping.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            bool any = false;
            {
                std::lock_guard lock{_socketsMutex};
                any = !_eventSockets.empty();
            }
            if (any)
            {
                broadcastEvent(statusJson());
            }
        }
    }

    void Application::broadcastEvent(std::string const& text)
    {
        std::vector<std::shared_ptr<ops::WebSocket>> sockets;
        {
            std::lock_guard lock{_socketsMutex};
            sockets.assign(_eventSockets.begin(), _eventSockets.end());
        }
        for (auto const& socket : sockets)
        {
            socket->sendText(text);
        }
    }

    // Preview frames for the interaction sessions at their rates (SPEC §7.3): one JPEG per new
    // frame, sent to each session that is due. Nothing is converted while nobody watches.
    void Application::previewLoop()
    {
        std::map<int, Clock::time_point> due;
        std::vector<std::uint8_t> rgb;
        std::uint64_t sequence = 0;
        while (!_stopping.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            auto const rates = _interact->previewRates();
            if (rates.empty())
            {
                due.clear();
                continue;
            }
            auto const now = Clock::now();
            std::vector<int> ready;
            for (auto const& [id, fps] : rates)
            {
                auto& next = due[id];
                if (fps > 0 && now >= next)
                {
                    ready.push_back(id);
                    next = now + std::chrono::microseconds(1'000'000 / fps);
                }
            }
            std::erase_if(due, [&](auto const& entry) { return rates.count(entry.first) == 0; });
            if (ready.empty())
            {
                continue;
            }
            _engine->requestPreview();
            for (int i = 0; i < 10 && !_engine->preview(rgb, sequence); ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            std::string jpeg;
            std::uint64_t grain = 0;
            {
                std::lock_guard lock{_jpegMutex};
                if (sequence != _jpegSequence && !rgb.empty())
                {
                    _jpeg = encodeJpeg(rgb, static_cast<int>(_engine->previewWidth()), static_cast<int>(_engine->previewHeight()));
                    _jpegSequence = sequence;
                    _jpegGrain = _clock->indexAt(_clock->nowNs());
                }
                jpeg = _jpeg;
                grain = _jpegGrain;
            }
            if (jpeg.empty())
            {
                continue;
            }
            std::string frame(8, '\0');
            for (int b = 0; b < 8; ++b)
            {
                frame[static_cast<std::size_t>(b)] = static_cast<char>((grain >> (8 * b)) & 0xFF);
            }
            frame += jpeg;
            std::vector<std::shared_ptr<ops::WebSocket>> targets;
            {
                std::lock_guard lock{_socketsMutex};
                for (int id : ready)
                {
                    if (auto it = _interactSockets.find(id); it != _interactSockets.end())
                    {
                        targets.push_back(it->second);
                    }
                }
            }
            for (auto const& socket : targets)
            {
                socket->sendBinary(frame);
            }
            _metrics.inc("preview_frames_total", {}, static_cast<double>(targets.size()));
        }
    }

    // Readiness (SPEC §10.5): with a registry, the node must be listed on the Query API.
    void Application::readinessLoop()
    {
        auto const query = _cfg.nmosQueryAddress.empty() ? _cfg.nmosRegistryAddress : _cfg.nmosQueryAddress;
        while (!_stopping.load())
        {
            if (!query.empty())
            {
                int const status = util::httpGet(query, _cfg.nmosQueryPort, "/x-nmos/query/v1.3/nodes/" + _ids.node, 2000);
                bool const registered = status == 200;
                if (registered != _registered.exchange(registered))
                {
                    log::info("nmos_query_visible", {{"visible", registered}});
                }
                _metrics.set("nmos_registered", {}, registered ? 1 : 0);
            }
            for (int i = 0; i < 20 && !_stopping.load(); ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }

    bool Application::ready(std::string* reason) const
    {
        auto fail = [&](char const* why) {
            if (reason != nullptr)
            {
                *reason = why;
            }
            return false;
        };
        if (_stopping.load())
        {
            return fail("stopping");
        }
        if (!_browserReady.load())
        {
            return fail("browser not created");
        }
        if (!_writersOpen.load())
        {
            return fail("MXL writers not open");
        }
        if (!_cfg.nmosRegistryAddress.empty() && !_registered.load())
        {
            return fail("node not on the NMOS Query API");
        }
        return true;
    }

    std::string Application::pageJson() const
    {
        std::lock_guard lock{_pageMutex};
        return R"({"url":)" + json(_url) + R"(,"title":)" + json(_title) + R"(,"state":")" + statusName(_page) + R"(","loading":)" +
               (_page == PageStatus::Loading ? "true" : "false") + R"(,"error":)" + json(_page == PageStatus::Error || _page == PageStatus::Crashed ? _pageReason : "") +
               R"(,"loads":)" + std::to_string(_loads) + "}";
    }

    std::string Application::renderJson() const
    {
        std::string mode;
        std::string renderer;
        {
            std::lock_guard lock{_pageMutex};
            mode = _renderActual;
            renderer = _rendererName;
        }
        return R"({"mode":")" + mode + R"(","renderer":)" + json(renderer) + R"(,"requested":")" +
               (_renderRequested == cef::RenderMode::Gpu ? "gpu" : "software") + R"(","degraded":)" + (_renderDegraded.load() ? "true" : "false") +
               R"(,"format":)" + json(_cfg.format.name) + R"(,"width":)" + std::to_string(_cfg.format.width) + R"(,"height":)" +
               std::to_string(_cfg.format.height) + "}";
    }
}
