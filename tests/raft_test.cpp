#include "raft/raft_node.h"

#include <chrono>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using ftkv::raft::AppendEntriesRequest;
using ftkv::raft::CommittedEntry;
using ftkv::raft::InMemoryRaftStorage;
using ftkv::raft::LogEntry;
using ftkv::raft::Message;
using ftkv::raft::NodeId;
using ftkv::raft::NodeState;
using ftkv::raft::PersistentState;
using ftkv::raft::RaftConfig;
using ftkv::raft::RaftNode;
using ftkv::raft::RaftStorage;
using ftkv::raft::RequestVoteRequest;
using ftkv::raft::RequestVoteResponse;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

[[nodiscard]] bool contains_command(const std::vector<CommittedEntry>& entries,
                                    const std::string_view command) {
    for (const auto& entry : entries) {
        if (entry.command == command) {
            return true;
        }
    }
    return false;
}

class TestCluster final {
  public:
    explicit TestCluster(std::vector<NodeId> node_ids) {
        for (const auto node_id : node_ids) {
            RaftConfig config;
            config.node_id = node_id;
            config.election_timeout_min = 150ms;
            config.election_timeout_max = 150ms;
            config.heartbeat_interval = 50ms;
            for (const auto peer : node_ids) {
                if (peer != node_id) {
                    config.peers.push_back(peer);
                }
            }

            auto storage = std::make_shared<InMemoryRaftStorage>();
            storage_.emplace(node_id, storage);
            nodes_.emplace(node_id, std::make_unique<RaftNode>(config, storage));
        }
    }

    [[nodiscard]] RaftNode& node(const NodeId node_id) { return *nodes_.at(node_id); }

    void tick(const NodeId node_id, const std::chrono::milliseconds elapsed) {
        node(node_id).tick(elapsed);
    }

    void partition(const NodeId first, const NodeId second) {
        blocked_.emplace(first, second);
        blocked_.emplace(second, first);
    }

    void isolate(const NodeId node_id) {
        for (const auto& [peer, unused] : nodes_) {
            static_cast<void>(unused);
            if (peer != node_id) {
                partition(node_id, peer);
            }
        }
    }

    void heal() { blocked_.clear(); }

    void deliver_all(std::vector<Message> initial_messages = {}) {
        std::deque<Message> pending{std::make_move_iterator(initial_messages.begin()),
                                    std::make_move_iterator(initial_messages.end())};
        drain_all_nodes(pending);

        std::size_t delivered = 0;
        while (!pending.empty()) {
            if (++delivered > 10'000U) {
                throw std::runtime_error{"Raft test transport did not quiesce"};
            }

            auto message = std::move(pending.front());
            pending.pop_front();
            if (blocked_.contains({message.from, message.to})) {
                continue;
            }

            static_cast<void>(node(message.to).step(message));
            auto responses = node(message.to).take_messages();
            for (auto& response : responses) {
                pending.push_back(std::move(response));
            }
        }
    }

  private:
    void drain_all_nodes(std::deque<Message>& pending) {
        for (auto& [unused, raft_node] : nodes_) {
            static_cast<void>(unused);
            auto messages = raft_node->take_messages();
            for (auto& message : messages) {
                pending.push_back(std::move(message));
            }
        }
    }

    std::map<NodeId, std::shared_ptr<RaftStorage>> storage_;
    std::map<NodeId, std::unique_ptr<RaftNode>> nodes_;
    std::set<std::pair<NodeId, NodeId>> blocked_;
};

void elect(TestCluster& cluster, const NodeId candidate) {
    cluster.tick(candidate, 150ms);
    cluster.deliver_all();
    expect(cluster.node(candidate).state() == NodeState::leader,
           "candidate must win an election with a majority");
}

void test_leader_election_and_failover() {
    TestCluster cluster{{1, 2, 3}};
    elect(cluster, 1);

    expect(cluster.node(1).current_term() == 1, "first election must use term one");
    expect(cluster.node(2).state() == NodeState::follower, "non-leader must remain follower");
    expect(cluster.node(3).state() == NodeState::follower, "non-leader must remain follower");
    expect(cluster.node(2).leader_id() == 1, "follower must learn the elected leader");
    expect(cluster.node(1).commit_index() == 1, "leader no-op must commit through a quorum");

    cluster.isolate(1);
    cluster.tick(2, 150ms);
    cluster.deliver_all();
    expect(cluster.node(2).state() == NodeState::leader,
           "a majority must elect a replacement for an unavailable leader");
    expect(cluster.node(2).current_term() == 2, "replacement leader must advance the term");

    cluster.heal();
    cluster.tick(2, 50ms);
    cluster.deliver_all();
    expect(cluster.node(1).state() == NodeState::follower,
           "recovered old leader must step down on the higher term");
    expect(cluster.node(1).leader_id() == 2, "recovered node must recognize the new leader");
}

void test_heartbeat_resets_election_timeout() {
    TestCluster cluster{{1, 2, 3}};
    elect(cluster, 1);

    cluster.tick(2, 140ms);
    cluster.tick(3, 140ms);
    cluster.tick(1, 50ms);
    auto heartbeats = cluster.node(1).take_messages();
    expect(heartbeats.size() == 2, "leader must send one heartbeat to each follower");
    for (const auto& heartbeat : heartbeats) {
        const auto* request = std::get_if<AppendEntriesRequest>(&heartbeat.rpc);
        expect(request != nullptr && request->entries.empty(),
               "heartbeat must be an empty AppendEntries request");
    }
    cluster.deliver_all(std::move(heartbeats));

    cluster.tick(2, 140ms);
    cluster.tick(3, 140ms);
    expect(cluster.node(2).state() == NodeState::follower,
           "heartbeat must reset the follower election timeout");
    expect(cluster.node(3).state() == NodeState::follower,
           "heartbeat must prevent unnecessary elections");
    expect(cluster.node(2).current_term() == 1, "heartbeat must preserve the current term");
}

