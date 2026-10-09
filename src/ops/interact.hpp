// SPDX-License-Identifier: MIT
// The interaction protocol (SPEC §7, docs/api.md): sessions on /api/v1/interact, who controls
// the page, the idle timeout, input limits, acks, and input turned into page events. No
// sockets and no CEF here: a session sends through functions, the page is an interface.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "ops/keymap.hpp"

namespace mbs::ops
{
    enum class MouseButton
    {
        Left = 0,
        Middle = 1,
        Right = 2,
    };

    /// The page, as the CEF layer drives it (view coordinates in DIP). Called on the caller's
    /// thread; the implementation posts to the UI thread.
    class PageInput
    {
    public:
        virtual ~PageInput() = default;
        virtual void mouseMove(int x, int y, std::uint32_t modifiers, bool leave) = 0;
        virtual void mouseClick(int x, int y, MouseButton button, bool up, int clicks, std::uint32_t modifiers) = 0;
        virtual void mouseWheel(int x, int y, int dx, int dy, std::uint32_t modifiers) = 0;
        virtual void key(KeyEvent const& event) = 0;
        virtual void imeComposition(std::u16string const& text, int selectionStart, int selectionEnd) = 0;
        virtual void imeCommit(std::u16string const& text) = 0;
        virtual void imeFinish() = 0;
        virtual void imeCancel() = 0;
        virtual void focus(bool focused) = 0;
    };

    struct InteractSettings
    {
        int viewWidth = 1920; // the view in DIP: raster / device scale factor
        int viewHeight = 1080;
        int timeoutS = 120;   // control ends after this long without input
        int previewFps = 10;  // default for watching sessions
        int maxPreviewFps = 25;
        int maxMessagesPerSecond = 200;
    };

    struct InteractCounters
    {
        std::map<std::string, std::uint64_t> events; // "<type>/<result>": ok, rejected, dropped
    };

    class InteractHub
    {
    public:
        using Clock = std::chrono::steady_clock;
        struct Peer
        {
            std::function<bool(std::string const&)> sendText;
            std::string address;
        };

        InteractHub(InteractSettings settings, PageInput& page);

        /// Builds the `page` and `render` parts of a state message (JSON objects as text).
        void setStateParts(std::function<std::string()> page, std::function<std::string()> render);
        /// The first grain index that can show input sent now (for acks).
        void setGrainSource(std::function<std::uint64_t()> nextGrain);
        void setViewSize(int width, int height);

        /// A session connected; returns its id and sends it the state.
        int open(Peer peer);
        void close(int session);
        /// One text message of a session.
        void message(int session, std::string const& text, Clock::time_point now = Clock::now());
        /// Ends control after the idle timeout; call about once a second.
        void tick(Clock::time_point now = Clock::now());
        /// Sends the state to every session (page loaded, title changed, …).
        void broadcastState();
        /// Sends a server message (`cursor`, `dialog`, `console`) to every session.
        void broadcast(std::string const& json);

        /// Preview rate each session asked for (0: none); the controller gets up to maxPreviewFps.
        [[nodiscard]] std::map<int, int> previewRates() const;
        [[nodiscard]] int sessions() const;
        [[nodiscard]] bool controlled() const;
        [[nodiscard]] InteractCounters counters() const;

    private:
        struct Session
        {
            Peer peer;
            int previewFps = 0;
            Clock::time_point windowStart{};
            int windowCount = 0;
        };

        using Actions = std::vector<std::function<void()>>;

        void count(std::string const& type, std::string const& result);
        [[nodiscard]] std::string stateJson(int id) const;
        /// Mouse-ups for the buttons the controller still holds (caller holds the lock).
        void releaseButtons(Actions& actions);
        /// Gives control to `id` (0: nobody); a change releases the held buttons.
        void setController(int id, Actions& actions);

        InteractSettings _settings;
        PageInput& _page;
        std::function<std::string()> _pagePart;
        std::function<std::string()> _renderPart;
        std::function<std::uint64_t()> _nextGrain;

        mutable std::mutex _mutex;
        std::map<int, Session> _sessions;
        int _nextId = 1;
        int _controller = 0; // session id, 0 = none
        std::uint32_t _buttons = 0; // kLeftMouseButton… the controller holds (down without up)
        int _lastX = 0;             // view position of the last pointer message
        int _lastY = 0;
        Clock::time_point _lastInput{};
        InteractCounters _counters;
    };
}
