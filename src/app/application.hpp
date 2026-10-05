// SPDX-License-Identifier: MIT
// The process (SPEC §13): startup order, the page's lifecycle and recovery, the HTTP API and
// WebSockets, NMOS, and the shutdown sequence.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "cef/observer.hpp"
#include "cef/runtime.hpp"
#include "config/config.hpp"
#include "config/document.hpp"
#include "engine/engine.hpp"
#include "nmos/ids.hpp"
#include "ops/httpserver.hpp"
#include "ops/interact.hpp"
#include "util/metrics.hpp"

namespace mbs::mxlio
{
    class Domain;
}

namespace mbs::nmos
{
    class Node;
}

namespace mbs::app
{
    class MxlClock;
    class MxlOutputs;

    /// A startup failure with its exit code (75 or 78).
    struct StartupError : std::runtime_error
    {
        StartupError(int code, std::string const& message)
            : std::runtime_error(message)
            , exitCode(code)
        {
        }
        int exitCode;
    };

    /// RGB24 → JPEG (stb), for the preview.
    std::string encodeJpeg(std::vector<std::uint8_t> const& rgb, int width, int height, int quality = 75);

    class Application final : public cef::PageObserver
    {
    public:
        Application(config::Loaded loaded, std::unique_ptr<config::Document> document, int argc, char** argv);
        ~Application() override;

        /// Starts everything, runs the CEF loop until SIGTERM/SIGINT, shuts down. Returns the
        /// exit code (143 after a signal); throws StartupError before the loop runs.
        int run();

        // cef::PageObserver
        void onBrowserCreated() override;
        void onBrowserClosed() override;
        void onPaint(void const* bgra, int stride, int width, int height) override;
        void onAudioStarted(int channels, int sampleRate) override;
        void onAudioPacket(float const* const* planes, int channels, int frames) override;
        void onAudioStopped() override;
        void onLoadStart(std::string const& url) override;
        void onLoadEnd(std::string const& url, int httpStatus) override;
        void onLoadError(std::string const& url, int code, std::string const& text) override;
        void onTitle(std::string const& title) override;
        void onAddress(std::string const& url) override;
        void onConsole(std::string const& level, std::string const& message, std::string const& source, int line) override;
        void onCursor(std::string const& cursor) override;
        void onDialog(std::string const& kind, std::string const& message, std::string const& result) override;
        void onPopup(std::string const& url, bool opened) override;
        void onDownloadBlocked(std::string const& url) override;
        void onPermissionDenied(std::string const& type) override;
        void onFileDialogCancelled() override;
        void onNavigationBlocked(std::string const& url, std::string const& reason) override;
        void onRendererTerminated(std::string const& reason) override;
        void onUnresponsive() override;
        void onPagePost(std::string const& json) override;
        void onDevToolsResult(int id, bool success, std::string const& json) override;

    private:
        enum class PageStatus
        {
            Loading,
            Loaded,
            Error,
            Crashed,
            Hung
        };

        // startup (application.cpp)
        void prepareState();
        void prepareMxl();
        void prepareBrowserEnvironment();
        void startCef();
        void startHttp();
        void startNmos();
        void startThreads();
        void shutdown();

        // page (application.cpp)
        void applySource(config::Source const& previous, config::Source const& next, bool navigate);
        void injectAfterLoad();
        void queueTemplateCall(std::string const& js);
        void setPage(PageStatus status, std::string const& reason);
        void updateEngineState();
        void scheduleReload(std::chrono::milliseconds delay);
        void supervisorLoop();
        void eventsLoop();
        void previewLoop();
        void readinessLoop();
        [[nodiscard]] std::string pageJson() const;
        [[nodiscard]] std::string renderJson() const;
        [[nodiscard]] static char const* statusName(PageStatus status);

        // HTTP (api.cpp)
        void handle(ops::HttpRequest const& req, ops::HttpResponse& res);
        std::optional<ops::WebSocketHandlers> upgrade(ops::HttpRequest const& req, ops::HttpResponse& reject);
        bool devtoolsTunnel(int fd, ops::HttpRequest const& req);
        [[nodiscard]] bool authorized(ops::HttpRequest const& req) const;
        [[nodiscard]] std::string statusJson() const;
        [[nodiscard]] std::string infoJson() const;
        [[nodiscard]] std::string nmosJson() const;
        [[nodiscard]] std::string metricsText();
        [[nodiscard]] std::string configJson() const;
        void broadcastEvent(std::string const& json);
        [[nodiscard]] bool ready(std::string* reason) const;

        config::Config _cfg;
        std::vector<config::SettingInfo> _table;
        std::unique_ptr<config::Document> _doc;
        int _argc;
        char** _argv;

        nmos::Ids _ids;
        std::string _domainDir;
        std::string _domainId;
        std::string _hostAddress;
        std::string _cachePath;
        cef::RenderMode _renderRequested = cef::RenderMode::Software;
        std::string _renderActual = "unknown"; // gpu / software from the page's WebGL renderer
        std::string _rendererName;
        std::atomic<bool> _renderDegraded{false};

        util::Metrics _metrics{"mxl_browser_source_"};
        std::unique_ptr<mxlio::Domain> _domain;
        std::unique_ptr<MxlClock> _clock;
        std::unique_ptr<MxlOutputs> _outputs;
        std::unique_ptr<engine::Engine> _engine;
        std::unique_ptr<cef::Browser> _browser;
        std::unique_ptr<ops::InteractHub> _interact;
        std::unique_ptr<ops::HttpServer> _http;
        std::unique_ptr<nmos::Node> _node;

        // Page state (guarded by _pageMutex).
        mutable std::mutex _pageMutex;
        PageStatus _page = PageStatus::Loading;
        std::string _pageReason;
        std::string _url;
        std::string _title;
        std::uint64_t _loads = 0;
        bool _slateShown = false;
        std::chrono::steady_clock::time_point _loadStarted{};
        std::deque<std::string> _templateQueue; // calls before OnLoadEnd
        std::deque<std::chrono::steady_clock::time_point> _crashes;
        std::optional<std::chrono::steady_clock::time_point> _reloadAt;
        bool _autoReloadBlocked = false;
        std::chrono::steady_clock::time_point _lastPeriodicReload{};
        std::deque<std::string> _console; // JSON lines, last 200
        bool _audioStream = false;

        // Liveness probe of the renderer (Runtime.evaluate through DevTools).
        std::mutex _probeMutex;
        int _probeId = 0;
        std::chrono::steady_clock::time_point _probeSent{};
        int _rendererProbeId = 0;

        // WebSockets.
        std::mutex _socketsMutex;
        std::set<std::shared_ptr<ops::WebSocket>> _eventSockets;
        std::map<int, std::shared_ptr<ops::WebSocket>> _interactSockets;
        std::atomic<int> _devtoolsSessions{0};

        // Preview JPEG cache for /api/v1/preview.jpg.
        std::mutex _jpegMutex;
        std::string _jpeg;
        std::uint64_t _jpegSequence = 0;
        std::uint64_t _jpegGrain = 0;

        std::atomic<bool> _browserReady{false};
        std::atomic<bool> _writersOpen{false};
        std::atomic<bool> _registered{false};
        std::atomic<bool> _stopping{false};
        std::mutex _closeMutex;
        std::condition_variable _closed;
        bool _browserClosed = false;
        std::vector<std::thread> _threads;
    };
}
