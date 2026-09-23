#pragma once

#include "raft/raft.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ftkv::raft {

class RaftNode final {
public:
    RaftNode(RaftConfig config, std::shared_ptr<RaftStorage> storage);

    RaftNode(const RaftNode&) = delete;
    RaftNode& operator=(const RaftNode&) = delete;

    void tick(std::chrono::milliseconds elapsed);
    [[nodiscard]] bool step(const Message& message);
    [[nodiscard]] std::optional<Message> handle_request(const Message& message);
    [[nodiscard]] std::optional<LogIndex> propose(std::string command);

    [[nodiscard]] std::vector<Message> take_messages();
    [[nodiscard]] std::vector<CommittedEntry> take_committed_entries();

    [[nodiscard]] NodeId id() const noexcept;
    [[nodiscard]] NodeState state() const;
    [[nodiscard]] Term current_term() const;
    [[nodiscard]] std::optional<NodeId> leader_id() const;
    [[nodiscard]] LogIndex commit_index() const;
    [[nodiscard]] LogIndex last_applied() const;
    [[nodiscard]] std::vector<LogEntry> log_entries() const;

private:
    void reset_election_timeout_locked();
    void persist_locked();
    void become_follower_locked(Term term, std::optional<NodeId> leader_id);
    void start_election_locked();
    void become_leader_locked();

    void handle_request_vote_locked(NodeId from, const RequestVoteRequest& request);
    void handle_request_vote_response_locked(NodeId from, const RequestVoteResponse& response);
    void handle_append_entries_locked(NodeId from, const AppendEntriesRequest& request);
    void handle_append_entries_response_locked(NodeId from,
                                               const AppendEntriesResponse& response);

    void broadcast_append_entries_locked();
    void send_append_entries_locked(NodeId peer);
    void send_locked(NodeId peer, Rpc rpc);
    [[nodiscard]] bool advance_commit_index_locked();
    void apply_committed_entries_locked();
    [[nodiscard]] bool candidate_log_is_current_locked(const RequestVoteRequest& request) const;
    [[nodiscard]] bool is_peer_locked(NodeId node_id) const;
    [[nodiscard]] LogIndex last_log_index_locked() const;
    [[nodiscard]] Term last_log_term_locked() const;
    [[nodiscard]] std::size_t quorum_size_locked() const;
    [[nodiscard]] bool step_locked(const Message& message);

    RaftConfig config_;
    std::shared_ptr<RaftStorage> storage_;
    mutable std::mutex mutex_;
    std::mt19937_64 random_;

    NodeState state_{NodeState::follower};
    Term current_term_{0};
    std::optional<NodeId> voted_for_;
    std::optional<NodeId> leader_id_;
    std::vector<LogEntry> log_;
    LogIndex commit_index_{0};
    LogIndex last_applied_{0};

    std::chrono::milliseconds election_elapsed_{0};
    std::chrono::milliseconds election_timeout_{0};
    std::chrono::milliseconds heartbeat_elapsed_{0};

    std::unordered_set<NodeId> members_;
    std::unordered_set<NodeId> votes_received_;
    std::unordered_map<NodeId, LogIndex> next_index_;
    std::unordered_map<NodeId, LogIndex> match_index_;
    std::unordered_map<NodeId, std::uint64_t> append_request_id_;
    std::vector<Message> outbox_;
    std::vector<CommittedEntry> committed_entries_;
};

}  // namespace ftkv::raft
