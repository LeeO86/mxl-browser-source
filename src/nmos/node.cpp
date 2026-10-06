// SPDX-License-Identifier: MIT
// Modelled on mxl-replay and mxl-decklink (src/nmos/node.cpp, MIT, same author).
#include "nmos/node.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "cpprest/host_utils.h"
#include "nmos/channels.h"
#include "nmos/clock_name.h"
#include "nmos/colorspace.h"
#include "nmos/connection_resources.h"
#include "nmos/format.h"
#include "nmos/group_hint.h"
#include "nmos/interlace_mode.h"
#include "nmos/log_gate.h"
#include "nmos/media_type.h"
#include "nmos/model.h"
#include "nmos/mxl.h"
#include "nmos/node_interfaces.h"
#include "nmos/node_resource.h"
#include "nmos/node_resources.h"
#include "nmos/node_server.h"
#include "nmos/query_utils.h"
#include "nmos/rational.h"
#include "nmos/server.h"
#include "nmos/settings.h"
#include "nmos/transfer_characteristic.h"
#include "nmos/transport.h"
#include "sdp/json.h"
#include "util/logging.hpp"
#include "util/net.hpp"

namespace mbs::nmos
{
    namespace
    {
        utility::string_t us(std::string const& text)
        {
            return utility::conversions::to_string_t(text);
        }

        std::string su(utility::string_t const& text)
        {
            return utility::conversions::to_utf8string(text);
        }

        // "<label> <role>", group hint <label>:<role>. The platform connects by sender label.
        void labelAndGroup(::nmos::resource& resource, std::string const& label, std::string const& role, std::string const& description)
        {
            resource.data[U("label")] = web::json::value::string(us(label + " " + role));
            resource.data[U("description")] = web::json::value::string(us(description));
            if (!resource.data.has_field(::nmos::fields::tags))
            {
                resource.data[U("tags")] = web::json::value::object();
            }
            web::json::push_back(resource.data[U("tags")][U("urn:x-nmos:tag:grouphint/v1.0")], ::nmos::make_group_hint({us(label), us(role)}));
        }

        void addTags(::nmos::resource& resource, std::map<std::string, std::vector<std::string>> const& tags)
        {
            if (!resource.data.has_field(::nmos::fields::tags))
            {
                resource.data[U("tags")] = web::json::value::object();
            }
            for (auto const& [key, values] : tags)
            {
                auto& list = resource.data[U("tags")][us(key)];
                if (!list.is_array())
                {
                    list = web::json::value::array();
                }
                for (auto const& value : values)
                {
                    web::json::push_back(list, web::json::value::string(us(value)));
                }
            }
        }

        std::vector<::nmos::channel> channels(int count)
        {
            if (count == 2)
            {
                return {{U("Left"), ::nmos::channel_symbols::L}, {U("Right"), ::nmos::channel_symbols::R}};
            }
            std::vector<::nmos::channel> out;
            for (int i = 1; i <= count; ++i)
            {
                out.push_back({us("Ch" + std::to_string(i)), ::nmos::channel_symbols::Undefined(static_cast<unsigned>(i))});
            }
            return out;
        }
    }

    struct Node::Impl
    {
        NodeSettings settings;
        std::function<void(std::string const&, bool)> onEnable;
        std::thread thread;
        std::atomic<bool> running{false};
        std::atomic<bool> stopRequested{false};
        std::atomic<bool> deregistered{false};
        std::atomic<bool> finished{false};
        std::atomic<bool> registered{false};
        std::mutex startMutex;
        std::condition_variable startCv;
        bool startDone = false;
        std::string error;

        // Which sender a connection id belongs to.
        [[nodiscard]] std::string senderName(std::string const& id) const
        {
            if (id == settings.ids.videoSender)
            {
                return "video";
            }
            if (id == settings.ids.keySender)
            {
                return "key";
            }
            if (id == settings.ids.audioSender)
            {
                return "audio";
            }
            return {};
        }

        [[nodiscard]] bool initiallyEnabled(std::string const& name) const
        {
            auto const it = settings.enabled.find(name);
            return it == settings.enabled.end() || it->second;
        }

