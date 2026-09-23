#include "node/node_runtime.h"

#include "storage/kv_store.h"

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace ftkv::node {
namespace {

constexpr std::string_view kRaftStateKey{"raft-hard-state"};
constexpr std::string_view kRaftStateMagic{"FTKVRAFT1"};
constexpr std::uint64_t kMaximumLogEntries = 1'000'000;
constexpr std::uint64_t kMaximumCommandBytes = 64U * 1024U * 1024U;
std::mutex log_mutex;

[[nodiscard]] std::string require_environment(const char* name) {
    const auto* value = std::getenv(name);
    if (value == nullptr || std::string_view{value}.empty()) {
        throw std::invalid_argument{std::string{"missing environment variable: "} + name};
    }
    return value;
}

template <typename Integer>
[[nodiscard]] Integer parse_integer(const std::string_view value, const std::string_view name) {
    Integer result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) {
        throw std::invalid_argument{std::string{"invalid integer for "} + std::string{name}};
    }
    return result;
}

[[nodiscard]] std::chrono::milliseconds environment_duration(const char* name,
                                                             const std::int64_t fallback) {
    const auto* value = std::getenv(name);
    if (value == nullptr) {
        return std::chrono::milliseconds{fallback};
    }
    return std::chrono::milliseconds{parse_integer<std::int64_t>(value, name)};
}

[[nodiscard]] std::map<raft::NodeId, std::string> parse_peers(const std::string_view value,
                                                              const raft::NodeId local_id) {
    std::map<raft::NodeId, std::string> peers;
    std::size_t offset = 0;
    while (offset < value.size()) {
        const auto delimiter = value.find(',', offset);
        const auto item = value.substr(offset, delimiter - offset);
        const auto separator = item.find('=');
        if (separator == std::string_view::npos || separator == 0 || separator + 1 >= item.size()) {
            throw std::invalid_argument{"FTKV_PEERS must use id=host:port entries"};
        }

        const auto peer_id = parse_integer<raft::NodeId>(item.substr(0, separator), "FTKV_PEERS");
        if (peer_id == local_id ||
            !peers.emplace(peer_id, std::string{item.substr(separator + 1)}).second) {
            throw std::invalid_argument{"FTKV_PEERS contains a duplicate or local node ID"};
        }
        if (delimiter == std::string_view::npos) {
            break;
        }
        offset = delimiter + 1;
    }
    if (peers.empty()) {
        throw std::invalid_argument{"FTKV_PEERS must contain at least one peer"};
    }
    return peers;
}

void append_uint64(std::string& output, const std::uint64_t value) {
    for (unsigned int shift = 0; shift < 64U; shift += 8U) {
        output.push_back(static_cast<char>((value >> shift) & 0xffU));
    }
}

[[nodiscard]] std::uint64_t read_uint64(const std::string_view input, std::size_t& offset) {
    if (offset > input.size() || input.size() - offset < sizeof(std::uint64_t)) {
        throw raft::RaftError{"persisted Raft state is truncated"};
    }
    std::uint64_t value = 0;
    for (unsigned int shift = 0; shift < 64U; shift += 8U) {
        value |=
            static_cast<std::uint64_t>(static_cast<unsigned char>(input[offset + (shift / 8U)]))
            << shift;
    }
    offset += sizeof(std::uint64_t);
    return value;
}

[[nodiscard]] raft::PersistentState decode_state(const std::string_view bytes) {
    if (!bytes.starts_with(kRaftStateMagic)) {
        throw raft::RaftError{"persisted Raft state has an unsupported format"};
    }
    std::size_t offset = kRaftStateMagic.size();
    raft::PersistentState state;
    state.current_term = read_uint64(bytes, offset);
    if (offset >= bytes.size()) {
        throw raft::RaftError{"persisted Raft state is truncated"};
    }
    const bool has_vote = bytes[offset++] != '\0';
    if (has_vote) {
        state.voted_for = read_uint64(bytes, offset);
    }

    const auto entry_count = read_uint64(bytes, offset);
    if (entry_count > kMaximumLogEntries) {
        throw raft::RaftError{"persisted Raft log exceeds the entry limit"};
    }
    state.log.reserve(static_cast<std::size_t>(entry_count));
    for (std::uint64_t index = 0; index < entry_count; ++index) {
        const auto term = read_uint64(bytes, offset);
        if (offset >= bytes.size()) {
            throw raft::RaftError{"persisted Raft state is truncated"};
        }
        const bool is_no_op = bytes[offset++] != '\0';
        const auto command_size = read_uint64(bytes, offset);
        if (command_size > kMaximumCommandBytes || offset > bytes.size() ||
            command_size > bytes.size() - offset) {
            throw raft::RaftError{"persisted Raft command has an invalid size"};
        }
        state.log.push_back(raft::LogEntry{
            term, std::string{bytes.substr(offset, static_cast<std::size_t>(command_size))},
            is_no_op});
        offset += static_cast<std::size_t>(command_size);
    }
    if (offset != bytes.size()) {
        throw raft::RaftError{"persisted Raft state contains trailing data"};
    }
    return state;
}

[[nodiscard]] std::string encode_state(const raft::PersistentState& state) {
    if (state.log.size() > kMaximumLogEntries) {
        throw raft::RaftError{"Raft log exceeds the persistence entry limit"};
    }

    std::string bytes{kRaftStateMagic};
    append_uint64(bytes, state.current_term);
    bytes.push_back(state.voted_for.has_value() ? '\1' : '\0');
    if (state.voted_for.has_value()) {
        append_uint64(bytes, *state.voted_for);
    }
    append_uint64(bytes, static_cast<std::uint64_t>(state.log.size()));
    for (const auto& entry : state.log) {
        if (entry.command.size() > kMaximumCommandBytes) {
            throw raft::RaftError{"Raft command exceeds the persistence size limit"};
        }
        append_uint64(bytes, entry.term);
        bytes.push_back(entry.is_no_op ? '\1' : '\0');
        append_uint64(bytes, static_cast<std::uint64_t>(entry.command.size()));
        bytes.append(entry.command);
    }
    return bytes;
}

class DurableRaftStorage final : public raft::RaftStorage {
  public:
    explicit DurableRaftStorage(const std::filesystem::path& file_path)
        : store_{std::make_unique<storage::FilePersistence>(file_path)} {}

    [[nodiscard]] raft::PersistentState load() const override {
        const auto encoded = store_.get(kRaftStateKey);
        return encoded.has_value() ? decode_state(*encoded) : raft::PersistentState{};
    }

    void save(const raft::PersistentState& state) override {
        store_.put(std::string{kRaftStateKey}, encode_state(state));
    }

  private:
    storage::KvStore store_;
};

[[nodiscard]] raft::RaftConfig raft_config(const NodeRuntimeConfig& config) {
    raft::RaftConfig result;
    result.node_id = config.node_id;
    result.election_timeout_min = config.election_timeout_min;
    result.election_timeout_max = config.election_timeout_max;
    result.heartbeat_interval = config.heartbeat_interval;
    result.random_seed ^= config.node_id;
    for (const auto& [peer_id, unused] : config.peers) {
        static_cast<void>(unused);
        result.peers.push_back(peer_id);
    }
    return result;
}

} // namespace

