// SPDX-License-Identifier: MIT
// CEF in this process (SPEC §2.3, §5.5): initialisation with the switches of the render mode,
// one windowless browser driven by external BeginFrames, and the facade the rest of the
// application uses. Every CefBrowserHost call is posted to the UI thread.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cef/observer.hpp"
#include "ops/interact.hpp"
#include "ops/urlpolicy.hpp"

namespace mbs::cef
{
    enum class RenderMode
    {
        Gpu,
        Software
    };

    struct RuntimeSettings
    {
        int argc = 0;
        char** argv = nullptr;
        RenderMode render = RenderMode::Software;
        std::string helperPath;           // mxl-browser-source-helper
        std::string resourcesDir;         // CEF resources and locales (next to the binary)
        std::string cachePath;            // root_cache_path
        std::string userAgentSuffix;      // "mxl-browser-source/<version>"
        std::string proxyServer;          // --proxy-server
        std::string proxyBypass;          // --proxy-bypass-list
        int devtoolsPort = 0;             // 0: no remote debugging
        std::string devtoolsOrigin;       // --remote-allow-origins
        std::vector<std::string> extraFlags; // BROWSER_CHROMIUM_FLAGS_APPEND, appended
        std::string templatesDir;         // served at https://templates.local/
    };

    struct BrowserSettings
    {
        std::string url;
        int width = 1920;   // raster
        int height = 1080;
        double deviceScaleFactor = 1.0;
        int frameRate = 50; // windowless_frame_rate, rounded up
        std::uint32_t background = 0; // 0xAARRGGBB, 0 = transparent
        bool popupsSameWindow = false;
        bool confirmAccept = false;
    };

    /// The process's role: returns >= 0 in a CEF subprocess (exit with it), -1 in the browser
    /// process. The main binary calls it first; the helper calls only this.
    int executeSubprocess(int argc, char** argv);

    /// The command line switches of a render mode (also logged at start).
    std::vector<std::string> switchesFor(RuntimeSettings const& settings);

    class Browser;

    /// CefInitialize; false when CEF cannot start (exit 75).
    bool initialize(RuntimeSettings const& settings);
    /// Runs the UI message loop until quit() (main thread).
    void runMessageLoop();
    /// Ends runMessageLoop() from any thread.
    void quit();
    /// CefShutdown, after the browser closed.
    void shutdown();
    /// Runs `task` on the UI thread.
    void postToUi(std::function<void()> task);
    /// True on CEF's UI thread.
    bool onUiThread();

    class Browser final : public ops::PageInput
    {
    public:
        Browser(BrowserSettings settings, PageObserver& observer, ops::UrlPolicy policy);
        ~Browser() override;
        Browser(Browser const&) = delete;
        Browser& operator=(Browser const&) = delete;

        /// Creates the browser (UI thread, asynchronous; onBrowserCreated follows).
        void create();
        /// Closes it (CloseBrowser(force)); onBrowserClosed follows.
        void close();
        [[nodiscard]] bool exists() const;

        void beginFrame();
        void invalidate();
        void navigate(std::string const& url);
        void reload(bool ignoreCache);
        void stopLoad();
        void executeJavaScript(std::string const& code);
        void setZoom(double factor);
        void setDeviceScaleFactor(double factor);
        /// ExecuteDevToolsMethod; the answer arrives at PageObserver::onDevToolsResult.
        int devToolsMethod(std::string const& method, std::string const& paramsJson);
        /// Asks a hung renderer to be terminated (the crash path takes over).
        void terminateRenderer();
        void setUrlPolicy(ops::UrlPolicy policy);

        // ops::PageInput
        void mouseMove(int x, int y, std::uint32_t modifiers, bool leave) override;
        void mouseClick(int x, int y, ops::MouseButton button, bool up, int clicks, std::uint32_t modifiers) override;
        void mouseWheel(int x, int y, int dx, int dy, std::uint32_t modifiers) override;
        void key(ops::KeyEvent const& event) override;
        void imeComposition(std::u16string const& text, int selectionStart, int selectionEnd) override;
        void imeCommit(std::u16string const& text) override;
        void imeFinish() override;
        void imeCancel() override;
        void focus(bool focused) override;

    private:
        struct Impl;
        std::shared_ptr<Impl> _impl;
    };
}
