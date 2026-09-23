#include "grpc/grpc_client.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <thread>
#include <utility>

namespace ftkv::rpc {
namespace {

struct InvocationResult {
    grpc::Status status;
    std::size_t attempts{0};
};

[[nodiscard]] bool is_retryable(const grpc::StatusCode code) {
    return code == grpc::StatusCode::UNAVAILABLE || code == grpc::StatusCode::DEADLINE_EXCEEDED ||
           code == grpc::StatusCode::RESOURCE_EXHAUSTED || code == grpc::StatusCode::ABORTED;
}

void validate_options(const GrpcClientOptions& options) {
    if (options.timeout.count() <= 0 || options.max_attempts == 0 ||
        options.initial_backoff.count() < 0 || options.maximum_backoff.count() < 0 ||
        options.initial_backoff > options.maximum_backoff) {
        throw std::invalid_argument{"invalid gRPC client options"};
    }
}

[[nodiscard]] std::shared_ptr<grpc::Channel> make_channel(std::string target) {
    if (target.empty()) {
        throw std::invalid_argument{"gRPC target must not be empty"};
    }
    return grpc::CreateChannel(std::move(target), grpc::InsecureChannelCredentials());
}

template <typename Operation>
[[nodiscard]] InvocationResult invoke_with_retry(const GrpcClientOptions& options,
                                                 Operation&& operation) {
    auto backoff = options.initial_backoff;
    for (std::size_t attempt = 1; attempt <= options.max_attempts; ++attempt) {
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + options.timeout);
        auto status = std::invoke(operation, context);
        if (status.ok() || !is_retryable(status.error_code()) || attempt == options.max_attempts) {
            return InvocationResult{std::move(status), attempt};
        }

        std::this_thread::sleep_for(backoff);
        backoff = std::min(options.maximum_backoff, backoff * 2);
    }
    throw std::logic_error{"gRPC retry loop terminated unexpectedly"};
}

[[nodiscard]] std::string error_text(const std::string& operation, const grpc::Status& status,
                                     const std::size_t attempts) {
    auto message = operation + " failed after " + std::to_string(attempts) + " attempt(s)";
    if (!status.error_message().empty()) {
        message += ": " + status.error_message();
    }
    return message;
}

} // namespace

GrpcError::GrpcError(std::string operation, const grpc::Status& status, const std::size_t attempts)
    : std::runtime_error{error_text(operation, status, attempts)},
      status_code_{status.error_code()}, attempts_{attempts} {}

grpc::StatusCode GrpcError::status_code() const noexcept { return status_code_; }

std::size_t GrpcError::attempts() const noexcept { return attempts_; }

GrpcClient::GrpcClient(std::string target, GrpcClientOptions options)
    : GrpcClient{make_channel(std::move(target)), options} {}

GrpcClient::GrpcClient(std::shared_ptr<grpc::Channel> channel, GrpcClientOptions options)
    : options_{options} {
    validate_options(options_);
    if (!channel) {
        throw std::invalid_argument{"gRPC channel must not be null"};
    }
    key_value_stub_ = ::ftkv::v1::KeyValueStore::NewStub(channel);
    raft_stub_ = ::ftkv::v1::Raft::NewStub(std::move(channel));
}

std::optional<std::string> GrpcClient::get(const std::string_view key) const {
    ::ftkv::v1::GetRequest request;
    request.set_key(std::string{key});
    ::ftkv::v1::GetResponse response;
    const auto result = invoke_with_retry(options_, [&](grpc::ClientContext& context) {
        response.Clear();
        return key_value_stub_->Get(&context, request, &response);
    });
    if (!result.status.ok()) {
        throw GrpcError{"Get", result.status, result.attempts};
    }
    if (!response.found()) {
        return std::nullopt;
    }
    return response.value();
}

void GrpcClient::put(std::string key, std::string value) const {
    ::ftkv::v1::PutRequest request;
    request.set_key(std::move(key));
    request.set_value(std::move(value));
    ::ftkv::v1::PutResponse response;
    const auto result = invoke_with_retry(options_, [&](grpc::ClientContext& context) {
        response.Clear();
        return key_value_stub_->Put(&context, request, &response);
    });
    if (!result.status.ok()) {
        throw GrpcError{"Put", result.status, result.attempts};
    }
}