        void addSender(::nmos::node_model& model, std::string const& name, std::string const& role, std::string const& flowId,
            std::string const& senderId, ::nmos::resource source, ::nmos::resource flow)
        {
            auto const& s = settings;
            labelAndGroup(source, s.senderLabel, role, "mxl-browser-source " + name + " source");
            labelAndGroup(flow, s.senderLabel, role, "mxl-browser-source " + name + " flow");
            ::nmos::insert_resource(model.node_resources, std::move(source));
            ::nmos::insert_resource(model.node_resources, std::move(flow));
            bool const enabled = initiallyEnabled(name);
            auto sender = ::nmos::make_sender(us(senderId), us(flowId), ::nmos::transports::mxl, us(s.ids.device), {}, {}, model.settings);
            sender.data[U("subscription")][U("active")] = web::json::value::boolean(enabled);
            labelAndGroup(sender, s.senderLabel, role, "MXL sender");
            ::nmos::insert_resource(model.node_resources, std::move(sender));
            auto connection = ::nmos::make_connection_mxl_sender(us(senderId), us(s.domainId), us(flowId));
            // make_connection_mxl_sender leaves "auto" in /active too; a Sender reports its real domain and
            // flow from the start (BCP-007-03, AMWA IS-05-01 test_11_01). /staged keeps "auto".
            web::json::value leg = web::json::value::object();
            leg[U("mxl_domain_id")] = web::json::value::string(us(s.domainId));
            leg[U("mxl_flow_id")] = web::json::value::string(us(flowId));
            connection.data[U("active")][U("transport_params")] = web::json::value::array({leg});
            connection.data[U("active")][U("master_enable")] = web::json::value::boolean(enabled);
            connection.data[U("staged")][U("master_enable")] = web::json::value::boolean(enabled);
            ::nmos::insert_resource(model.connection_resources, std::move(connection));
        }

        void build(::nmos::node_model& model)
        {
            auto const& s = settings;
            auto const clocks = web::json::value_of({::nmos::make_internal_clock(::nmos::clock_names::clk0)});
            auto const interfaces = ::nmos::experimental::node_interfaces(::nmos::get_host_interfaces(model.settings));
            auto node = ::nmos::make_node(us(s.ids.node), clocks, ::nmos::make_node_interfaces(interfaces), model.settings);
            node.data[U("label")] = web::json::value::string(us(s.label));
            node.data[U("description")] = web::json::value::string(U("mxl-browser-source"));
            addTags(node, s.tags);
            ::nmos::insert_resource(model.node_resources, std::move(node));

            std::vector<::nmos::id> senders{us(s.ids.videoSender)};
            if (s.keyFlow)
            {
                senders.push_back(us(s.ids.keySender));
            }
            if (s.audioChannels > 0)
            {
                senders.push_back(us(s.ids.audioSender));
            }
            // make_device adds the urn:x-nmos:control:sr-ctrl controls for the Connection API.
            auto device = ::nmos::make_device(us(s.ids.device), us(s.ids.node), senders, {}, model.settings);
            device.data[U("label")] = web::json::value::string(us(s.label));
            device.data[U("description")] = web::json::value::string(U("mxl-browser-source"));
            addTags(device, s.tags);
            ::nmos::insert_resource(model.node_resources, std::move(device));

            ::nmos::rational const rate{s.rateNum, s.rateDen};
            auto videoFlow = [&](std::string const& flowId, std::string const& sourceId, bool alpha) {
                return ::nmos::make_coded_video_flow(us(flowId), us(sourceId), us(s.ids.device), rate, s.width, s.height, ::nmos::interlace_modes::progressive,
                    ::nmos::colorspaces::BT709, ::nmos::transfer_characteristics::SDR, sdp::samplings::YCbCr_4_2_2, 10,
                    alpha ? ::nmos::media_types::video_v210a : ::nmos::media_types::video_v210, model.settings);
            };
            addSender(model, "video", "Video", s.ids.videoFlow, s.ids.videoSender,
                ::nmos::make_video_source(us(s.ids.videoSource), us(s.ids.device), ::nmos::clock_names::clk0, rate, model.settings),
                videoFlow(s.ids.videoFlow, s.ids.videoSource, s.v210a));
            if (s.keyFlow)
            {
                addSender(model, "key", "Key", s.ids.keyFlow, s.ids.keySender,
                    ::nmos::make_video_source(us(s.ids.keySource), us(s.ids.device), ::nmos::clock_names::clk0, rate, model.settings),
                    videoFlow(s.ids.keyFlow, s.ids.keySource, false));
            }
            if (s.audioChannels > 0)
            {
                auto audioFlow = ::nmos::make_raw_audio_flow(us(s.ids.audioFlow), us(s.ids.audioSource), us(s.ids.device), ::nmos::rational{48000, 1},
                    ::nmos::media_types::audio_float32, 32, model.settings);
                audioFlow.data[U("channel_count")] = s.audioChannels;
                addSender(model, "audio", "Audio", s.ids.audioFlow, s.ids.audioSender,
                    ::nmos::make_audio_source(us(s.ids.audioSource), us(s.ids.device), ::nmos::clock_names::clk0, ::nmos::rational{48000, 1},
                        channels(s.audioChannels), model.settings),
                    std::move(audioFlow));
            }
        }

