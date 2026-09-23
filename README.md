# Fault-Tolerant Distributed Key-Value Store

A C++20 distributed-systems project that combines a persistent key-value engine, a Raft consensus
core, gRPC communication, and a reproducible three-node Docker deployment.

> **Engineering status:** This repository is a production-oriented prototype. Leader election,
> heartbeats, quorum commit, log repair, durable Raft state, RPC deadlines/retries, and automated
> failure-recovery tests are implemented. Client key-value mutations are currently node-local;
> applying committed Raft commands to the key-value state machine is the next major milestone.

# Project Overview

The project explores the engineering problems behind a fault-tolerant service: concurrent access,
durable state, leader election, replicated logs, unreliable peers, bounded network calls, and
repeatable failure testing. Its layers are deliberately separated so the storage engine, consensus
algorithm, transport, process runtime, and deployment topology can evolve independently.

The repository demonstrates:

- modern C++ ownership, RAII, concurrency, and interface design;
- a Raft state machine based on the extended Raft paper;
- gRPC and Protocol Buffers for client and peer communication;
- containerized multi-node operation with persisted state; and
- unit and end-to-end tests that exercise failure and recovery paths.

# Features

- Thread-safe `GET`, `PUT`, and `DELETE` operations.
- In-memory and atomic file-backed storage implementations.
- Follower, candidate, and leader Raft roles.
- Randomized election timeouts and majority-vote leader election.
- Heartbeats, log replication, conflict repair, and majority quorum commit.
- Durable Raft term, vote, and log state across process restarts.
- gRPC APIs for key-value requests and internal Raft messages.
- Per-request deadlines, bounded retries, exponential backoff, and status-code mapping.
- Graceful process shutdown and health/status probes.
- Three-node Docker Compose topology with isolated volumes and configurable ports.
- CTest unit and Docker integration suites, including leader failure and node recovery.

# Architecture

The target client-to-consensus flow is shown below. The active Raft leader is a dynamic role held
by one of the three nodes, not a separate service. Today, clients can also contact any exposed node
directly for node-local CRUD.

```text
Client
   |
Leader
   |
-----------------------------
|             |             |
Node1         Node2         Node3
```

| Layer | Responsibility | Main location |
| --- | --- | --- |
| API and transport | Client CRUD RPCs, Raft RPCs, deadlines, retries | `proto/`, `src/grpc/` |
| Consensus | Elections, heartbeats, replicated log, quorum commit | `src/raft/` |
| Node runtime | Configuration, timers, peer workers, lifecycle | `src/node/` |
| Storage | Thread-safe CRUD and persistence abstraction | `src/storage/`, `src/store/` |
| Deployment | Image build, networking, health checks, volumes | `Dockerfile`, `docker-compose.yml` |

Two flows are currently implemented:

```text
Client CRUD -> gRPC adapter -> local key-value store -> atomic snapshot

Raft peer RPC -> gRPC adapter -> Raft state machine -> durable Raft snapshot
```

The next architecture step is to encode client mutations as Raft commands and apply them to every
node only after quorum commit. See [docs/architecture.md](docs/architecture.md) for component
boundaries and current failure assumptions.

# Tech Stack

| Technology | Usage |
| --- | --- |
| C++20 | Core implementation, concurrency, RAII, filesystem persistence |
| CMake 3.20+ | Builds, generated protocol code, installation, test registration |
| Protocol Buffers | Versioned client and peer message contracts |
| gRPC C++ | Client API and node-to-node transport |
| Raft | Leader election and replicated-log consensus |
| Docker / Docker Compose | Reproducible image and local three-node cluster |
| CTest | Unit and container-based integration test orchestration |

# Installation

Clone the repository:

```bash
git clone https://github.com/VinhLuu25/fault-tolerant-kv-store.git
cd fault-tolerant-kv-store
```

For a native Ubuntu 24.04 build, install the toolchain and protocol dependencies:

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake \
  libgrpc++-dev libprotobuf-dev \
  protobuf-compiler protobuf-compiler-grpc
