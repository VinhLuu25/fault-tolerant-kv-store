#include "grpc/grpc_client.h"
#include "grpc/grpc_server.h"
#include "ftkv/store/in_memory_key_value_store.hpp"
#include "raft/raft_node.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace std::chrono_literals;
using ftkv::raft::AppendEntriesRequest;
using ftkv::raft::InMemoryRaftStorage;
using ftkv::raft::LogEntry;
using ftkv::raft::RaftConfig;
using ftkv::raft::RaftNode;
using ftkv::raft::RequestVoteRequest;
using ftkv::rpc::GrpcClient;
using ftkv::rpc::GrpcClientOptions;
using ftkv::rpc::GrpcError;
using ftkv::rpc::GrpcServer;
using ftkv::store::InMemoryKeyValueStore;
using ftkv::store::KeyValueStore;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

[[nodiscard]] std::shared_ptr<RaftNode> make_raft_node() {
    RaftConfig config;
    config.node_id = 2;
    config.peers = {1};
    config.election_timeout_min = 150ms;
    config.election_timeout_max = 300ms;
    config.heartbeat_interval = 50ms;
    return std::make_shared<RaftNode>(config, std::make_shared<InMemoryRaftStorage>());
}

class DelayedStore final : public KeyValueStore {
public:
    void put(std::string, std::string) override { std::this_thread::sleep_for(30ms); }

    [[nodiscard]] std::optional<std::string> get(std::string_view) const override {
        std::this_thread::sleep_for(30ms);
        return std::nullopt;
    }

    [[nodiscard]] bool erase(std::string_view) override {
        std::this_thread::sleep_for(30ms);
        return false;
    }
};

struct ServerFixture {
    ServerFixture() {
        store = std::make_shared<InMemoryKeyValueStore>();
        raft_node = make_raft_node();
        server = std::make_unique<GrpcServer>("127.0.0.1:0", store, raft_node);
        server->start();
    }

    std::shared_ptr<InMemoryKeyValueStore> store;
    std::shared_ptr<RaftNode> raft_node;
    std::unique_ptr<GrpcServer> server;
};

[[nodiscard]] GrpcClientOptions client_options() {
    GrpcClientOptions options;
    options.timeout = 200ms;
    options.max_attempts = 3;
    options.initial_backoff = 1ms;
    options.maximum_backoff = 2ms;
    return options;
}

void test_deadline_handling() {
    GrpcServer server{"127.0.0.1:0", std::make_shared<DelayedStore>(), make_raft_node()};
    server.start();

    auto options = client_options();
    options.timeout = 5ms;
    options.max_attempts = 2;
    GrpcClient client{server.endpoint(), options};
    try {
        static_cast<void>(client.get("slow"));
        expect(false, "request exceeding its deadline must fail");
    } catch (const GrpcError& error) {
        expect(error.status_code() == grpc::StatusCode::DEADLINE_EXCEEDED,
               "slow request must report deadline exceeded");
        expect(error.attempts() == 2, "deadline failures must respect the retry budget");
    }
}

void test_client_requests_and_retries() {
    ServerFixture fixture;
    GrpcClient client{fixture.server->endpoint(), client_options()};
    const std::string binary_value{"value\0bytes", 11};

    expect(!client.get("missing").has_value(), "Get must report a missing key");
    client.put("key", binary_value);
    expect(client.get("key") == binary_value, "Put and Get must preserve binary values");
    expect(client.erase("key"), "Delete must report an existing key");
    expect(!client.get("key").has_value(), "Delete must remove the key");

    fixture.server->shutdown();
    try {
        static_cast<void>(client.get("unavailable"));
        expect(false, "unavailable server must produce a client error");
    } catch (const GrpcError& error) {
        expect(error.attempts() == 3, "transient failures must use the configured retry budget");
        expect(error.status_code() == grpc::StatusCode::UNAVAILABLE ||
                   error.status_code() == grpc::StatusCode::DEADLINE_EXCEEDED,
               "unavailable server must return a transport or deadline error");
    }
}

void test_node_communication() {
    ServerFixture fixture;
    GrpcClient client{fixture.server->endpoint(), client_options()};

    const auto vote = client.request_vote(1, RequestVoteRequest{1, 1, 0, 0});
    expect(vote.term == 1 && vote.vote_granted,
           "RequestVote RPC must grant an eligible candidate");

    AppendEntriesRequest append;
    append.term = 1;
    append.leader_id = 1;
    append.entries = {LogEntry{1, "PUT replicated=value", false}};
    append.leader_commit = 1;
    append.request_id = 42;
    const auto response = client.append_entries(1, append);

    expect(response.success, "AppendEntries RPC must replicate a matching log entry");
    expect(response.match_index == 1 && response.request_id == 42,
           "AppendEntries response must identify the replicated request");
    expect(fixture.raft_node->commit_index() == 1,
           "follower must advance its commit index from the leader");
    const auto committed = fixture.raft_node->take_committed_entries();
    expect(committed.size() == 1 && committed.front().command == "PUT replicated=value",
           "replicated command must be published to the state machine");

    try {
        static_cast<void>(client.request_vote(9, RequestVoteRequest{2, 9, 1, 1}));
        expect(false, "RPC from a non-member must fail");
    } catch (const GrpcError& error) {
        expect(error.status_code() == grpc::StatusCode::INVALID_ARGUMENT,
               "invalid peer must map to INVALID_ARGUMENT");
        expect(error.attempts() == 1, "non-transient errors must not be retried");
    }
}

}  // namespace

int main() {
    test_client_requests_and_retries();
    test_deadline_handling();
    test_node_communication();
    return EXIT_SUCCESS;
}
