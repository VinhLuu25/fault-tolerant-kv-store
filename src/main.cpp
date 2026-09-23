#include "grpc/grpc_client.h"
#include "node/node_runtime.h"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

volatile std::sig_atomic_t stop_requested = 0;

void request_stop(int) { stop_requested = 1; }

[[nodiscard]] ftkv::rpc::GrpcClient make_probe_client(std::string target) {
    ftkv::rpc::GrpcClientOptions options;
    options.timeout = std::chrono::milliseconds{500};
    options.max_attempts = 2;
    options.initial_backoff = std::chrono::milliseconds{25};
    options.maximum_backoff = std::chrono::milliseconds{50};
    return ftkv::rpc::GrpcClient{std::move(target), options};
}

void print_status(const ftkv::raft::NodeSnapshot& status) {
    std::cout << "node_id=" << status.node_id << " state=" << ftkv::node::state_name(status.state)
              << " term=" << status.current_term << " leader_id="
              << status.leader_id.value_or(0) << " commit_index=" << status.commit_index
              << " last_applied=" << status.last_applied
              << " last_log_index=" << status.last_log_index << '\n';
}

[[nodiscard]] int run_probe(const std::string_view mode, std::string target) {
    const auto status = make_probe_client(std::move(target)).status();
    if (mode == "--status") {
        print_status(status);
    }
    return EXIT_SUCCESS;
}

[[nodiscard]] int run_node() {
    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);

    ftkv::node::NodeRuntime runtime{ftkv::node::NodeRuntimeConfig::from_environment()};
    runtime.start();
    while (stop_requested == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    runtime.stop();
    return EXIT_SUCCESS;
}

}  // namespace

int main(const int argc, char* argv[]) {
    try {
        if (argc == 1) {
            return run_node();
        }
        if (argc == 3 &&
            (std::string_view{argv[1]} == "--healthcheck" ||
             std::string_view{argv[1]} == "--status")) {
            return run_probe(argv[1], argv[2]);
        }

        std::cerr << "usage: ftkv_server [--healthcheck TARGET | --status TARGET]\n";
        return EXIT_FAILURE;
    } catch (const std::exception& error) {
        std::cerr << "ftkv_server: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