NodeRuntimeConfig NodeRuntimeConfig::from_environment() {
    NodeRuntimeConfig config;
    config.node_id =
        parse_integer<raft::NodeId>(require_environment("FTKV_NODE_ID"), "FTKV_NODE_ID");
    config.listen_address = require_environment("FTKV_LISTEN_ADDRESS");
    config.data_directory = require_environment("FTKV_DATA_DIR");
    config.peers = parse_peers(require_environment("FTKV_PEERS"), config.node_id);
    config.election_timeout_min = environment_duration("FTKV_ELECTION_TIMEOUT_MIN_MS", 300);
    config.election_timeout_max = environment_duration("FTKV_ELECTION_TIMEOUT_MAX_MS", 500);
    config.heartbeat_interval = environment_duration("FTKV_HEARTBEAT_INTERVAL_MS", 100);
    return config;
}

NodeRuntime::NodeRuntime(NodeRuntimeConfig config) : config_{std::move(config)} {
    auto raft_storage =
        std::make_shared<DurableRaftStorage>(config_.data_directory / "raft.snapshot");
    raft_node_ = std::make_shared<raft::RaftNode>(raft_config(config_), std::move(raft_storage));
    key_value_store_ = std::make_shared<storage::KvStore>(
        std::make_unique<storage::FilePersistence>(config_.data_directory / "kv.snapshot"));
    server_ =
        std::make_unique<rpc::GrpcServer>(config_.listen_address, key_value_store_, raft_node_);

    rpc::GrpcClientOptions options;
    options.timeout = std::chrono::milliseconds{150};
    options.max_attempts = 2;
    options.initial_backoff = std::chrono::milliseconds{10};
    options.maximum_backoff = std::chrono::milliseconds{25};
    for (const auto& [peer_id, endpoint] : config_.peers) {
        peer_clients_.emplace(peer_id, std::make_unique<rpc::GrpcClient>(endpoint, options));
    }
}