bool GrpcClient::erase(const std::string_view key) const {
    ::ftkv::v1::DeleteRequest request;
    request.set_key(std::string{key});
    ::ftkv::v1::DeleteResponse response;
    const auto result = invoke_with_retry(options_, [&](grpc::ClientContext& context) {
        response.Clear();
        return key_value_stub_->Delete(&context, request, &response);
    });
    if (!result.status.ok()) {
        throw GrpcError{"Delete", result.status, result.attempts};
    }
    return response.deleted();
}

NodeStatus GrpcClient::status() const {
    ::ftkv::v1::NodeStatusRequest request;
    ::ftkv::v1::NodeStatusResponse response;
    const auto result = invoke_with_retry(options_, [&](grpc::ClientContext& context) {
        response.Clear();
        return raft_stub_->GetStatus(&context, request, &response);
    });
    if (!result.status.ok()) {
        throw GrpcError{"GetStatus", result.status, result.attempts};
    }

    raft::NodeState state;
    switch (response.role()) {
    case ::ftkv::v1::NODE_ROLE_FOLLOWER:
        state = raft::NodeState::follower;
        break;
    case ::ftkv::v1::NODE_ROLE_CANDIDATE:
        state = raft::NodeState::candidate;
        break;
    case ::ftkv::v1::NODE_ROLE_LEADER:
        state = raft::NodeState::leader;
        break;
    default:
        throw GrpcError{
            "GetStatus",
            grpc::Status{grpc::StatusCode::DATA_LOSS, "server returned an unknown Raft role"},
            result.attempts};
    }

    std::optional<raft::NodeId> leader_id;
    if (response.has_leader()) {
        leader_id = response.leader_id();
    }
    return NodeStatus{response.node_id(),       state,
                      response.current_term(),  leader_id,
                      response.commit_index(),  response.last_applied(),
                      response.last_log_index()};
}

raft::RequestVoteResponse GrpcClient::request_vote(const raft::NodeId sender,
                                                   const raft::RequestVoteRequest& request) const {
    if (request.candidate_id != sender) {
        throw std::invalid_argument{"RequestVote sender must match candidate ID"};
    }
    ::ftkv::v1::RequestVoteRequest wire_request;
    wire_request.set_term(request.term);
    wire_request.set_candidate_id(request.candidate_id);
    wire_request.set_last_log_index(request.last_log_index);
    wire_request.set_last_log_term(request.last_log_term);
    ::ftkv::v1::RequestVoteResponse wire_response;

    const auto result = invoke_with_retry(options_, [&](grpc::ClientContext& context) {
        wire_response.Clear();
        return raft_stub_->RequestVote(&context, wire_request, &wire_response);
    });
    if (!result.status.ok()) {
        throw GrpcError{"RequestVote", result.status, result.attempts};
    }
    return raft::RequestVoteResponse{wire_response.term(), wire_response.vote_granted()};
}

raft::AppendEntriesResponse
GrpcClient::append_entries(const raft::NodeId sender,
                           const raft::AppendEntriesRequest& request) const {
    if (request.leader_id != sender) {
        throw std::invalid_argument{"AppendEntries sender must match leader ID"};
    }
    ::ftkv::v1::AppendEntriesRequest wire_request;
    wire_request.set_term(request.term);
    wire_request.set_leader_id(request.leader_id);
    wire_request.set_previous_log_index(request.previous_log_index);
    wire_request.set_previous_log_term(request.previous_log_term);
    wire_request.set_leader_commit(request.leader_commit);
    wire_request.set_request_id(request.request_id);
    for (const auto& entry : request.entries) {
        auto* wire_entry = wire_request.add_entries();
        wire_entry->set_term(entry.term);
        wire_entry->set_command(entry.command);
        wire_entry->set_is_no_op(entry.is_no_op);
    }
    ::ftkv::v1::AppendEntriesResponse wire_response;

    const auto result = invoke_with_retry(options_, [&](grpc::ClientContext& context) {
        wire_response.Clear();
        return raft_stub_->AppendEntries(&context, wire_request, &wire_response);
    });
    if (!result.status.ok()) {
        throw GrpcError{"AppendEntries", result.status, result.attempts};
    }

    std::optional<raft::Term> conflict_term;
    if (wire_response.has_conflict_term()) {
        conflict_term = wire_response.conflict_term();
    }
    return raft::AppendEntriesResponse{
        wire_response.term(),           wire_response.success(), wire_response.match_index(),
        wire_response.conflict_index(), conflict_term,           wire_response.request_id()};
}

} // namespace ftkv::rpc
