#include "grpc/grpc_server.h"

#include "kvstore.grpc.pb.h"

#include <functional>
#include <optional>
#include <stdexcept>
#include <utility>

namespace ftkv::rpc {
namespace {

template <typename Operation>
[[nodiscard]] grpc::Status run_handler(grpc::ServerContext* context, Operation&& operation) {
    if (context->IsCancelled()) {
        return {grpc::StatusCode::CANCELLED, "request was cancelled"};
    }

    try {
        std::invoke(std::forward<Operation>(operation));
        if (context->IsCancelled()) {
            return {grpc::StatusCode::CANCELLED, "request deadline elapsed"};
        }
        return grpc::Status::OK;
    } catch (const std::invalid_argument& error) {
        return {grpc::StatusCode::INVALID_ARGUMENT, error.what()};
    } catch (const raft::RaftError& error) {
        return {grpc::StatusCode::FAILED_PRECONDITION, error.what()};
    } catch (const std::exception& error) {
        return {grpc::StatusCode::INTERNAL, error.what()};
    } catch (...) {
        return {grpc::StatusCode::INTERNAL, "unhandled server error"};
    }
}

}  // namespace

class GrpcServer::KeyValueService final : public ::ftkv::v1::KeyValueStore::Service {
public:
    explicit KeyValueService(std::shared_ptr<store::KeyValueStore> key_value_store)
        : key_value_store_{std::move(key_value_store)} {
        if (!key_value_store_) {
            throw std::invalid_argument{"key-value store must not be null"};
        }
    }

    grpc::Status Get(grpc::ServerContext* context, const ::ftkv::v1::GetRequest* request,
                     ::ftkv::v1::GetResponse* response) override {
        return run_handler(context, [&] {
            const auto value = key_value_store_->get(request->key());
            response->set_found(value.has_value());
            if (value.has_value()) {
                response->set_value(*value);
            }
        });
    }

    grpc::Status Put(grpc::ServerContext* context, const ::ftkv::v1::PutRequest* request,
                     ::ftkv::v1::PutResponse*) override {
        return run_handler(context,
                           [&] { key_value_store_->put(request->key(), request->value()); });
    }

    grpc::Status Delete(grpc::ServerContext* context, const ::ftkv::v1::DeleteRequest* request,
                        ::ftkv::v1::DeleteResponse* response) override {
        return run_handler(
            context, [&] { response->set_deleted(key_value_store_->erase(request->key())); });
    }

private:
    std::shared_ptr<store::KeyValueStore> key_value_store_;
};

class GrpcServer::RaftService final : public ::ftkv::v1::Raft::Service {
public:
    explicit RaftService(std::shared_ptr<raft::RaftNode> raft_node)
        : raft_node_{std::move(raft_node)} {
        if (!raft_node_) {
            throw std::invalid_argument{"Raft node must not be null"};
        }
    }

    grpc::Status RequestVote(grpc::ServerContext* context,
                             const ::ftkv::v1::RequestVoteRequest* request,
                             ::ftkv::v1::RequestVoteResponse* response) override {
        return run_handler(context, [&] {
            const raft::RequestVoteRequest vote_request{request->term(), request->candidate_id(),
                                                        request->last_log_index(),
                                                        request->last_log_term()};
            const auto message = raft_node_->handle_request(
                raft::Message{request->candidate_id(), raft_node_->id(), vote_request});
            if (!message.has_value()) {
                throw std::invalid_argument{"RequestVote sender is not a cluster member"};
            }

            const auto* vote_response = std::get_if<raft::RequestVoteResponse>(&message->rpc);
            if (vote_response == nullptr) {
                throw std::logic_error{"Raft node did not produce a RequestVote response"};
            }
            response->set_term(vote_response->term);
            response->set_vote_granted(vote_response->vote_granted);
        });
    }

    grpc::Status AppendEntries(grpc::ServerContext* context,
                               const ::ftkv::v1::AppendEntriesRequest* request,
                               ::ftkv::v1::AppendEntriesResponse* response) override {
        return run_handler(context, [&] {
            raft::AppendEntriesRequest append_request;
            append_request.term = request->term();
            append_request.leader_id = request->leader_id();
            append_request.previous_log_index = request->previous_log_index();
            append_request.previous_log_term = request->previous_log_term();
            append_request.leader_commit = request->leader_commit();
            append_request.request_id = request->request_id();
            append_request.entries.reserve(static_cast<std::size_t>(request->entries_size()));
            for (const auto& entry : request->entries()) {
                append_request.entries.push_back(
                    raft::LogEntry{entry.term(), entry.command(), entry.is_no_op()});
            }

            const auto message = raft_node_->handle_request(
                raft::Message{request->leader_id(), raft_node_->id(), std::move(append_request)});
            if (!message.has_value()) {
                throw std::invalid_argument{"AppendEntries sender is not a cluster member"};
            }

            const auto* append_response =
                std::get_if<raft::AppendEntriesResponse>(&message->rpc);
            if (append_response == nullptr) {
                throw std::logic_error{"Raft node did not produce an AppendEntries response"};
            }
            response->set_term(append_response->term);
            response->set_success(append_response->success);
            response->set_match_index(append_response->match_index);
            response->set_conflict_index(append_response->conflict_index);
            response->set_has_conflict_term(append_response->conflict_term.has_value());
            if (append_response->conflict_term.has_value()) {
                response->set_conflict_term(*append_response->conflict_term);
            }
            response->set_request_id(append_response->request_id);
        });
    }

private:
    std::shared_ptr<raft::RaftNode> raft_node_;
};

GrpcServer::GrpcServer(std::string listen_address,
                       std::shared_ptr<store::KeyValueStore> key_value_store,
                       std::shared_ptr<raft::RaftNode> raft_node)
    : listen_address_{std::move(listen_address)},
      key_value_service_{std::make_unique<KeyValueService>(std::move(key_value_store))},
      raft_service_{std::make_unique<RaftService>(std::move(raft_node))} {
    if (listen_address_.empty()) {
        throw std::invalid_argument{"gRPC listen address must not be empty"};
    }
}

GrpcServer::~GrpcServer() { shutdown(); }

void GrpcServer::start() {
    const std::scoped_lock lock{mutex_};
    if (server_) {
        throw std::logic_error{"gRPC server is already running"};
    }

    grpc::ServerBuilder builder;
    builder.AddListeningPort(listen_address_, grpc::InsecureServerCredentials(), &bound_port_);
    builder.RegisterService(key_value_service_.get());
    builder.RegisterService(raft_service_.get());
    server_ = builder.BuildAndStart();
    if (!server_) {
        bound_port_ = 0;
        throw std::runtime_error{"failed to start gRPC server on " + listen_address_};
    }
}

void GrpcServer::shutdown() noexcept {
    std::unique_ptr<grpc::Server> server;
    {
        const std::scoped_lock lock{mutex_};
        server = std::move(server_);
    }
    if (server) {
        server->Shutdown();
        server->Wait();
    }
}

int GrpcServer::bound_port() const {
    const std::scoped_lock lock{mutex_};
    return bound_port_;
}

std::string GrpcServer::endpoint() const {
    const std::scoped_lock lock{mutex_};
    if (bound_port_ == 0) {
        throw std::logic_error{"gRPC server has not been started"};
    }
    const auto separator = listen_address_.rfind(':');
    if (separator == std::string::npos) {
        return listen_address_;
    }
    return listen_address_.substr(0, separator + 1U) + std::to_string(bound_port_);
}

}  // namespace ftkv::rpc
