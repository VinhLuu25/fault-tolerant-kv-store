#include "raft/raft_node.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <limits>
#include <type_traits>
#include <utility>

namespace ftkv::raft {
namespace {

void validate_config(const RaftConfig& config) {
    if (config.heartbeat_interval.count() <= 0 || config.election_timeout_min.count() <= 0 ||
        config.election_timeout_max < config.election_timeout_min ||
        config.election_timeout_min <= config.heartbeat_interval) {
        throw std::invalid_argument{"invalid Raft timing configuration"};
    }

    std::unordered_set<NodeId> members{config.node_id};
    for (const auto peer : config.peers) {
        if (!members.insert(peer).second) {
            throw std::invalid_argument{"Raft members must be unique"};
        }
    }
}

}  // namespace

RaftNode::RaftNode(RaftConfig config, std::shared_ptr<RaftStorage> storage)
    : config_{std::move(config)}, storage_{std::move(storage)},
      random_{config_.random_seed ^ (config_.node_id * 0x9e3779b97f4a7c15ULL)} {
    validate_config(config_);
    if (!storage_) {
        throw std::invalid_argument{"Raft storage must not be null"};
    }

    members_.insert(config_.node_id);
    members_.insert(config_.peers.begin(), config_.peers.end());

    const auto persisted = storage_->load();
    current_term_ = persisted.current_term;
    voted_for_ = persisted.voted_for;
    if (voted_for_.has_value() && !members_.contains(*voted_for_)) {
        throw RaftError{"persisted vote references a non-member"};
    }

    log_.push_back(LogEntry{});  // Sentinel keeps Raft log indexes one-based.
    Term previous_term = 0;
    for (const auto& entry : persisted.log) {
        if (entry.term == 0 || entry.term > current_term_ || entry.term < previous_term) {
            throw RaftError{"persisted Raft log is invalid"};
        }
        log_.push_back(entry);
        previous_term = entry.term;
    }
    reset_election_timeout_locked();
}

void RaftNode::tick(const std::chrono::milliseconds elapsed) {
    if (elapsed.count() < 0) {
        throw std::invalid_argument{"elapsed time must not be negative"};
    }

    const std::scoped_lock lock{mutex_};
    if (state_ == NodeState::leader) {
        if (elapsed >= config_.heartbeat_interval - heartbeat_elapsed_) {
            heartbeat_elapsed_ = std::chrono::milliseconds{0};
            broadcast_append_entries_locked();
        } else {
            heartbeat_elapsed_ += elapsed;
        }
        return;
    }

    if (elapsed >= election_timeout_ - election_elapsed_) {
        start_election_locked();
    } else {
        election_elapsed_ += elapsed;
    }
}

bool RaftNode::step(const Message& message) {
    const std::scoped_lock lock{mutex_};
    return step_locked(message);
}

std::optional<Message> RaftNode::handle_request(const Message& message) {
    const bool is_request = std::holds_alternative<RequestVoteRequest>(message.rpc) ||
                            std::holds_alternative<AppendEntriesRequest>(message.rpc);
    if (!is_request) {
        throw std::invalid_argument{"Raft message must contain a request"};
    }

    const std::scoped_lock lock{mutex_};
    const auto first_new_message = outbox_.size();
    if (!step_locked(message)) {
        return std::nullopt;
    }

    for (auto index = first_new_message; index < outbox_.size(); ++index) {
        const auto is_vote_response = std::holds_alternative<RequestVoteRequest>(message.rpc) &&
                                      std::holds_alternative<RequestVoteResponse>(
                                          outbox_[index].rpc);
        const auto* append_request = std::get_if<AppendEntriesRequest>(&message.rpc);
        const auto* append_response = std::get_if<AppendEntriesResponse>(&outbox_[index].rpc);
        const auto is_append_response = append_request != nullptr && append_response != nullptr &&
                                        append_response->request_id == append_request->request_id;
        if (outbox_[index].to == message.from && (is_vote_response || is_append_response)) {
            auto response = std::move(outbox_[index]);
            outbox_.erase(std::next(outbox_.begin(), static_cast<std::ptrdiff_t>(index)));
            return response;
        }
    }
    return std::nullopt;
}

bool RaftNode::step_locked(const Message& message) {
    if (message.to != config_.node_id || !is_peer_locked(message.from)) {
        return false;
    }

    const auto incoming_term = message_term(message);
    if (incoming_term > current_term_) {
        become_follower_locked(incoming_term, std::nullopt);
    }

    std::visit(
        [this, from = message.from](const auto& rpc) {
            using RpcType = std::decay_t<decltype(rpc)>;
            if constexpr (std::is_same_v<RpcType, RequestVoteRequest>) {
                handle_request_vote_locked(from, rpc);
            } else if constexpr (std::is_same_v<RpcType, RequestVoteResponse>) {
                handle_request_vote_response_locked(from, rpc);
            } else if constexpr (std::is_same_v<RpcType, AppendEntriesRequest>) {
                handle_append_entries_locked(from, rpc);
            } else {
                handle_append_entries_response_locked(from, rpc);
            }
        },
        message.rpc);
    return true;
}

std::optional<LogIndex> RaftNode::propose(std::string command) {
    const std::scoped_lock lock{mutex_};
    if (state_ != NodeState::leader) {
        return std::nullopt;
    }
    if (log_.size() == std::numeric_limits<LogIndex>::max()) {
        throw RaftError{"Raft log index overflow"};
    }

    log_.push_back(LogEntry{current_term_, std::move(command), false});
    persist_locked();
    const auto index = last_log_index_locked();
    static_cast<void>(advance_commit_index_locked());
    broadcast_append_entries_locked();
    return index;
}

std::vector<Message> RaftNode::take_messages() {
    const std::scoped_lock lock{mutex_};
    auto messages = std::move(outbox_);
    outbox_.clear();
    return messages;
}

std::vector<CommittedEntry> RaftNode::take_committed_entries() {
    const std::scoped_lock lock{mutex_};
    auto entries = std::move(committed_entries_);
    committed_entries_.clear();
    return entries;
}

NodeId RaftNode::id() const noexcept { return config_.node_id; }

NodeState RaftNode::state() const {
    const std::scoped_lock lock{mutex_};
    return state_;
}

Term RaftNode::current_term() const {
    const std::scoped_lock lock{mutex_};
    return current_term_;
}

std::optional<NodeId> RaftNode::leader_id() const {
    const std::scoped_lock lock{mutex_};
    return leader_id_;
}

LogIndex RaftNode::commit_index() const {
    const std::scoped_lock lock{mutex_};
    return commit_index_;
}

LogIndex RaftNode::last_applied() const {
    const std::scoped_lock lock{mutex_};
    return last_applied_;
}

std::vector<LogEntry> RaftNode::log_entries() const {
    const std::scoped_lock lock{mutex_};
    return {std::next(log_.begin()), log_.end()};
}

NodeSnapshot RaftNode::snapshot() const {
    const std::scoped_lock lock{mutex_};
    return NodeSnapshot{config_.node_id, state_,         current_term_, leader_id_,
                        commit_index_,   last_applied_, last_log_index_locked()};
}

void RaftNode::reset_election_timeout_locked() {
    const auto minimum = config_.election_timeout_min.count();
    const auto maximum = config_.election_timeout_max.count();
    std::uniform_int_distribution<std::chrono::milliseconds::rep> distribution{minimum, maximum};
    election_timeout_ = std::chrono::milliseconds{distribution(random_)};
    election_elapsed_ = std::chrono::milliseconds{0};
}

void RaftNode::persist_locked() {
    PersistentState state;
    state.current_term = current_term_;
    state.voted_for = voted_for_;
    state.log.assign(std::next(log_.begin()), log_.end());
    storage_->save(state);
}

void RaftNode::become_follower_locked(const Term term, const std::optional<NodeId> leader_id) {
    const bool term_changed = term > current_term_;
    if (term_changed) {
        current_term_ = term;
        voted_for_.reset();
    }
    state_ = NodeState::follower;
    leader_id_ = leader_id;
    votes_received_.clear();
    next_index_.clear();
    match_index_.clear();
    append_request_id_.clear();
    heartbeat_elapsed_ = std::chrono::milliseconds{0};
    reset_election_timeout_locked();
    if (term_changed) {
        persist_locked();
    }
}

void RaftNode::start_election_locked() {
    state_ = NodeState::candidate;
    leader_id_.reset();
    ++current_term_;
    voted_for_ = config_.node_id;
    votes_received_.clear();
    votes_received_.insert(config_.node_id);
    reset_election_timeout_locked();
    persist_locked();

    if (votes_received_.size() >= quorum_size_locked()) {
        become_leader_locked();
        return;
    }

    const RequestVoteRequest request{current_term_, config_.node_id, last_log_index_locked(),
                                     last_log_term_locked()};
    for (const auto peer : config_.peers) {
        send_locked(peer, request);
    }
}

void RaftNode::become_leader_locked() {
    state_ = NodeState::leader;
    leader_id_ = config_.node_id;
    heartbeat_elapsed_ = std::chrono::milliseconds{0};

    const auto next_index = last_log_index_locked() + 1U;
    for (const auto peer : config_.peers) {
        next_index_[peer] = next_index;
        match_index_[peer] = 0;
        append_request_id_[peer] = 0;
    }

    // The current-term no-op safely commits entries inherited from an earlier leader.
    log_.push_back(LogEntry{current_term_, {}, true});
    persist_locked();
    static_cast<void>(advance_commit_index_locked());
    broadcast_append_entries_locked();
}

void RaftNode::handle_request_vote_locked(const NodeId from,
                                          const RequestVoteRequest& request) {
    if (request.candidate_id != from || request.term < current_term_) {
        send_locked(from, RequestVoteResponse{current_term_, false});
        return;
    }

    const bool can_vote = !voted_for_.has_value() || voted_for_ == request.candidate_id;
    const bool grant_vote = can_vote && candidate_log_is_current_locked(request);
    if (grant_vote) {
        if (voted_for_ != request.candidate_id) {
            voted_for_ = request.candidate_id;
            persist_locked();
        }
        reset_election_timeout_locked();
    }
    send_locked(from, RequestVoteResponse{current_term_, grant_vote});
}

void RaftNode::handle_request_vote_response_locked(const NodeId from,
                                                   const RequestVoteResponse& response) {
    if (state_ != NodeState::candidate || response.term != current_term_ ||
        !response.vote_granted) {
        return;
    }

    votes_received_.insert(from);
    if (votes_received_.size() >= quorum_size_locked()) {
        become_leader_locked();
    }
}

void RaftNode::handle_append_entries_locked(const NodeId from,
                                            const AppendEntriesRequest& request) {
    auto reject = [this, from, &request](const LogIndex conflict_index,
                                         const std::optional<Term> conflict_term = std::nullopt) {
        send_locked(from, AppendEntriesResponse{current_term_, false, 0, conflict_index,
                                                conflict_term, request.request_id});
    };

    if (request.leader_id != from || request.term < current_term_) {
        reject(last_log_index_locked() + 1U);
        return;
    }
    if (std::any_of(request.entries.begin(), request.entries.end(),
                    [&request](const LogEntry& entry) {
                        return entry.term == 0 || entry.term > request.term;
                    })) {
        reject(last_log_index_locked() + 1U);
        return;
    }

    if (state_ != NodeState::follower || leader_id_ != request.leader_id) {
        become_follower_locked(current_term_, request.leader_id);
    } else {
        reset_election_timeout_locked();
    }

    if (request.previous_log_index > last_log_index_locked()) {
        reject(last_log_index_locked() + 1U);
        return;
    }
    if (request.previous_log_index > 0 &&
        log_[static_cast<std::size_t>(request.previous_log_index)].term !=
            request.previous_log_term) {
        const auto conflict_term =
            log_[static_cast<std::size_t>(request.previous_log_index)].term;
        auto conflict_index = request.previous_log_index;
        while (conflict_index > 1U &&
               log_[static_cast<std::size_t>(conflict_index - 1U)].term == conflict_term) {
            --conflict_index;
        }
        reject(conflict_index, conflict_term);
        return;
    }

    bool log_changed = false;
    std::size_t incoming_offset = 0;
    auto entry_index = request.previous_log_index + 1U;
    while (incoming_offset < request.entries.size() && entry_index <= last_log_index_locked()) {
        const auto& local_entry = log_[static_cast<std::size_t>(entry_index)];
        const auto& incoming_entry = request.entries[incoming_offset];
        if (local_entry.term != incoming_entry.term) {
            if (entry_index <= commit_index_) {
                reject(entry_index, local_entry.term);
                return;
            }
            log_.resize(static_cast<std::size_t>(entry_index));
            log_changed = true;
            break;
        }
        if (local_entry != incoming_entry) {
            throw RaftError{"log matching invariant violated"};
        }
        ++incoming_offset;
        ++entry_index;
    }

    if (incoming_offset < request.entries.size()) {
        log_.insert(log_.end(), std::next(request.entries.begin(),
                                         static_cast<std::ptrdiff_t>(incoming_offset)),
                    request.entries.end());
        log_changed = true;
    }
    if (log_changed) {
        persist_locked();
    }

    if (request.leader_commit > commit_index_) {
        commit_index_ = std::min(request.leader_commit, last_log_index_locked());
        apply_committed_entries_locked();
    }
    const auto match_index = request.previous_log_index +
                             static_cast<LogIndex>(request.entries.size());
    send_locked(from, AppendEntriesResponse{current_term_, true, match_index, match_index + 1U,
                                            std::nullopt, request.request_id});
}

void RaftNode::handle_append_entries_response_locked(
    const NodeId from, const AppendEntriesResponse& response) {
    if (state_ != NodeState::leader || response.term != current_term_) {
        return;
    }

    if (response.success) {
        if (response.match_index > last_log_index_locked()) {
            return;
        }
        match_index_[from] = std::max(match_index_[from], response.match_index);
        next_index_[from] = std::max(next_index_[from], response.match_index + 1U);
        if (advance_commit_index_locked()) {
            broadcast_append_entries_locked();
        } else if (match_index_[from] < last_log_index_locked()) {
            send_append_entries_locked(from);
        }
        return;
    }

    if (append_request_id_[from] != response.request_id) {
        return;
    }

    auto next_index = response.conflict_index;
    if (response.conflict_term.has_value()) {
        for (auto index = last_log_index_locked(); index > 0; --index) {
            if (log_[static_cast<std::size_t>(index)].term == *response.conflict_term) {
                next_index = index + 1U;
                break;
            }
        }
    }
    next_index = std::clamp(next_index, LogIndex{1}, last_log_index_locked() + 1U);
    next_index_[from] = std::max(next_index, match_index_[from] + 1U);
    send_append_entries_locked(from);
}

void RaftNode::broadcast_append_entries_locked() {
    for (const auto peer : config_.peers) {
        send_append_entries_locked(peer);
    }
}

void RaftNode::send_append_entries_locked(const NodeId peer) {
    const auto next_index = next_index_.at(peer);
    const auto previous_index = next_index - 1U;
    std::vector<LogEntry> entries{std::next(log_.begin(), static_cast<std::ptrdiff_t>(next_index)),
                                  log_.end()};
    const auto request_id = ++append_request_id_[peer];
    send_locked(peer, AppendEntriesRequest{current_term_, config_.node_id, previous_index,
                                           log_[static_cast<std::size_t>(previous_index)].term,
                                           std::move(entries), commit_index_, request_id});
}

void RaftNode::send_locked(const NodeId peer, Rpc rpc) {
    outbox_.push_back(Message{config_.node_id, peer, std::move(rpc)});
}

bool RaftNode::advance_commit_index_locked() {
    for (auto candidate = last_log_index_locked(); candidate > commit_index_; --candidate) {
        if (log_[static_cast<std::size_t>(candidate)].term != current_term_) {
            continue;
        }

        std::size_t replicated = 1;
        for (const auto peer : config_.peers) {
            if (match_index_[peer] >= candidate) {
                ++replicated;
            }
        }
        if (replicated >= quorum_size_locked()) {
            commit_index_ = candidate;
            apply_committed_entries_locked();
            return true;
        }
    }
    return false;
}

void RaftNode::apply_committed_entries_locked() {
    while (last_applied_ < commit_index_) {
        ++last_applied_;
        const auto& entry = log_[static_cast<std::size_t>(last_applied_)];
        if (!entry.is_no_op) {
            committed_entries_.push_back(
                CommittedEntry{last_applied_, entry.term, entry.command});
        }
    }
}

bool RaftNode::candidate_log_is_current_locked(const RequestVoteRequest& request) const {
    const auto local_term = last_log_term_locked();
    return request.last_log_term > local_term ||
           (request.last_log_term == local_term &&
            request.last_log_index >= last_log_index_locked());
}

bool RaftNode::is_peer_locked(const NodeId node_id) const {
    return node_id != config_.node_id && members_.contains(node_id);
}

LogIndex RaftNode::last_log_index_locked() const {
    return static_cast<LogIndex>(log_.size() - 1U);
}

Term RaftNode::last_log_term_locked() const { return log_.back().term; }

std::size_t RaftNode::quorum_size_locked() const { return (members_.size() / 2U) + 1U; }

}  // namespace ftkv::raft