void test_log_replication_and_majority_commit() {
    TestCluster cluster{{1, 2, 3}};
    elect(cluster, 1);
    cluster.partition(1, 3);

    const auto index = cluster.node(1).propose("PUT customer=42");
    expect(index.has_value(), "leader must accept a client command");
    cluster.deliver_all();

    expect(cluster.node(1).commit_index() == *index,
           "leader must commit after replication to a majority");
    expect(cluster.node(2).commit_index() == *index,
           "leader must propagate the commit index to followers");
    expect(cluster.node(3).commit_index() < *index,
           "isolated follower must not report an unobserved commit");
    expect(contains_command(cluster.node(1).take_committed_entries(), "PUT customer=42"),
           "leader must publish the committed command");
    expect(contains_command(cluster.node(2).take_committed_entries(), "PUT customer=42"),
           "quorum follower must apply the committed command");

    cluster.heal();
    cluster.tick(1, 50ms);
    cluster.deliver_all();
    expect(cluster.node(3).commit_index() == *index,
           "recovered follower must catch up through normal replication");
    expect(cluster.node(3).log_entries() == cluster.node(1).log_entries(),
           "recovered follower log must converge with the leader");

    cluster.isolate(1);
    const auto uncommitted_index = cluster.node(1).propose("DELETE customer");
    expect(uncommitted_index.has_value(), "isolated leader may append a local command");
    cluster.deliver_all();
    expect(cluster.node(1).commit_index() == *index,
           "leader must not commit without a majority quorum");
}

void test_leader_recovery_repairs_conflicting_log() {
    TestCluster cluster{{1, 2, 3}};
    elect(cluster, 1);
    cluster.isolate(1);

    const auto stale_index = cluster.node(1).propose("stale-command");
    expect(stale_index == 2, "isolated leader must append the uncommitted entry locally");
    cluster.deliver_all();

    cluster.tick(2, 150ms);
    cluster.deliver_all();
    expect(cluster.node(2).state() == NodeState::leader,
           "remaining quorum must recover leadership");

    cluster.heal();
    cluster.tick(2, 50ms);
    cluster.deliver_all();
    expect(cluster.node(1).state() == NodeState::follower,
           "former leader must recover as a follower");
    expect(cluster.node(1).log_entries() == cluster.node(2).log_entries(),
           "new leader must replace an uncommitted conflicting suffix");
    expect(cluster.node(1).log_entries().at(1) == LogEntry{2, {}, true},
           "recovered log must contain the new leader term no-op");
}

void test_persistent_state_survives_restart() {
    auto storage = std::make_shared<InMemoryRaftStorage>();
    RaftConfig config;
    config.node_id = 7;
    config.election_timeout_min = 150ms;
    config.election_timeout_max = 150ms;
    config.heartbeat_interval = 50ms;

    {
        RaftNode node{config, storage};
        node.tick(150ms);
        expect(node.state() == NodeState::leader, "single node must elect itself");
        expect(node.propose("durable-command").has_value(), "leader must accept a command");
    }

    RaftNode recovered{config, storage};
    expect(recovered.state() == NodeState::follower, "restarted node must begin as follower");
    expect(recovered.current_term() == 1, "current term must survive restart");
    expect(recovered.log_entries().size() == 2, "Raft log must survive restart");

    recovered.tick(150ms);
    expect(recovered.state() == NodeState::leader, "restarted single node must regain leadership");
    expect(recovered.current_term() == 2, "new election after restart must advance the term");
    expect(contains_command(recovered.take_committed_entries(), "durable-command"),
           "new leader no-op must commit the inherited command");
}

void test_vote_requires_an_up_to_date_log() {
    PersistentState persisted;
    persisted.current_term = 2;
    persisted.log = {LogEntry{1, "first", false}, LogEntry{2, "second", false}};
    auto storage = std::make_shared<InMemoryRaftStorage>(persisted);

    RaftConfig config;
    config.node_id = 1;
    config.peers = {2, 3};
    RaftNode voter{config, storage};

    const Message stale_request{2, 1, RequestVoteRequest{3, 2, 10, 1}};
    expect(voter.step(stale_request), "vote request from a member must be processed");
    auto responses = voter.take_messages();
    const auto* stale_response = std::get_if<RequestVoteResponse>(&responses.at(0).rpc);
    expect(stale_response != nullptr && !stale_response->vote_granted,
           "longer log with an older last term must not receive a vote");

    const Message current_request{3, 1, RequestVoteRequest{3, 3, 2, 2}};
    expect(voter.step(current_request), "current-log vote request must be processed");
    responses = voter.take_messages();
    const auto* current_response = std::get_if<RequestVoteResponse>(&responses.at(0).rpc);
    expect(current_response != nullptr && current_response->vote_granted,
           "candidate with an equally current log must receive the available vote");

    expect(voter.step(stale_request), "second candidate request must be processed");
    responses = voter.take_messages();
    const auto* second_response = std::get_if<RequestVoteResponse>(&responses.at(0).rpc);
    expect(second_response != nullptr && !second_response->vote_granted,
           "server must grant at most one vote in a term");
}

} // namespace

int main() {
    test_leader_election_and_failover();
    test_heartbeat_resets_election_timeout();
    test_log_replication_and_majority_commit();
    test_leader_recovery_repairs_conflicting_log();
    test_persistent_state_survives_restart();
    test_vote_requires_an_up_to_date_log();
    return EXIT_SUCCESS;
}