```

Alternatively, install Docker with the Compose plugin and use the container workflow below. If
gRPC is installed under a custom prefix, pass that prefix through `CMAKE_PREFIX_PATH` when
configuring the native build.

# Build

Use an out-of-source build directory:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
```

For a non-system gRPC installation:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH=/path/to/grpc/prefix
```

Create a release build by replacing `Debug` with `Release`. Strict compiler warnings are enabled
by default.

# Docker Deployment

Start the production-like local cluster:

```bash
docker compose up --build --detach --wait
docker compose ps
```

| Service | Default role | Host endpoint | Volume |
| --- | --- | --- | --- |
| `node1` | Preferred initial leader | `127.0.0.1:50051` | `node1-data` |
| `node2` | Follower | `127.0.0.1:50052` | `node2-data` |
| `node3` | Follower | `127.0.0.1:50053` | `node3-data` |

Node 1 has the shortest election window, making startup predictable while still requiring a Raft
majority vote. Copy `.env.example` to `.env` to override image name, ports, or election timing.

Stop containers while preserving data:

```bash
docker compose down
```

The full deployment, recovery, persistence, and troubleshooting runbook is in
[docs/deployment.md](docs/deployment.md).

# Testing

The default suite runs native unit tests and, when Docker is available, isolated container
integration tests:

```bash
ctest --test-dir build --output-on-failure
```

| Suite | Coverage |
| --- | --- |
| Storage unit tests | CRUD, binary data, persistence, corruption, rollback, concurrency |
| Raft unit tests | Election, heartbeat, replication, quorum commit, failover, log repair |
| gRPC unit tests | CRUD requests, deadlines, retries, peer messages, node status |
| Integration tests | Cluster startup, leader election, leader failure, container recovery |

Run suites independently:

```bash
ctest --test-dir build --output-on-failure --label-regex unit
ctest --test-dir build --output-on-failure --label-regex integration
```

The integration fixture uses ephemeral host ports and a unique Compose project, then removes its
containers, image, network, and volumes. Configure with `-DFTKV_ENABLE_DOCKER_TESTS=OFF` for a
native-only build.

# Example Usage

Inspect the elected leader and replicated-log progress with the built-in status command:

```bash
docker compose exec node1 ftkv_server --status 127.0.0.1:50051
```

Example output:

```text
node_id=1 state=leader term=1 leader_id=1 commit_index=1 last_applied=1 last_log_index=1
```

With `grpcurl` installed, call the key-value API directly. Protocol Buffer `bytes` fields use
base64 in JSON; the examples below store key `hello` with value `world`.

```bash
grpcurl -plaintext -import-path proto -proto kvstore.proto \
  -d '{"key":"aGVsbG8=","value":"d29ybGQ="}' \
  127.0.0.1:50051 ftkv.v1.KeyValueStore/Put

grpcurl -plaintext -import-path proto -proto kvstore.proto \
  -d '{"key":"aGVsbG8="}' \
  127.0.0.1:50051 ftkv.v1.KeyValueStore/Get

grpcurl -plaintext -import-path proto -proto kvstore.proto \
  -d '{"key":"aGVsbG8="}' \
  127.0.0.1:50051 ftkv.v1.KeyValueStore/Delete
```

These CRUD calls currently target the selected node's local persistent store; they do not yet
provide replicated or linearizable client semantics.

# Future Improvements

- Route client mutations through the elected leader and apply only committed Raft entries.
- Add linearizable reads and explicit follower redirection.
- Replace full snapshots with a write-ahead log, compaction, and stronger crash durability.
- Implement Raft snapshots, log installation, and safe membership changes.
- Add TLS, mutual peer authentication, authorization, and secret management.
- Export structured logs, metrics, traces, and operational health signals.
- Add network-partition, latency, disk-failure, and long-running Jepsen-style tests.
- Provide Kubernetes manifests and production service discovery.

Additional documentation:

- [Architecture](docs/architecture.md)
- [Deployment runbook](docs/deployment.md)
- [Development guide](docs/development.md)

No license has been selected yet. Until one is added, all rights are reserved by the copyright
holders.