NodeRuntime::~NodeRuntime() { stop(); }

void NodeRuntime::start() {
    if (running_.exchange(true)) {
        throw std::logic_error{"node runtime is already running"};
    }

    try {
        server_->start();
        for (std::size_t worker = 0; worker < config_.peers.size(); ++worker) {
            workers_.emplace_back([this] { worker_loop(); });
        }
        ticker_ = std::thread{[this] { ticker_loop(); }};
        const auto snapshot = raft_node_->snapshot();
        const std::scoped_lock lock{log_mutex};
        std::cout << "node_id=" << config_.node_id
                  << " event=started state=" << state_name(snapshot.state)
                  << " term=" << snapshot.current_term
                  << " last_log_index=" << snapshot.last_log_index << '\n';
    } catch (...) {
        running_ = false;
        queue_ready_.notify_all();
        if (ticker_.joinable()) {
            ticker_.join();
        }
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers_.clear();
        server_->shutdown();
        throw;
    }
}

void NodeRuntime::stop() noexcept {
    if (!running_.exchange(false)) {
        return;
    }
    {
        const std::scoped_lock lock{queue_mutex_};
        message_queue_.clear();
    }
    queue_ready_.notify_all();
    if (ticker_.joinable()) {
        ticker_.join();
    }
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
    server_->shutdown();
}

void NodeRuntime::ticker_loop() {
    auto previous = std::chrono::steady_clock::now();
    auto last_snapshot = raft_node_->snapshot();
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        if (!running_) {
            break;
        }
        const auto now = std::chrono::steady_clock::now();
        raft_node_->tick(std::chrono::duration_cast<std::chrono::milliseconds>(now - previous));
        previous = now;
        enqueue_messages(raft_node_->take_messages());

        const auto snapshot = raft_node_->snapshot();
        if (snapshot.state != last_snapshot.state ||
            snapshot.current_term != last_snapshot.current_term ||
            snapshot.leader_id != last_snapshot.leader_id) {
            const std::scoped_lock lock{log_mutex};
            std::cout << "node_id=" << config_.node_id
                      << " event=state_changed state=" << state_name(snapshot.state)
                      << " term=" << snapshot.current_term
                      << " leader_id=" << snapshot.leader_id.value_or(0) << '\n';
            last_snapshot = snapshot;
        }
    }
}

void NodeRuntime::worker_loop() {
    while (true) {
        raft::Message message;
        {
            std::unique_lock lock{queue_mutex_};
            queue_ready_.wait(lock, [this] { return !running_ || !message_queue_.empty(); });
            if (!running_ && message_queue_.empty()) {
                return;
            }
            message = std::move(message_queue_.front());
            message_queue_.pop_front();
        }
        send_message(message);
    }
}

void NodeRuntime::enqueue_messages(std::vector<raft::Message> messages) {
    if (messages.empty()) {
        return;
    }
    {
        const std::scoped_lock lock{queue_mutex_};
        for (auto& message : messages) {
            message_queue_.push_back(std::move(message));
        }
    }
    queue_ready_.notify_all();
}

void NodeRuntime::send_message(const raft::Message& message) {
    const auto client = peer_clients_.find(message.to);
    if (client == peer_clients_.end()) {
        return;
    }

    try {
        if (const auto* request = std::get_if<raft::RequestVoteRequest>(&message.rpc)) {
            const auto response = client->second->request_vote(config_.node_id, *request);
            static_cast<void>(
                raft_node_->step(raft::Message{message.to, config_.node_id, response}));
        } else if (const auto* request = std::get_if<raft::AppendEntriesRequest>(&message.rpc)) {
            const auto response = client->second->append_entries(config_.node_id, *request);
            static_cast<void>(
                raft_node_->step(raft::Message{message.to, config_.node_id, response}));
        }
    } catch (const rpc::GrpcError&) {
        // Raft retries elections and replication on later ticks.
    }
}

std::string_view state_name(const raft::NodeState state) noexcept {
    switch (state) {
    case raft::NodeState::follower:
        return "follower";
    case raft::NodeState::candidate:
        return "candidate";
    case raft::NodeState::leader:
        return "leader";
    }
    return "unknown";
}

} // namespace ftkv::node
