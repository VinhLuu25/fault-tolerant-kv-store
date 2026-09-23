# Fault-Tolerant Distributed Key-Value Store

A modern C++20 foundation for a distributed key-value store designed to remain available and
consistent in the presence of node and network failures.

> **Project status:** Early foundation. The repository provides thread-safe in-memory and
> file-backed storage engines, a Raft consensus core, gRPC communication, a durable node runtime,
> tests, and a local three-node Docker deployment.

## Goals

- Provide a simple key-value API with predictable semantics.
- Replicate data across nodes and tolerate individual node failures.
- Keep the storage, consensus, transport, and application layers independently testable.
- Offer reproducible local and containerized builds.

## Requirements

- A C++20 compiler (Clang 12+, GCC 10+, or MSVC 19.29+)
- CMake 3.20 or newer
- Protobuf and gRPC C++ development packages
- Docker (optional)

Set `CMAKE_PREFIX_PATH` when gRPC is installed under a non-system prefix.

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH=/path/to/grpc/prefix
cmake --build build
ctest --test-dir build --output-on-failure
```

The server reads its node identity, membership, timing, and data directory from environment
variables. The ready-to-run configuration is provided by Docker Compose.

## Run a three-node cluster

```bash
docker compose up --build --detach --wait
docker compose ps
```

The default host endpoints are `127.0.0.1:50051`, `127.0.0.1:50052`, and
`127.0.0.1:50053`. Node 1 has the shortest election window and is the preferred initial leader;
nodes 2 and 3 remain followers after the election. Confirm the live roles with:

```bash
docker compose exec node1 ftkv_server --status 127.0.0.1:50051
docker compose exec node2 ftkv_server --status 127.0.0.1:50051
docker compose exec node3 ftkv_server --status 127.0.0.1:50051
```

Each node stores Raft and key-value snapshots in its own named volume. Run
`./docker/test_cluster.sh` to verify election, follower roles, restart node 3, and confirm recovery
of its persisted Raft progress. See [docs/deployment.md](docs/deployment.md) for configuration,
ports, volume lifecycle, and operations.

## Repository layout

```text
include/   Public C++ interfaces
src/       Library implementations and executable entry points
proto/     Versioned wire-protocol definitions
tests/     Unit and integration tests
docs/      Architecture and contributor documentation
docker/    Container integration and recovery checks
```

The component boundaries and request flow are documented in
[docs/architecture.md](docs/architecture.md). Development commands and conventions are in
[docs/development.md](docs/development.md).

## Roadmap

- Replace full-snapshot persistence with a durable write-ahead log and compaction.
- Connect committed Raft commands to the key-value state machine.
- Add TLS credentials, peer authentication, and production orchestration.
- Add membership, failure detection, observability, and fault-injection tests.
- Publish compatibility and operational guidance for the first stable release.

## Contributing

Contributions are welcome while the project is taking shape. Before submitting a change, format
the affected C++ files, build with warnings enabled, and run the full test suite. See
[docs/development.md](docs/development.md) for the expected workflow.

## License

No license has been selected yet. Until one is added, all rights are reserved by the copyright
holders.