        // Nulls every resource, senders first and the node last, so nmos-cpp sends the DELETEs.
        static void deregister(::nmos::node_model& model)
        {
            auto rankOf = [](::nmos::type const& type) {
                if (type == ::nmos::types::sender || type == ::nmos::types::receiver)
                {
                    return 0;
                }
                if (type == ::nmos::types::flow)
                {
                    return 1;
                }
                if (type == ::nmos::types::source)
                {
                    return 2;
                }
                if (type == ::nmos::types::device)
                {
                    return 3;
                }
                return type == ::nmos::types::node ? 4 : -1;
            };
            std::vector<std::pair<int, ::nmos::id>> ranked;
            for (auto const& resource : model.node_resources)
            {
                if (int const rank = rankOf(resource.type); rank >= 0 && resource.has_data())
                {
                    ranked.emplace_back(rank, resource.id);
                }
            }
            std::stable_sort(ranked.begin(), ranked.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
            for (auto const& [rank, id] : ranked)
            {
                auto found = model.node_resources.find(id);
                if (found == model.node_resources.end() || !found->has_data())
                {
                    continue;
                }
                auto const pre = found->data;
                auto const updated = ::nmos::strictly_increasing_update(model.node_resources);
                model.node_resources.modify(found, [&](::nmos::resource& resource) {
                    resource.data = web::json::value::null();
                    resource.updated = updated;
                });
                ::nmos::insert_resource_events(model.node_resources, found->version, found->downgrade_version, found->type, pre, found->data);
            }
        }

        void run()
        {
            auto const& s = settings;
            try
            {
                ::nmos::experimental::log_model logModel;
                std::ostream errorLog(std::cerr.rdbuf());
                std::filebuf discarded;
                std::ostream accessLog(&discarded);
                ::nmos::experimental::log_gate gate(errorLog, accessLog, logModel);
                ::nmos::node_model model;
                web::json::value config = web::json::value::object();
                config[U("http_port")] = s.port;
                config[U("label")] = web::json::value::string(us(s.label));
                config[U("description")] = web::json::value::string(U("mxl-browser-source"));
                config[U("seed_id")] = web::json::value::string(us(s.ids.node));
                config[U("logging_level")] = 20;
                config[U("control_protocol_ws_port")] = -1;
                config[U("host_address")] = web::json::value::string(us(s.hostAddress));
                web::json::value addresses = web::json::value::array();
                web::json::push_back(addresses, web::json::value::string(us(s.hostAddress)));
                config[U("host_addresses")] = addresses;
                config[U("href_mode")] = 2; // IP addresses, never names
                if (s.dnsSd)
                {
                    // Multicast DNS-SD through Avahi works in "local." only. Unset, nmos-cpp takes the
                    // interface's DNS domain (media.int in the lab), and Avahi fails every browse and
                    // advertisement with -65537.
                    config[U("domain")] = web::json::value::string(U("local."));
                }
                else
                {
                    config[U("pri")] = std::numeric_limits<int>::max();
                    config[U("highest_pri")] = std::numeric_limits<int>::max();
                    config[U("authorization_highest_pri")] = std::numeric_limits<int>::max();
                }
                if (!s.registryAddress.empty())
                {
                    config[U("registry_address")] = web::json::value::string(us(s.registryAddress));
                    config[U("registration_port")] = s.registryPort;
                }
                model.settings = config;
                ::nmos::insert_node_default_settings(model.settings);
                logModel.settings = model.settings;
                logModel.level = ::nmos::fields::logging_level(logModel.settings);

                auto implementation =
                    ::nmos::experimental::node_implementation()
                        .on_parse_transport_file([](::nmos::resource const&, ::nmos::resource const&, utility::string_t const&, utility::string_t const&,
                                                     slog::base_gate&) -> web::json::value { throw std::runtime_error("MXL does not use a transport file"); })
                        .on_resolve_auto([this](::nmos::resource const& sender, ::nmos::resource const&, web::json::value& params) {
                            if (params.is_array() && params.size() > 0)
                            {
                                ::nmos::details::resolve_auto(params.at(0), U("mxl_domain_id"), [this] { return web::json::value::string(us(settings.domainId)); });
                                // BCP-007-03: a Sender's active parameters never keep "auto" (AMWA IS-05-01 test_11_01).
                                ::nmos::details::resolve_auto(params.at(0), U("mxl_flow_id"), [&sender] { return sender.data.at(U("flow_id")); });
                            }
                        })
                        .on_set_transportfile([](::nmos::resource const&, ::nmos::resource const&, web::json::value& file) { file = web::json::value::null(); })
                        .on_registration_changed([this](web::uri const& uri) {
                            registered.store(!uri.is_empty());
                            log::info("nmos_registration", {{"registered", !uri.is_empty()}, {"registry", uri.is_empty() ? "" : su(uri.to_string())}});
                        })
                        .on_connection_activated([this](::nmos::resource const&, ::nmos::resource const& connection) {
                            auto const name = senderName(su(connection.id));
                            if (name.empty() || !connection.data.has_field(U("active")))
                            {
                                return;
                            }
                            auto const& active = connection.data.at(U("active"));
                            bool const enabled = active.has_field(U("master_enable")) && active.at(U("master_enable")).as_bool();
                            log::info("sender_enable", {{"sender", name}, {"enabled", enabled}});
                            if (onEnable)
                            {
                                onEnable(name, enabled);
                            }
                        });

                auto server = ::nmos::experimental::make_node_server(model, implementation, logModel, gate);
                server.thread_functions.push_back([this, &model] {
                    auto lock = model.write_lock();
                    build(model);
                    model.notify();
                    running.store(true);
                    model.wait(lock, [&] { return model.shutdown || stopRequested.load(); });
                    if (!model.shutdown)
                    {
                        deregister(model);
                        model.notify();
                        deregistered.store(true);
                        model.wait(lock, [&] { return model.shutdown; });
                    }
                });
                ::nmos::server_guard guard(server);
                for (int i = 0; i < 200 && !running.load(); ++i)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                // nmos-cpp logs a failed bind and carries on: the port must be ours.
                bool listening = false;
                for (int i = 0; i < 40 && running.load() && !listening; ++i)
                {
                    listening = util::ownsListener(s.port);
                    if (!listening)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                }
                if (!listening)
                {
                    error = "cannot listen on NMOS_PORT " + std::to_string(s.port);
                }
                {
                    std::lock_guard lock{startMutex};
                    startDone = true;
                }
                startCv.notify_all();
                if (!error.empty())
                {
                    auto lock = model.write_lock();
                    model.shutdown = true;
                    model.notify();
                    return;
                }
                while (!stopRequested.load())
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                {
                    auto lock = model.write_lock();
                    model.notify(); // wakes the thread function, which deregisters
                }
                for (int i = 0; i < 100 && !deregistered.load(); ++i)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                // Time for the registration thread to send the DELETEs.
                for (int i = 0; i < 40 && registered.load(); ++i)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                {
                    auto lock = model.write_lock();
                    model.shutdown = true;
                    model.notify();
                }
            }
            catch (std::exception const& ex)
            {
                error = ex.what();
                log::error("nmos_node_failed", {{"error", ex.what()}});
                {
                    std::lock_guard lock{startMutex};
                    startDone = true;
                }
                startCv.notify_all();
            }
            finished.store(true);
        }
    };

