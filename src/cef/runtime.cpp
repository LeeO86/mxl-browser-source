// SPDX-License-Identifier: MIT
#include "cef/runtime.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>

#include <unistd.h>

#include "include/base/cef_callback.h"
#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_devtools_message_observer.h"
#include "include/cef_parser.h"
#include "include/cef_scheme.h"
#include "include/cef_task.h"
#include "include/cef_version.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_message_router.h"
#include "include/wrapper/cef_stream_resource_handler.h"
#include "util/logging.hpp"

namespace mbs::cef
{
    namespace
    {
        namespace fs = std::filesystem;

        RuntimeSettings gSettings; // the browser process's switches (set before CefInitialize)

        // SIGKILL to the renderer processes of this browser process: command line with
        // --type=renderer and this process among the first ancestors (renderers are children of
        // the zygote). Returns how many were killed.
        int killOwnRenderers()
        {
            auto const parentOf = [](pid_t pid) -> pid_t {
                std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
                std::string line;
                std::getline(in, line);
                auto const close = line.rfind(')'); // the name may contain spaces
                if (close == std::string::npos)
                {
                    return 0;
                }
                std::istringstream rest(line.substr(close + 1));
                char state = 0;
                pid_t ppid = 0;
                rest >> state >> ppid;
                return ppid;
            };
            pid_t const self = ::getpid();
            int killed = 0;
            std::error_code ec;
            for (auto const& entry : fs::directory_iterator("/proc", ec))
            {
                auto const name = entry.path().filename().string();
                if (name.empty() || !std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
                {
                    continue;
                }
                std::ifstream in(entry.path() / "cmdline", std::ios::binary);
                std::string const cmdline((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                if (cmdline.find("--type=renderer") == std::string::npos)
                {
                    continue;
                }
                auto const pid = static_cast<pid_t>(std::stol(name));
                pid_t up = pid;
                for (int depth = 0; depth < 4 && up > 1 && up != self; ++depth)
                {
                    up = parentOf(up);
                }
                if (up == self && ::kill(pid, SIGKILL) == 0)
                {
                    ++killed;
                }
            }
            return killed;
        }

        CefMessageRouterConfig routerConfig()
        {
            CefMessageRouterConfig config;
            config.js_query_function = "mxlBrowserSourceQuery";
            config.js_cancel_function = "mxlBrowserSourceQueryCancel";
            return config;
        }

        // window.mxlBrowserSource.post(obj) → the message router → PageObserver::onPagePost.
        constexpr char const* kPageApi =
            "window.mxlBrowserSource=Object.freeze({post:function(o){window.mxlBrowserSourceQuery({request:JSON.stringify(o),"
            "persistent:false,onSuccess:function(){},onFailure:function(){}});}});";

        std::string cursorName(cef_cursor_type_t type)
        {
            switch (type)
            {
            case CT_CROSS: return "crosshair";
            case CT_HAND: return "pointer";
            case CT_IBEAM: return "text";
            case CT_WAIT: return "wait";
            case CT_HELP: return "help";
            case CT_EASTRESIZE: return "e-resize";
            case CT_NORTHRESIZE: return "n-resize";
            case CT_NORTHEASTRESIZE: return "ne-resize";
            case CT_NORTHWESTRESIZE: return "nw-resize";
            case CT_SOUTHRESIZE: return "s-resize";
            case CT_SOUTHEASTRESIZE: return "se-resize";
            case CT_SOUTHWESTRESIZE: return "sw-resize";
            case CT_WESTRESIZE: return "w-resize";
            case CT_NORTHSOUTHRESIZE: return "ns-resize";
            case CT_EASTWESTRESIZE: return "ew-resize";
            case CT_COLUMNRESIZE: return "col-resize";
            case CT_ROWRESIZE: return "row-resize";
            case CT_MOVE: return "move";
            case CT_VERTICALTEXT: return "vertical-text";
            case CT_CELL: return "cell";
            case CT_CONTEXTMENU: return "context-menu";
            case CT_ALIAS: return "alias";
            case CT_PROGRESS: return "progress";
            case CT_NODROP: return "no-drop";
            case CT_COPY: return "copy";
            case CT_NONE: return "none";
            case CT_NOTALLOWED: return "not-allowed";
            case CT_ZOOMIN: return "zoom-in";
            case CT_ZOOMOUT: return "zoom-out";
            case CT_GRAB: return "grab";
            case CT_GRABBING: return "grabbing";
            default: return "default";
            }
        }

        std::string terminationReason(cef_termination_status_t status)
        {
            switch (status)
            {
            case TS_ABNORMAL_TERMINATION: return "abnormal";
            case TS_PROCESS_WAS_KILLED: return "killed";
            case TS_PROCESS_CRASHED: return "crashed";
            case TS_PROCESS_OOM: return "oom";
            default: return "other";
            }
        }

        std::string severityName(cef_log_severity_t level)
        {
            switch (level)
            {
            case LOGSEVERITY_VERBOSE: return "debug";
            case LOGSEVERITY_WARNING: return "warning";
            case LOGSEVERITY_ERROR:
            case LOGSEVERITY_FATAL: return "error";
            default: return "info";
            }
        }

        // https://templates.local/<path>: the templates directory first, the image's built-in
        // pages second. No network, no file://.
        class TemplatesFactory : public CefSchemeHandlerFactory
        {
        public:
            TemplatesFactory(std::string dir, std::string fallback)
                : _dir(std::move(dir))
                , _fallback(std::move(fallback))
            {
            }

            CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefString const&, CefRefPtr<CefRequest> request) override
            {
                CefURLParts parts;
                std::string path = "/";
                if (CefParseURL(request->GetURL(), parts))
                {
                    path = CefURIDecode(CefString(&parts.path), true, static_cast<cef_uri_unescape_rule_t>(UU_SPACES | UU_URL_SPECIAL_CHARS_EXCEPT_PATH_SEPARATORS))
                               .ToString();
                }
                if (path.empty() || path.back() == '/')
                {
                    path += "index.html";
                }
                for (auto const& root : {_dir, _fallback})
                {
                    if (auto const file = resolve(root, path); !file.empty())
                    {
                        auto const extension = fs::path(file).extension().string();
                        std::string mime = CefGetMimeType(extension.empty() ? "" : extension.substr(1)).ToString();
                        if (mime.empty())
                        {
                            mime = "application/octet-stream";
                        }
                        CefResponse::HeaderMap headers;
                        headers.emplace("Cache-Control", "no-store");
                        headers.emplace("Access-Control-Allow-Origin", "*");
                        return new CefStreamResourceHandler(200, "OK", mime, headers, CefStreamReader::CreateForFile(file));
                    }
                }
                static constexpr char kNotFound[] = "not found";
                return new CefStreamResourceHandler(404, "Not Found", "text/plain", {},
                    CefStreamReader::CreateForData(const_cast<char*>(kNotFound), sizeof(kNotFound) - 1));
            }

            // A regular file under `root`; empty for anything outside it (.., symlinks out).
            static std::string resolve(std::string const& root, std::string const& path)
            {
                if (root.empty())
                {
                    return {};
                }
                std::error_code ec;
                auto const base = fs::weakly_canonical(root, ec);
                auto const file = fs::weakly_canonical(fs::path(root) / fs::path(path).relative_path(), ec);
                if (ec || !fs::is_regular_file(file, ec))
                {
                    return {};
                }
                auto const rel = file.lexically_relative(base);
                if (rel.empty() || *rel.begin() == "..")
                {
                    return {};
                }
                return file.string();
            }

        private:
            std::string _dir;
            std::string _fallback;
            IMPLEMENT_REFCOUNTING(TemplatesFactory);
        };

        // https://ograf.local/ (docs/ograf.md §3, §4): the built-in renderer pages under /renderer/
        // and the library's packages under /packages/<uuid>/<path>. No network, no file://.
        class OgrafFactory : public CefSchemeHandlerFactory
        {
        public:
            OgrafFactory(std::string packagesDir, std::map<std::string, std::string> pages)
                : _packagesDir(std::move(packagesDir))
                , _pages(std::move(pages))
            {
            }

            CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefString const&, CefRefPtr<CefRequest> request) override
            {
                CefURLParts parts;
                std::string path = "/";
                if (CefParseURL(request->GetURL(), parts))
                {
                    path = CefURIDecode(CefString(&parts.path), true, static_cast<cef_uri_unescape_rule_t>(UU_SPACES | UU_URL_SPECIAL_CHARS_EXCEPT_PATH_SEPARATORS))
                               .ToString();
                }
                CefResponse::HeaderMap headers;
                headers.emplace("Cache-Control", "no-store");
                headers.emplace("Access-Control-Allow-Origin", "*");
                if (path == "/renderer/" || path == "/renderer")
                {
                    path = "/renderer/index.html";
                }
                if (auto const page = _pages.find(path); page != _pages.end())
                {
                    return new CefStreamResourceHandler(200, "OK", "text/html", headers,
                        CefStreamReader::CreateForData(const_cast<char*>(page->second.data()), page->second.size()));
                }
                if (path.starts_with("/packages/"))
                {
                    if (auto const file = TemplatesFactory::resolve(_packagesDir, path.substr(10)); !file.empty())
                    {
                        auto const extension = fs::path(file).extension().string();
                        std::string mime;
                        if (extension == ".mjs" || extension == ".js")
                        {
                            mime = "text/javascript"; // module scripts need a JavaScript MIME type
                        }
                        else
                        {
                            mime = CefGetMimeType(extension.empty() ? "" : extension.substr(1)).ToString();
                        }
                        return new CefStreamResourceHandler(200, "OK", mime.empty() ? "application/octet-stream" : mime, headers, CefStreamReader::CreateForFile(file));
                    }
                }
                static constexpr char kNotFound[] = "not found";
                return new CefStreamResourceHandler(404, "Not Found", "text/plain", {},
                    CefStreamReader::CreateForData(const_cast<char*>(kNotFound), sizeof(kNotFound) - 1));
            }

        private:
            std::string _packagesDir;
            std::map<std::string, std::string> _pages; // kept alive: the stream readers point into them
            IMPLEMENT_REFCOUNTING(OgrafFactory);
        };

        class App : public CefApp, public CefBrowserProcessHandler, public CefRenderProcessHandler
        {
        public:
            CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
            CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }

            void OnBeforeCommandLineProcessing(CefString const& processType, CefRefPtr<CefCommandLine> commandLine) override
            {
                if (!processType.empty())
                {
                    return; // subprocesses get their switches from the browser process
                }
                for (auto const& sw : switchesFor(gSettings))
                {
                    auto const eq = sw.find('=');
                    if (eq == std::string::npos)
                    {
                        commandLine->AppendSwitch(sw);
                    }
                    else
                    {
                        commandLine->AppendSwitchWithValue(sw.substr(0, eq), sw.substr(eq + 1));
                    }
                }
            }

            void OnContextInitialized() override
            {
                CefRegisterSchemeHandlerFactory("https", "templates.local", new TemplatesFactory(gSettings.templatesDir, "/usr/local/share/mxl-browser-source/templates"));
                CefRegisterSchemeHandlerFactory("https", "ograf.local", new OgrafFactory(gSettings.ografPackagesDir, gSettings.ografPages));
            }

            // Renderer process: the page API and the router's renderer side.
            void OnWebKitInitialized() override { _router = CefMessageRouterRendererSide::Create(routerConfig()); }

            void OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context) override
            {
                if (_router)
                {
                    _router->OnContextCreated(browser, frame, context);
                }
                CefRefPtr<CefV8Value> result;
                CefRefPtr<CefV8Exception> exception;
                context->Eval(kPageApi, CefString(), 0, result, exception);
            }

