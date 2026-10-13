// SPDX-License-Identifier: MIT
// The 8 OGraf layers (docs/ograf.md §3): what each holds, the actions, the state the function
// keeps (it is the source of truth, not the page), persistence and restore. The page is behind
// the Renderer interface, so the unit tests drive it with a fake. No CEF here.
#pragma once

#include <array>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "ograf/library.hpp"
#include "picojson/picojson.h"

namespace mbs::ograf
{
    inline constexpr int kLayers = 8;

    /// The answer of one call into the renderer page.
    struct CallResult
    {
        enum class Kind
        {
            Ok,         // the graphic's promise resolved; `value` is its ReturnPayload
            Threw,      // it threw or rejected; `error`
            Timeout,    // no answer within BROWSER_OGRAF_ACTION_TIMEOUT_MS
            Unavailable // no renderer page (mode page, loading, crashed)
        };
        Kind kind = Kind::Ok;
        picojson::value value;
        std::string error;
    };

    class Renderer
    {
    public:
        virtual ~Renderer() = default;
        /// window.ografRenderer.call(layer, method, args) in the page; blocks until it settles.
        /// Methods: load {module, data, renderCharacteristics}, playAction, stopAction,
        /// updateAction, customAction (the OGraf params), dispose {}.
        virtual CallResult call(int layer, std::string const& method, picojson::value const& args) = 0;
    };

    struct Layer
    {
        int number = 0;
        std::string state = "empty"; // empty, loading, ready, on, stopped, error
        std::string graphicId;
        std::string graphicName;
        std::string package;
        std::string instanceId;
        int stepCount = 1;
        std::optional<int> step;
        picojson::value data; // load() data with every updateAction merged in
        int pending = 0;      // calls still waiting for the graphic
        std::string error;
    };

    /// HTTP status and body of a Server API answer.
    struct Outcome
    {
        int status = 200;
        picojson::value body;
    };

    class LayerSet
    {
    public:
        LayerSet(Renderer& renderer, Library& library, std::string statePath, RenderInfo render);

        /// Called after every change (outside the lock), e.g. to push an event.
        void onChange(std::function<void()> callback);
        /// Called after each restored layer with its result (for a metric).
        void onRestore(std::function<void(bool ok)> callback);

        Outcome load(int layer, std::string const& graphicId, picojson::value const& data);
        /// `method`: playAction, stopAction, updateAction or customAction (with `actionId`).
        Outcome action(int layer, std::string const& instanceId, std::string const& method, picojson::value const& params, std::string const& actionId = {});
        /// Disposes the layer's graphic (also when the page is gone) and empties it.
        Outcome clear(int layer);
        /// Clears every layer that matches all given parts of the filter (empty = any).
        std::vector<Layer> clearWhere(std::optional<int> layer, std::string const& graphicId, std::string const& instanceId);

        [[nodiscard]] std::vector<Layer> snapshot() const;
        [[nodiscard]] std::set<std::string> packagesInUse() const;
        [[nodiscard]] bool anyLoaded() const;

        /// Reads the saved layers (graphic and data) at start; the page then restores them.
        void loadSaved();
        /// After the renderer page has loaded. `withSteps`: a reload after a crash (put each layer
        /// back where it was, without animation); false after a process start (loaded, not played).
        void restore(bool withSteps);
        /// The page went away (crash, hang, mode change): pending calls are over, the state stays.
        void pageLost();
        /// Empties every layer without calling the page (it is about to go away).
        void forgetAll();

    private:
        void changed();
        void saveLocked() const;
        Layer& at(int layer) { return _layers[static_cast<std::size_t>(layer - 1)]; }

        Renderer& _renderer;
        Library& _library;
        std::string _statePath;
        RenderInfo _render;
        mutable std::mutex _mu;
        std::array<Layer, kLayers> _layers;
        std::function<void()> _onChange;
        std::function<void(bool)> _onRestore;
    };

    /// Layer as JSON for /api/v1/ograf/layers and the events.
    picojson::value layerJson(Layer const& layer);
}