    Node::Node(NodeSettings settings, std::function<void(std::string const&, bool)> onEnable)
        : _impl(std::make_unique<Impl>())
    {
        _impl->settings = std::move(settings);
        _impl->onEnable = std::move(onEnable);
    }

    Node::~Node()
    {
        stop();
    }

    void Node::start()
    {
        _impl->thread = std::thread([this] { _impl->run(); });
        std::unique_lock lock{_impl->startMutex};
        _impl->startCv.wait_for(lock, std::chrono::seconds(20), [&] { return _impl->startDone; });
        if (!_impl->error.empty() || !_impl->startDone)
        {
            throw std::runtime_error(_impl->error.empty() ? "NMOS node did not start" : _impl->error);
        }
    }

    bool Node::stop()
    {
        _impl->stopRequested.store(true);
        if (!_impl->thread.joinable())
        {
            return true;
        }
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(_impl->settings.shutdownTimeoutS);
        while (!_impl->finished.load() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (_impl->finished.load())
        {
            _impl->thread.join();
            return true;
        }
        _impl->thread.detach();
        log::error("nmos_shutdown_timeout", {{"seconds", _impl->settings.shutdownTimeoutS}});
        return false;
    }

    bool Node::registered() const
    {
        return _impl->registered.load();
    }

    NodeSettings const& Node::settings() const
    {
        return _impl->settings;
    }
}
