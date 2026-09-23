#pragma once

#include "kvstore.grpc.pb.h"
#include "raft/raft.h"

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ftkv::rpc {

struct GrpcClientOptions {
    std::chrono::milliseconds timeout{500};
    std::size_t max_attempts{3};
    std::chrono::milliseconds initial_backoff{10};
    std::chrono::milliseconds maximum_backoff{100};
};

using NodeStatus = raft::NodeSnapshot;

class GrpcError final : public std::runtime_error {
  public:
    GrpcError(std::string operation, const grpc::Status& status, std::size_t attempts);

    [[nodiscard]] grpc::StatusCode status_code() const noexcept;
    [[nodiscard]] std::size_t attempts() const noexcept;

  private:
    grpc::StatusCode status_code_;
    std::size_t attempts_;
};

class GrpcClient final {
  public:
    explicit GrpcClient(std::string target, GrpcClientOptions options = {});
    GrpcClient(std::shared_ptr<grpc::Channel> channel, GrpcClientOptions options = {});

    GrpcClient(const GrpcClient&) = delete;
    GrpcClient& operator=(const GrpcClient&) = delete;

    [[nodiscard]] std::optional<std::string> get(std::string_view key) const;
    void put(std::string key, std::string value) const;
    [[nodiscard]] bool erase(std::string_view key) const;
    [[nodiscard]] NodeStatus status() const;

    [[nodiscard]] raft::RequestVoteResponse
    request_vote(raft::NodeId sender, const raft::RequestVoteRequest& request) const;
    [[nodiscard]] raft::AppendEntriesResponse
    append_entries(raft::NodeId sender, const raft::AppendEntriesRequest& request) const;

  private:
    GrpcClientOptions options_;
    std::unique_ptr<::ftkv::v1::KeyValueStore::Stub> key_value_stub_;
    std::unique_ptr<::ftkv::v1::Raft::Stub> raft_stub_;
};

} // namespace ftkv::rpc
