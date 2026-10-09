// SPDX-License-Identifier: MIT
// What the page reports to the application (SPEC §4, §5, §6, §13). Called on CEF's threads:
// paints and page events on the UI thread, audio on CEF's audio thread. Implementations
// return quickly and never call back into CEF while holding their own locks.
#pragma once

#include <cstdint>
#include <string>

namespace mbs::cef
{
    class PageObserver
    {
    public:
        virtual ~PageObserver() = default;

        virtual void onBrowserCreated() = 0;
        virtual void onBrowserClosed() = 0;

        /// The view, `stride` bytes per row, premultiplied BGRA; valid only during the call.
        virtual void onPaint(void const* bgra, int stride, int width, int height) = 0;

        virtual void onAudioStarted(int channels, int sampleRate) = 0;
        virtual void onAudioPacket(float const* const* planes, int channels, int frames) = 0;
        virtual void onAudioStopped() = 0;

        virtual void onLoadStart(std::string const& url) = 0;
        virtual void onLoadEnd(std::string const& url, int httpStatus) = 0;
        virtual void onLoadError(std::string const& url, int code, std::string const& text) = 0;
        virtual void onTitle(std::string const& title) = 0;
        virtual void onAddress(std::string const& url) = 0;
        /// Whether the main frame can go back or forward in its history (OnLoadingStateChange).
        virtual void onHistory(bool canGoBack, bool canGoForward) = 0;
        /// `level`: debug, info, warning, error.
        virtual void onConsole(std::string const& level, std::string const& message, std::string const& source, int line) = 0;
        virtual void onCursor(std::string const& cursor) = 0;

        /// A JavaScript dialog answered without an operator: kind alert, confirm, prompt or
        /// beforeunload; result dismissed, accepted or cancelled.
        virtual void onDialog(std::string const& kind, std::string const& message, std::string const& result) = 0;
        virtual void onPopup(std::string const& url, bool opened) = 0;
        virtual void onDownloadBlocked(std::string const& url) = 0;
        virtual void onPermissionDenied(std::string const& type) = 0;
        virtual void onFileDialogCancelled() = 0;
        virtual void onNavigationBlocked(std::string const& url, std::string const& reason) = 0;

        virtual void onRendererTerminated(std::string const& reason) = 0;
        virtual void onUnresponsive() = 0;
        /// window.mxlBrowserSource.post(obj), as JSON text.
        virtual void onPagePost(std::string const& json) = 0;
        /// The answer to a DevTools method sent with Browser::devToolsMethod.
        virtual void onDevToolsResult(int id, bool success, std::string const& json) = 0;
    };
}
