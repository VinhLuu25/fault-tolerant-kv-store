#pragma once

#include "ftkv/store/key_value_store.hpp"
#include "grpc/grpc_client.h"
#include "grpc/grpc_server.h"
#include "raft/raft_node.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace ftkv::node {

struct NodeRuntimeConfig {
    raft::NodeId node_id{0};
    std::string listen_address;
    std::filesystem::path data_directory;
    std::map<raft::NodeId, std::string> peers;
    std::chrono::milliseconds election_timeout_min{300};
    std::chrono::milliseconds election_timeout_max{500};
    std::chrono::milliseconds heartbeat_interval{100};

    [[nodiscard]] static NodeRuntimeConfig from_environment();
};

class NodeRuntime final {
  public:
    explicit NodeRuntime(NodeRuntimeConfig config);
    ~NodeRuntime();

    NodeRuntime(const NodeRuntime&) = delete;
    NodeRuntime& operator=(const NodeRuntime&) = delete;

    void start();
    void stop() noexcept;

  private:
    void ticker_loop();
    void worker_loop();
    void enqueue_messages(std::vector<raft::Message> messages);
    void send_message(const raft::Message& message);

    NodeRuntimeConfig config_;
    std::shared_ptr<raft::RaftNode> raft_node_;
    std::shared_ptr<store::KeyValueStore> key_value_store_;
    std::unique_ptr<rpc::GrpcServer> server_;
    std::map<raft::NodeId, std::unique_ptr<rpc::GrpcClient>> peer_clients_;

    std::atomic_bool running_{false};
    std::mutex queue_mutex_;
    std::condition_variable queue_ready_;
    std::deque<raft::Message> message_queue_;
    std::thread ticker_;
    std::vector<std::thread> workers_;
};

[[nodiscard]] std::string_view state_name(raft::NodeState state) noexcept;

} // namespace ftkv::node