            void OnContextReleased(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context) override
            {
                if (_router)
                {
                    _router->OnContextReleased(browser, frame, context);
                }
            }

            bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefProcessId source, CefRefPtr<CefProcessMessage> message) override
            {
                return _router && _router->OnProcessMessageReceived(browser, frame, source, message);
            }

        private:
            CefRefPtr<CefMessageRouterRendererSide> _router;
            IMPLEMENT_REFCOUNTING(App);
        };

        CefRefPtr<App>& app()
        {
            static CefRefPtr<App> instance(new App());
            return instance;
        }
    }

    std::vector<std::string> switchesFor(RuntimeSettings const& s)
    {
        // Spike S1/S3: headless Ozone, no X server. GPU: ANGLE on the NVIDIA EGL device
        // (Chromium 144 rejects --use-gl=egl). Software: no GPU process at all.
        std::vector<std::string> out{
            "ozone-platform=headless",
            "disable-features=BackgroundTracing",
            "autoplay-policy=no-user-gesture-required",
            "disable-background-timer-throttling",
            "disable-renderer-backgrounding",
            "disable-backgrounding-occluded-windows",
            "password-store=basic",
            "no-first-run",
            "disable-component-update",
            "disable-default-apps",
            "disable-sync",
        };
        if (s.render == RenderMode::Gpu)
        {
            out.insert(out.end(), {"use-gl=angle", "use-angle=gl-egl", "enable-gpu-rasterization", "ignore-gpu-blocklist"});
        }
        else
        {
            out.insert(out.end(), {"disable-gpu", "disable-gpu-compositing", "use-gl=disabled"});
        }
        // Chromium reads one value per switch (the last one), so feature lists are merged: the
        // function's entries first, then BROWSER_CHROMIUM_FLAGS_APPEND's.
        auto const merge = [&out](std::string const& name, std::string const& values) {
            auto const prefix = name + "=";
            for (auto& sw : out)
            {
                if (sw.rfind(prefix, 0) == 0)
                {
                    sw += "," + values;
                    return;
                }
            }
            out.push_back(prefix + values);
        };
        // Windowless CEF cannot show Chromium's WebAuthn dialog: without PublicKeyCredential,
        // sign-in pages offer other methods instead of waiting for a passkey (SPEC §4.5).
        if (!s.webauthn)
        {
            merge("disable-blink-features", "WebAuth");
        }
        // WebGPU (Dawn on Vulkan) is not used by broadcast graphics and has no Vulkan driver in the
        // image: a page testing navigator.gpu made Dawn probe for Vulkan, and a helper died with
        // "stack smashing detected" in the M4 soak. Off by default: navigator.gpu stays (Blink always
        // exposes it) but requestAdapter() answers null without Dawn. The GPU process still logs one
        // failed vkCreateInstance at start (Chromium decides WebGPU-on-Vulkan interop before features).
        if (!s.webgpu)
        {
            merge("disable-features", "WebGPUService,WebGPUBlobCache");
        }
        if (!s.proxyServer.empty())
        {
            out.push_back("proxy-server=" + s.proxyServer);
        }
        if (!s.proxyBypass.empty())
        {
            out.push_back("proxy-bypass-list=" + s.proxyBypass);
        }
        if (s.devtoolsPort > 0 && !s.devtoolsOrigin.empty())
        {
            out.push_back("remote-allow-origins=" + s.devtoolsOrigin);
        }
        // BROWSER_CHROMIUM_FLAGS_APPEND is a comma list, so "--disable-features=A,B" arrives as
        // "--disable-features=A" and "B": an entry without "--" right after a feature list continues it.
        std::string list;
        for (auto const& flag : s.extraFlags)
        {
            bool const dashed = flag.rfind("--", 0) == 0;
            auto const trimmed = dashed ? flag.substr(2) : flag;
            if (trimmed.empty())
            {
                continue;
            }
            if (!dashed && !list.empty() && trimmed.find('=') == std::string::npos)
            {
                merge(list, trimmed);
                continue;
            }
            auto const eq = trimmed.find('=');
            auto const name = trimmed.substr(0, eq);
            list.clear();
            if (eq != std::string::npos &&
                (name == "disable-features" || name == "enable-features" || name == "disable-blink-features" || name == "enable-blink-features"))
            {
                merge(name, trimmed.substr(eq + 1));
                list = name;
            }
            else
            {
                out.push_back(trimmed);
            }
        }
        return out;
    }

    int executeSubprocess(int argc, char** argv)
    {
        CefMainArgs args(argc, argv);
        return CefExecuteProcess(args, app(), nullptr);
    }

    bool initialize(RuntimeSettings const& settings)
    {
        gSettings = settings;
        CefMainArgs args(settings.argc, settings.argv);
        CefSettings cef;
        cef.windowless_rendering_enabled = true;
        cef.no_sandbox = true;
        cef.multi_threaded_message_loop = false;
        cef.log_severity = LOGSEVERITY_WARNING;
        // Chromium logs to stderr and to this file; /dev/stderr would print every line twice.
        CefString(&cef.log_file).FromString("/dev/null");
        CefString(&cef.browser_subprocess_path).FromString(settings.helperPath);
        if (!settings.resourcesDir.empty())
        {
            CefString(&cef.resources_dir_path).FromString(settings.resourcesDir);
            CefString(&cef.locales_dir_path).FromString(settings.resourcesDir + "/locales");
        }
        CefString(&cef.root_cache_path).FromString(settings.cachePath);
        CefString(&cef.cache_path).FromString(settings.cachePath);
        // Chromium's reduced user agent plus the suffix (SPEC §4.1): user_agent_product would
        // replace "Chrome/…" and break pages that look for it.
        std::string agent = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/" + std::to_string(CHROME_VERSION_MAJOR) +
                            ".0.0.0 Safari/537.36";
        if (!settings.userAgentSuffix.empty())
        {
            agent += " " + settings.userAgentSuffix;
        }
        CefString(&cef.user_agent).FromString(agent);
        if (settings.devtoolsPort > 0)
        {
            cef.remote_debugging_port = settings.devtoolsPort; // 127.0.0.1 only
        }
        cef.background_color = 0x00000000;
        return CefInitialize(args, cef, app(), nullptr);
    }

    void runMessageLoop()
    {
        CefRunMessageLoop();
    }

    void quit()
    {
        CefPostTask(TID_UI, base::BindOnce([] { CefQuitMessageLoop(); }));
    }

    void shutdown()
    {
        CefShutdown();
    }

    void postToUi(std::function<void()> task)
    {
        CefPostTask(TID_UI, base::BindOnce([](std::function<void()> t) { t(); }, std::move(task)));
    }

    bool onUiThread()
    {
        return CefCurrentlyOn(TID_UI);
    }

    // ------------------------------------------------------------------------------------
    // Browser

    class Client;

    struct Browser::Impl
    {
        BrowserSettings settings;
        PageObserver& observer;
        mutable std::mutex mutex; // guards policy and the settings that change at runtime
        ops::UrlPolicy policy;
        CefRefPtr<CefBrowser> browser; // UI thread only
        CefRefPtr<Client> client;
        CefRefPtr<CefMessageRouterBrowserSide> router;
        CefRefPtr<CefRegistration> devtoolsRegistration;
        CefRefPtr<CefUnresponsiveProcessCallback> unresponsive;
        std::atomic<bool> exists{false};
        std::atomic<int> nextDevToolsId{100000};

        Impl(BrowserSettings s, PageObserver& o, ops::UrlPolicy p)
            : settings(std::move(s))
            , observer(o)
            , policy(std::move(p))
        {
        }

        [[nodiscard]] ops::UrlVerdict check(std::string const& url, bool topLevel) const
        {
            std::lock_guard lock{mutex};
            return policy.check(url, topLevel);
        }

        CefRefPtr<CefBrowserHost> host() const { return browser ? browser->GetHost() : nullptr; }
    };

    namespace
    {
        class PostHandler : public CefMessageRouterBrowserSide::Handler
        {
        public:
            explicit PostHandler(PageObserver& observer)
                : _observer(observer)
            {
            }
            bool OnQuery(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int64_t, CefString const& request, bool, CefRefPtr<Callback> callback) override
            {
                if (request.length() > 64 * 1024)
                {
                    callback->Failure(413, "message too large");
                    return true;
                }
                _observer.onPagePost(request.ToString());
                callback->Success("");
                return true;
            }

        private:
            PageObserver& _observer;
        };

        class DevToolsObserver : public CefDevToolsMessageObserver
        {
        public:
            explicit DevToolsObserver(PageObserver& observer)
                : _observer(observer)
            {
            }
            void OnDevToolsMethodResult(CefRefPtr<CefBrowser>, int id, bool success, void const* result, size_t size) override
            {
                _observer.onDevToolsResult(id, success, std::string(static_cast<char const*>(result), size));
            }

        private:
            PageObserver& _observer;
            IMPLEMENT_REFCOUNTING(DevToolsObserver);
        };
    }

    class Client : public CefClient,
                   public CefRenderHandler,
                   public CefAudioHandler,
                   public CefLifeSpanHandler,
                   public CefDisplayHandler,
                   public CefLoadHandler,
                   public CefRequestHandler,
                   public CefResourceRequestHandler,
                   public CefJSDialogHandler,
                   public CefDownloadHandler,
                   public CefPermissionHandler,
                   public CefContextMenuHandler,
                   public CefDialogHandler
    {
    public:
        explicit Client(std::weak_ptr<Browser::Impl> impl)
            : _impl(std::move(impl))
        {
        }

        CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
        CefRefPtr<CefAudioHandler> GetAudioHandler() override { return this; }
        CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
        CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
        CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
        CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
        CefRefPtr<CefJSDialogHandler> GetJSDialogHandler() override { return this; }
        CefRefPtr<CefDownloadHandler> GetDownloadHandler() override { return this; }
        CefRefPtr<CefPermissionHandler> GetPermissionHandler() override { return this; }
        CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
        CefRefPtr<CefDialogHandler> GetDialogHandler() override { return this; }

        bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefProcessId source, CefRefPtr<CefProcessMessage> message) override
        {
            auto impl = _impl.lock();
            return impl && impl->router && impl->router->OnProcessMessageReceived(browser, frame, source, message);
        }

        // --- render
        void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override
        {
            auto impl = _impl.lock();
            if (!impl)
            {
                rect = CefRect(0, 0, 1920, 1080);
                return;
            }
            std::lock_guard lock{impl->mutex};
            double const dsf = std::max(0.25, impl->settings.deviceScaleFactor);
            rect = CefRect(0, 0, static_cast<int>(std::lround(impl->settings.width / dsf)), static_cast<int>(std::lround(impl->settings.height / dsf)));
        }

        bool GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo& info) override
        {
            auto impl = _impl.lock();
            if (!impl)
            {
                return false;
            }
            CefRect rect;
            GetViewRect(browser, rect);
            std::lock_guard lock{impl->mutex};
            info.device_scale_factor = static_cast<float>(impl->settings.deviceScaleFactor);
            info.rect = rect;
            info.available_rect = rect;
            info.depth = 24;
            info.depth_per_component = 8;
            return true;
        }

        void OnPaint(CefRefPtr<CefBrowser>, PaintElementType type, RectList const&, void const* buffer, int width, int height) override
        {
            if (type != PET_VIEW)
            {
                return;
            }
            if (auto impl = _impl.lock())
            {
                impl->observer.onPaint(buffer, width * 4, width, height);
            }
        }

        // --- audio
        bool GetAudioParameters(CefRefPtr<CefBrowser>, CefAudioParameters& params) override
        {
            params.sample_rate = 48000;
            params.frames_per_buffer = 480;
            // SPEC §6: the layout of BROWSER_AUDIO_CHANNELS. Chromium mixes the page into it (a
            // stereo page fills 1–2). 16 asks for 7.1: CEF has no discrete layout with a channel
            // count, so channels 9–16 stay silent.
            int channels = 2;
            if (auto impl = _impl.lock())
            {
                channels = impl->settings.audioChannels;
            }
            params.channel_layout = channels >= 8 ? CEF_CHANNEL_LAYOUT_7_1 : CEF_CHANNEL_LAYOUT_STEREO;
            return true;
        }
        void OnAudioStreamStarted(CefRefPtr<CefBrowser>, CefAudioParameters const& params, int channels) override
        {
            _channels = channels;
            if (auto impl = _impl.lock())
            {
                impl->observer.onAudioStarted(channels, params.sample_rate);
            }
        }
        void OnAudioStreamPacket(CefRefPtr<CefBrowser>, float const** data, int frames, int64_t pts) override
        {
            if (auto impl = _impl.lock())
            {
                impl->observer.onAudioPacket(data, _channels, frames, pts);
            }
        }
        void OnAudioStreamStopped(CefRefPtr<CefBrowser>) override
        {
            if (auto impl = _impl.lock())
            {
                impl->observer.onAudioStopped();
            }
        }
        void OnAudioStreamError(CefRefPtr<CefBrowser>, CefString const& message) override
        {
            // CEF stops the stream itself after any error but a socket error (OnAudioStreamStopped
            // follows); after a socket error packets keep coming, so the stream stays active.
            if (auto impl = _impl.lock())
            {
                impl->observer.onAudioError(message.ToString());
            }
        }

        // --- life span
        bool OnBeforePopup(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int, CefString const& targetUrl, CefString const&, cef_window_open_disposition_t, bool,
            CefPopupFeatures const&, CefWindowInfo&, CefRefPtr<CefClient>&, CefBrowserSettings&, CefRefPtr<CefDictionaryValue>&, bool*) override
        {
            auto impl = _impl.lock();
            if (!impl)
            {
                return true;
            }
            auto const url = targetUrl.ToString();
            bool sameWindow = false;
            {
                std::lock_guard lock{impl->mutex};
                sameWindow = impl->settings.popupsSameWindow;
            }
            bool const opened = sameWindow && impl->check(url, true).allowed;
            if (opened && frame)
            {
                frame->LoadURL(targetUrl);
            }
            impl->observer.onPopup(url, opened);
            return true; // never a new browser
        }

        void OnAfterCreated(CefRefPtr<CefBrowser> browser) override
        {
            auto impl = _impl.lock();
            if (!impl)
            {
                return;
            }
            impl->browser = browser;
            impl->devtoolsRegistration = browser->GetHost()->AddDevToolsMessageObserver(new DevToolsObserver(impl->observer));
            impl->exists.store(true);
            impl->observer.onBrowserCreated();
        }

        void OnBeforeClose(CefRefPtr<CefBrowser> browser) override
        {
            auto impl = _impl.lock();
            if (!impl)
            {
                return;
            }
            if (impl->router)
            {
                impl->router->OnBeforeClose(browser);
            }
            impl->devtoolsRegistration = nullptr;
            impl->browser = nullptr;
            impl->exists.store(false);
            impl->observer.onBrowserClosed();
        }

        // --- display
        void OnTitleChange(CefRefPtr<CefBrowser>, CefString const& title) override
        {
            if (auto impl = _impl.lock())
            {
                impl->observer.onTitle(title.ToString());
            }
        }
        void OnAddressChange(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, CefString const& url) override
        {
            auto impl = _impl.lock();
            if (impl && frame->IsMain())
            {
                impl->observer.onAddress(url.ToString());
            }
        }
        bool OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t level, CefString const& message, CefString const& source, int line) override
        {
            if (auto impl = _impl.lock())
            {
                impl->observer.onConsole(severityName(level), message.ToString(), source.ToString(), line);
            }
            return true; // not into Chromium's log
        }
        bool OnCursorChange(CefRefPtr<CefBrowser>, CefCursorHandle, cef_cursor_type_t type, CefCursorInfo const&) override
        {
            if (auto impl = _impl.lock())
            {
                impl->observer.onCursor(cursorName(type));
            }
            return true;
        }

        // --- load
        void OnLoadingStateChange(CefRefPtr<CefBrowser>, bool, bool canGoBack, bool canGoForward) override
        {
            if (auto impl = _impl.lock())
            {
                impl->observer.onHistory(canGoBack, canGoForward);
            }
        }
        void OnLoadStart(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, TransitionType) override
        {
            auto impl = _impl.lock();
            if (impl && frame->IsMain())
            {
                impl->observer.onLoadStart(frame->GetURL().ToString());
            }
        }
        void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int httpStatusCode) override
        {
            auto impl = _impl.lock();
            if (impl && frame->IsMain())
            {
                impl->observer.onLoadEnd(frame->GetURL().ToString(), httpStatusCode);
            }
        }
        void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, ErrorCode code, CefString const& text, CefString const& url) override
        {
            auto impl = _impl.lock();
            if (impl && frame->IsMain() && code != ERR_ABORTED)
            {
                impl->observer.onLoadError(url.ToString(), static_cast<int>(code), text.ToString());
            }
        }

        // --- request: the URL policy (SPEC §14.3) and process health
        bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request, bool, bool) override
        {
            auto impl = _impl.lock();
            if (!impl)
            {
                return true;
            }
            if (impl->router)
            {
                impl->router->OnBeforeBrowse(browser, frame);
            }
            auto const url = request->GetURL().ToString();
            auto const verdict = impl->check(url, true);
            if (!verdict.allowed)
            {
                impl->observer.onNavigationBlocked(url, verdict.reason);
                return true;
            }
            return false;
        }

        CefRefPtr<CefResourceRequestHandler> GetResourceRequestHandler(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest>, bool, bool, CefString const&,
            bool&) override
        {
            return this;
        }

        cef_return_value_t OnBeforeResourceLoad(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest> request, CefRefPtr<CefCallback>) override
        {
            auto impl = _impl.lock();
            if (!impl)
            {
                return RV_CANCEL;
            }
            bool const topLevel = request->GetResourceType() == RT_MAIN_FRAME;
            auto const url = request->GetURL().ToString();
            auto const verdict = impl->check(url, topLevel);
            if (!verdict.allowed)
            {
                impl->observer.onNavigationBlocked(url, verdict.reason);
                return RV_CANCEL;
            }
            return RV_CONTINUE;
        }

        void OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser, TerminationStatus status, int, CefString const&) override
        {
            auto impl = _impl.lock();
            if (!impl)
            {
                return;
            }
            if (impl->router)
            {
                impl->router->OnRenderProcessTerminated(browser);
            }
            impl->unresponsive = nullptr;
            impl->observer.onRendererTerminated(terminationReason(status));
        }

        bool OnRenderProcessUnresponsive(CefRefPtr<CefBrowser>, CefRefPtr<CefUnresponsiveProcessCallback> callback) override
        {
            auto impl = _impl.lock();
            if (!impl)
            {
                return false;
            }
            impl->unresponsive = callback; // terminateRenderer() uses it
            impl->observer.onUnresponsive();
            return true;
        }

        void OnRenderProcessResponsive(CefRefPtr<CefBrowser>) override
        {
            if (auto impl = _impl.lock())
            {
                impl->unresponsive = nullptr;
            }
        }

        // --- dialogs (SPEC §4.5): nothing ever waits for an operator
        bool OnJSDialog(CefRefPtr<CefBrowser>, CefString const&, JSDialogType type, CefString const& message, CefString const&, CefRefPtr<CefJSDialogCallback> callback,
            bool& suppress) override
        {
            auto impl = _impl.lock();
            if (!impl)
            {
                suppress = true;
                return false;
            }
            auto const text = message.ToString();
            if (type == JSDIALOGTYPE_ALERT)
            {
                impl->observer.onDialog("alert", text, "dismissed");
                suppress = true;
                return false;
            }
            if (type == JSDIALOGTYPE_CONFIRM)
            {
                bool accept = false;
                {
                    std::lock_guard lock{impl->mutex};
                    accept = impl->settings.confirmAccept;
                }
                impl->observer.onDialog("confirm", text, accept ? "accepted" : "cancelled");
                callback->Continue(accept, CefString());
                return true;
            }
            impl->observer.onDialog("prompt", text, "cancelled");
            callback->Continue(false, CefString());
            return true;
        }

        bool OnBeforeUnloadDialog(CefRefPtr<CefBrowser>, CefString const& message, bool, CefRefPtr<CefJSDialogCallback> callback) override
        {
            if (auto impl = _impl.lock())
            {
                impl->observer.onDialog("beforeunload", message.ToString(), "accepted");
            }
            callback->Continue(true, CefString());
            return true;
        }

        // --- downloads, permissions, context menu, file chooser
        bool CanDownload(CefRefPtr<CefBrowser>, CefString const& url, CefString const&) override
        {
            if (auto impl = _impl.lock())
            {
                impl->observer.onDownloadBlocked(url.ToString());
            }
            return false;
        }

        bool OnBeforeDownload(CefRefPtr<CefBrowser>, CefRefPtr<CefDownloadItem>, CefString const&, CefRefPtr<CefBeforeDownloadCallback>) override { return false; }

        bool OnRequestMediaAccessPermission(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefString const&, uint32_t requested,
            CefRefPtr<CefMediaAccessCallback> callback) override
        {
            if (auto impl = _impl.lock())
            {
                if ((requested & CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE) != 0)
                {
                    impl->observer.onPermissionDenied("camera");
                }
                if ((requested & CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE) != 0)
                {
                    impl->observer.onPermissionDenied("microphone");
                }
                if ((requested & (CEF_MEDIA_PERMISSION_DESKTOP_AUDIO_CAPTURE | CEF_MEDIA_PERMISSION_DESKTOP_VIDEO_CAPTURE)) != 0)
                {
                    impl->observer.onPermissionDenied("screen");
                }
            }
            callback->Cancel();
            return true;
        }

        bool OnShowPermissionPrompt(CefRefPtr<CefBrowser>, uint64_t, CefString const&, uint32_t requested, CefRefPtr<CefPermissionPromptCallback> callback) override
        {
            if (auto impl = _impl.lock())
            {
                std::string type = "other";
                if ((requested & CEF_PERMISSION_TYPE_GEOLOCATION) != 0)
                {
                    type = "geolocation";
                }
                else if ((requested & CEF_PERMISSION_TYPE_NOTIFICATIONS) != 0)
                {
                    type = "notifications";
                }
                else if ((requested & CEF_PERMISSION_TYPE_CLIPBOARD) != 0)
                {
                    type = "clipboard";
                }
                else if ((requested & CEF_PERMISSION_TYPE_MIDI_SYSEX) != 0)
                {
                    type = "midi";
                }
                else if ((requested & (CEF_PERMISSION_TYPE_CAMERA_STREAM | CEF_PERMISSION_TYPE_CAMERA_PAN_TILT_ZOOM)) != 0)
                {
                    type = "camera";
                }
                else if ((requested & CEF_PERMISSION_TYPE_MIC_STREAM) != 0)
                {
                    type = "microphone";
                }
                impl->observer.onPermissionDenied(type);
            }
            callback->Continue(CEF_PERMISSION_RESULT_DENY);
            return true;
        }

        void OnBeforeContextMenu(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefContextMenuParams>, CefRefPtr<CefMenuModel> model) override { model->Clear(); }

        bool OnFileDialog(CefRefPtr<CefBrowser>, FileDialogMode, CefString const&, CefString const&, std::vector<CefString> const&, std::vector<CefString> const&,
            std::vector<CefString> const&, CefRefPtr<CefFileDialogCallback> callback) override
        {
            if (auto impl = _impl.lock())
            {
                impl->observer.onFileDialogCancelled();
            }
            callback->Cancel();
            return true;
        }

    private:
        std::weak_ptr<Browser::Impl> _impl;
        std::atomic<int> _channels{2}; // set when a stream starts (CEF_CHANNEL_LAYOUT_STEREO)
        IMPLEMENT_REFCOUNTING(Client);
    };

    Browser::Browser(BrowserSettings settings, PageObserver& observer, ops::UrlPolicy policy)
        : _impl(std::make_shared<Impl>(std::move(settings), observer, std::move(policy)))
    {
    }

    Browser::~Browser() = default;

    void Browser::create()
    {
        auto impl = _impl;
        postToUi([impl] {
            impl->client = new Client(impl);
            impl->router = CefMessageRouterBrowserSide::Create(routerConfig());
            impl->router->AddHandler(new PostHandler(impl->observer), false);
            CefWindowInfo info;
            info.SetAsWindowless(kNullWindowHandle);
            CefBrowserSettings settings;
            std::string url;
            {
                std::lock_guard lock{impl->mutex};
                info.external_begin_frame_enabled = impl->settings.externalBeginFrames;
                settings.windowless_frame_rate = std::clamp(impl->settings.frameRate, 1, 60);
                settings.background_color = impl->settings.background;
                url = impl->settings.url;
            }
            CefBrowserHost::CreateBrowser(info, impl->client, url, settings, nullptr, nullptr);
        });
    }

    void Browser::setStartUrl(std::string const& url)
    {
        std::lock_guard lock{_impl->mutex};
        _impl->settings.url = url;
    }

    void Browser::close()
    {
        auto impl = _impl;
        postToUi([impl] {
            if (auto host = impl->host())
            {
                host->CloseBrowser(true);
            }
        });
    }

    bool Browser::exists() const
    {
        return _impl->exists.load();
    }

    void Browser::beginFrame(std::function<void()> sent)
    {
        auto impl = _impl;
        postToUi([impl, sent = std::move(sent)] {
            if (auto host = impl->host())
            {
                if (sent)
                {
                    sent();
                }
                host->SendExternalBeginFrame();
            }
        });
    }

    void Browser::anchorFrameClock(std::uint64_t targetNs, std::function<std::uint64_t()> now, std::function<void(std::uint64_t)> done)
    {
        // The UI task wakes a few milliseconds early (CefPostDelayedTask counts whole
        // milliseconds, and the UI thread may be copying a paint) and spins to the time. A task
        // that comes later than kLateNs does nothing: the frame clock tries again at the next grain.
        constexpr std::int64_t kEarlyMs = 3;
        constexpr std::uint64_t kLateNs = 300'000;
        constexpr std::uint64_t kMaxSpinNs = 8'000'000;
        auto const ahead = static_cast<std::int64_t>(targetNs) - static_cast<std::int64_t>(now());
        auto const delayMs = std::max<std::int64_t>(0, ahead / 1'000'000 - kEarlyMs);
        auto impl = _impl;
        auto task = [impl, targetNs, now = std::move(now), done = std::move(done)] {
            auto host = impl->host();
            auto t = now();
            if (!host || t > targetNs + kLateNs || targetNs > t + kMaxSpinNs)
            {
                done(0);
                return;
            }
            while ((t = now()) < targetNs)
            {
                __builtin_ia32_pause();
            }
            int rate = 0;
            {
                std::lock_guard lock{impl->mutex};
                rate = std::clamp(impl->settings.frameRate, 1, 60);
            }
            host->SetWindowlessFrameRate(rate); // CefRenderWidgetHostViewOSR::SetFrameRate: timebase = Now()
            done(t);
        };
        CefPostDelayedTask(TID_UI, base::BindOnce([](std::function<void()> t) { t(); }, std::function<void()>(std::move(task))), delayMs);
    }

    void Browser::invalidate()
    {
        auto impl = _impl;
        postToUi([impl] {
            if (auto host = impl->host())
            {
                host->Invalidate(PET_VIEW);
            }
        });
    }

    void Browser::navigate(std::string const& url)
    {
        auto impl = _impl;
        postToUi([impl, url] {
            if (impl->browser)
            {
                impl->browser->GetMainFrame()->LoadURL(url);
            }
        });
    }

    void Browser::reload(bool ignoreCache)
    {
        auto impl = _impl;
        postToUi([impl, ignoreCache] {
            if (!impl->browser)
            {
                return;
            }
            if (ignoreCache)
            {
                impl->browser->ReloadIgnoreCache();
            }
            else
            {
                impl->browser->Reload();
            }
        });
    }

    void Browser::goBack()
    {
        auto impl = _impl;
        postToUi([impl] {
            if (impl->browser)
            {
                impl->browser->GoBack();
            }
        });
    }

    void Browser::goForward()
    {
        auto impl = _impl;
        postToUi([impl] {
            if (impl->browser)
            {
                impl->browser->GoForward();
            }
        });
    }

    void Browser::stopLoad()
    {
        auto impl = _impl;
        postToUi([impl] {
            if (impl->browser)
            {
                impl->browser->StopLoad();
            }
        });
    }

    void Browser::executeJavaScript(std::string const& code)
    {
        auto impl = _impl;
        postToUi([impl, code] {
            if (impl->browser)
            {
                auto frame = impl->browser->GetMainFrame();
                frame->ExecuteJavaScript(code, frame->GetURL(), 0);
            }
        });
    }

    void Browser::setZoom(double factor)
    {
        auto impl = _impl;
        postToUi([impl, factor] {
            if (auto host = impl->host())
            {
                host->SetZoomLevel(std::log(std::max(0.05, factor)) / std::log(1.2));
            }
        });
    }

    void Browser::setDeviceScaleFactor(double factor)
    {
        {
            std::lock_guard lock{_impl->mutex};
            _impl->settings.deviceScaleFactor = factor;
        }
        auto impl = _impl;
        postToUi([impl] {
            if (auto host = impl->host())
            {
                host->NotifyScreenInfoChanged();
                host->WasResized();
                host->Invalidate(PET_VIEW);
            }
        });
    }

    void Browser::setUrlPolicy(ops::UrlPolicy policy)
    {
        std::lock_guard lock{_impl->mutex};
        _impl->policy = std::move(policy);
    }

    int Browser::devToolsMethod(std::string const& method, std::string const& paramsJson)
    {
        int const id = _impl->nextDevToolsId.fetch_add(1);
        auto impl = _impl;
        postToUi([impl, id, method, paramsJson] {
            auto host = impl->host();
            if (!host)
            {
                impl->observer.onDevToolsResult(id, false, R"({"error":"no browser"})");
                return;
            }
            CefRefPtr<CefDictionaryValue> params;
            if (!paramsJson.empty())
            {
                if (auto value = CefParseJSON(paramsJson, JSON_PARSER_RFC); value && value->GetType() == VTYPE_DICTIONARY)
                {
                    params = value->GetDictionary();
                }
            }
            if (host->ExecuteDevToolsMethod(id, method, params) == 0)
            {
                impl->observer.onDevToolsResult(id, false, R"({"error":"not sent"})");
            }
        });
        return id;
    }

    void Browser::terminateRenderer()
    {
        auto impl = _impl;
        postToUi([impl] {
            if (impl->unresponsive)
            {
                impl->unresponsive->Terminate();
                impl->unresponsive = nullptr;
            }
            else if (killOwnRenderers() > 0)
            {
                // Not reported unresponsive (a hang without input): OnRenderProcessTerminated
                // follows, as for Terminate().
            }
            else if (auto host = impl->host())
            {
                // No renderer process found: close and let the app create the browser again.
                host->CloseBrowser(true);
            }
        });
    }

    void Browser::mouseMove(int x, int y, std::uint32_t modifiers, bool leave)
    {
        auto impl = _impl;
        postToUi([impl, x, y, modifiers, leave] {
            if (auto host = impl->host())
            {
                CefMouseEvent event;
                event.x = x;
                event.y = y;
                event.modifiers = modifiers;
                host->SendMouseMoveEvent(event, leave);
            }
        });
    }

    void Browser::mouseClick(int x, int y, ops::MouseButton button, bool up, int clicks, std::uint32_t modifiers)
    {
        auto impl = _impl;
        postToUi([impl, x, y, button, up, clicks, modifiers] {
            if (auto host = impl->host())
            {
                CefMouseEvent event;
                event.x = x;
                event.y = y;
                event.modifiers = modifiers;
                host->SendMouseClickEvent(event, static_cast<CefBrowserHost::MouseButtonType>(button), up, clicks);
            }
        });
    }

    void Browser::mouseWheel(int x, int y, int dx, int dy, std::uint32_t modifiers)
    {
        auto impl = _impl;
        postToUi([impl, x, y, dx, dy, modifiers] {
            if (auto host = impl->host())
            {
                CefMouseEvent event;
                event.x = x;
                event.y = y;
                event.modifiers = modifiers;
                // The API uses the DOM sign (positive dy scrolls down); CEF the opposite.
                host->SendMouseWheelEvent(event, -dx, -dy);
            }
        });
    }

    void Browser::key(ops::KeyEvent const& e)
    {
        auto impl = _impl;
        postToUi([impl, e] {
            if (auto host = impl->host())
            {
                CefKeyEvent event;
                event.type = static_cast<cef_key_event_type_t>(e.type);
                event.windows_key_code = e.windowsKeyCode;
                event.native_key_code = 0;
                event.character = e.character;
                event.unmodified_character = e.unmodifiedCharacter;
                event.modifiers = e.modifiers;
                host->SendKeyEvent(event);
            }
        });
    }

    void Browser::imeComposition(std::u16string const& text, int selectionStart, int selectionEnd)
    {
        auto impl = _impl;
        postToUi([impl, text, selectionStart, selectionEnd] {
            if (auto host = impl->host())
            {
                CefCompositionUnderline underline;
                underline.range = CefRange(0, static_cast<uint32_t>(text.size()));
                underline.color = 0xFF000000;
                underline.background_color = 0;
                underline.thick = false;
                underline.style = CEF_CUS_SOLID;
                host->ImeSetComposition(CefString(text), {underline}, CefRange(UINT32_MAX, UINT32_MAX),
                    CefRange(static_cast<uint32_t>(std::max(0, selectionStart)), static_cast<uint32_t>(std::max(0, selectionEnd))));
            }
        });
    }

    void Browser::imeCommit(std::u16string const& text)
    {
        auto impl = _impl;
        postToUi([impl, text] {
            if (auto host = impl->host())
            {
                host->ImeCommitText(CefString(text), CefRange(UINT32_MAX, UINT32_MAX), 0);
            }
        });
    }

    void Browser::imeFinish()
    {
        auto impl = _impl;
        postToUi([impl] {
            if (auto host = impl->host())
            {
                host->ImeFinishComposingText(false);
            }
        });
    }

    void Browser::imeCancel()
    {
        auto impl = _impl;
        postToUi([impl] {
            if (auto host = impl->host())
            {
                host->ImeCancelComposition();
            }
        });
    }

    void Browser::focus(bool focused)
    {
        auto impl = _impl;
        postToUi([impl, focused] {
            if (auto host = impl->host())
            {
                host->SetFocus(focused);
            }
        });
    }
}
