#pragma once

#include "ftkv/store/key_value_store.hpp"
#include "raft/raft_node.h"

#include <grpcpp/grpcpp.h>

#include <memory>
#include <mutex>
#include <string>

namespace ftkv::rpc {

class GrpcServer final {
  public:
    GrpcServer(std::string listen_address, std::shared_ptr<store::KeyValueStore> key_value_store,
               std::shared_ptr<raft::RaftNode> raft_node);
    ~GrpcServer();

    GrpcServer(const GrpcServer&) = delete;
    GrpcServer& operator=(const GrpcServer&) = delete;

    void start();
    void shutdown() noexcept;

    [[nodiscard]] int bound_port() const;
    [[nodiscard]] std::string endpoint() const;

  private:
    class KeyValueService;
    class RaftService;

    std::string listen_address_;
    std::unique_ptr<KeyValueService> key_value_service_;
    std::unique_ptr<RaftService> raft_service_;
    mutable std::mutex mutex_;
    std::unique_ptr<grpc::Server> server_;
    int bound_port_{0};
};

} // namespace ftkv::rpc
