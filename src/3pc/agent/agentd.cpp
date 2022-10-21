// Copyright (c) 2021 MIT Digital Currency Initiative,
//                    Federal Reserve Bank of Boston
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "broker/impl.hpp"
#include "crypto/sha256.h"
#include "directory/impl.hpp"
#include "format.hpp"
#include "impl.hpp"
#include "runners/lua/server.hpp"
#include "runtime_locking_shard/client.hpp"
#include "ticket_machine/client.hpp"
#include "util.hpp"
#include "util/common/logging.hpp"
#include "util/rpc/format.hpp"
#include "util/rpc/tcp_server.hpp"
#include "util/serialization/format.hpp"

#include <csignal>

auto main(int argc, char** argv) -> int {
    auto log = std::make_shared<cbdc::logging::log>(
        cbdc::logging::log_level::trace);

    auto sha2_impl = SHA256AutoDetect();
    log->info("using sha2: ", sha2_impl);

    auto cfg = cbdc::threepc::read_config(argc, argv);
    if(!cfg.has_value()) {
        log->error("Error parsing options");
        return 1;
    }

    log->set_loglevel(cfg->m_loglevel);

    if(cfg->m_agent_endpoints.size() <= cfg->m_component_id) {
        log->error("No endpoint for component id");
        return 1;
    }

    log->info("Connecting to shards...");

    auto shards = std::vector<
        std::shared_ptr<cbdc::threepc::runtime_locking_shard::interface>>();
    for(const auto& shard_ep : cfg->m_shard_endpoints) {
        auto client = std::make_shared<
            cbdc::threepc::runtime_locking_shard::rpc::client>(
            std::vector<cbdc::network::endpoint_t>{shard_ep});
        if(!client->init()) {
            log->error("Error connecting to shard");
            return 1;
        }
        shards.emplace_back(client);
    }

    log->info("Connected to shards, connecting to ticketer...");

    auto ticketer
        = std::make_shared<cbdc::threepc::ticket_machine::rpc::client>(
            std::vector<cbdc::network::endpoint_t>{
                cfg->m_ticket_machine_endpoints});
    if(!ticketer->init()) {
        log->error("Error connecting to ticket machine");
        return 1;
    }

    auto directory
        = std::make_shared<cbdc::threepc::directory::impl>(shards.size());
    auto broker
        = std::make_shared<cbdc::threepc::broker::impl>(cfg->m_component_id,
                                                        shards,
                                                        ticketer,
                                                        directory,
                                                        log);

    log->info("Requesting broker recovery...");

    auto recover_success = std::promise<bool>();
    auto recover_fut = recover_success.get_future();
    auto success = broker->recover(
        [&](cbdc::threepc::broker::interface::recover_return_type res) {
            recover_success.set_value(!res.has_value());
        });
    if(!success) {
        log->error("Error requesting broker recovery");
        return 1;
    }

    constexpr auto recover_delay = std::chrono::seconds(60);
    auto wait_res = recover_fut.wait_for(recover_delay);
    if(wait_res == std::future_status::timeout) {
        log->error("Timeout waiting for broker recovery");
        return 1;
    }
    auto recover_res = recover_fut.get();
    if(!recover_res) {
        log->error("Error during broker recovery");
        return 1;
    }

    auto server
        = std::unique_ptr<cbdc::threepc::agent::rpc::server_interface>();

    if(cfg->m_runner_type == cbdc::threepc::runner_type::lua) {
        auto rpc_server = std::make_unique<
            cbdc::rpc::async_tcp_server<cbdc::threepc::agent::rpc::request,
                                        cbdc::threepc::agent::rpc::response>>(
            cfg->m_agent_endpoints[cfg->m_component_id]);
        server = std::make_unique<cbdc::threepc::agent::rpc::server>(
            std::move(rpc_server),
            broker,
            log,
            cfg.value());
    } else {
        log->error("Unknown runner type");
        return 1;
    }

    if(!server->init()) {
        log->error("Error listening on RPC interface");
        return 1;
    }

    static auto running = std::atomic_bool{true};

    std::signal(SIGINT, [](int /* signal */) {
        running = false;
    });

    log->info("Agent running");

    while(running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    log->info("Shutting down...");

    return 0;
}