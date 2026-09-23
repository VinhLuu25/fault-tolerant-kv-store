#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace ftkv::raft {

using NodeId = std::uint64_t;
using Term = std::uint64_t;
using LogIndex = std::uint64_t;

enum class NodeState { follower, candidate, leader };

struct LogEntry {
    Term term{0};
    std::string command;
    bool is_no_op{false};

    bool operator==(const LogEntry&) const = default;
};

struct CommittedEntry {
    LogIndex index{0};
    Term term{0};
    std::string command;

    bool operator==(const CommittedEntry&) const = default;
};

struct NodeSnapshot {
    NodeId node_id{0};
    NodeState state{NodeState::follower};
    Term current_term{0};
    std::optional<NodeId> leader_id;
    LogIndex commit_index{0};
    LogIndex last_applied{0};
    LogIndex last_log_index{0};
};

struct RequestVoteRequest {
    Term term{0};
    NodeId candidate_id{0};
    LogIndex last_log_index{0};
    Term last_log_term{0};
};

struct RequestVoteResponse {
    Term term{0};
    bool vote_granted{false};
};

struct AppendEntriesRequest {
    Term term{0};
    NodeId leader_id{0};
    LogIndex previous_log_index{0};
    Term previous_log_term{0};
    std::vector<LogEntry> entries;
    LogIndex leader_commit{0};
    std::uint64_t request_id{0};
};

struct AppendEntriesResponse {
    Term term{0};
    bool success{false};
    LogIndex match_index{0};
    LogIndex conflict_index{1};
    std::optional<Term> conflict_term;
    std::uint64_t request_id{0};
};

using Rpc = std::variant<RequestVoteRequest, RequestVoteResponse, AppendEntriesRequest,
                         AppendEntriesResponse>;

struct Message {
    NodeId from{0};
    NodeId to{0};
    Rpc rpc;
};

struct RaftConfig {
    NodeId node_id{0};
    std::vector<NodeId> peers;
    std::chrono::milliseconds election_timeout_min{150};
    std::chrono::milliseconds election_timeout_max{300};
    std::chrono::milliseconds heartbeat_interval{50};
    std::uint64_t random_seed{0x72616674U};
};

struct PersistentState {
    Term current_term{0};
    std::optional<NodeId> voted_for;
    std::vector<LogEntry> log;
};

class RaftStorage {
  public:
    virtual ~RaftStorage() = default;

    [[nodiscard]] virtual PersistentState load() const = 0;
    virtual void save(const PersistentState& state) = 0;
};

class InMemoryRaftStorage final : public RaftStorage {
  public:
    explicit InMemoryRaftStorage(PersistentState initial_state = {});

    [[nodiscard]] PersistentState load() const override;
    void save(const PersistentState& state) override;

  private:
    mutable std::mutex mutex_;
    PersistentState state_;
};

class RaftError final : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] Term message_term(const Message& message);

} // namespace ftkv::raft
