# Fault-Tolerant Distributed Key-Value Store

A modern C++20 foundation for a distributed key-value store designed to remain available and
consistent in the presence of node and network failures.

> **Project status:** Early foundation. The repository currently provides thread-safe in-memory and
> file-backed storage engines, a server entry point, an initial protocol contract, tests, and build
> tooling. Replication, consensus, and networking are planned work.

## Goals

- Provide a simple key-value API with predictable semantics.
- Replicate data across nodes and tolerate individual node failures.
- Keep the storage, consensus, transport, and application layers independently testable.
- Offer reproducible local and containerized builds.

## Requirements

- A C++20 compiler (Clang 12+, GCC 10+, or MSVC 19.29+)
- CMake 3.20 or newer
- Docker (optional)

No third-party libraries are required for the initial build.

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Run the placeholder server executable:

```bash
./build/ftkv_server
```

Build the container image:

```bash
docker build -f docker/Dockerfile -t ftkv-store .
docker run --rm ftkv-store
```

## Repository layout

```text
include/   Public C++ interfaces
src/       Library implementations and executable entry points
proto/     Versioned wire-protocol definitions
tests/     Unit and integration tests
docs/      Architecture and contributor documentation
docker/    Container build definitions
```

The intended component boundaries and future request flow are documented in
[docs/architecture.md](docs/architecture.md). Development commands and conventions are in
[docs/development.md](docs/development.md).

## Roadmap

- Define client/server error semantics and generate RPC bindings.
- Replace full-snapshot persistence with a durable write-ahead log and compaction.
- Implement leader election and replicated-log consensus.
- Add membership, failure detection, observability, and fault-injection tests.
- Publish compatibility and operational guidance for the first stable release.

## Contributing

Contributions are welcome while the project is taking shape. Before submitting a change, format
the affected C++ files, build with warnings enabled, and run the full test suite. See
[docs/development.md](docs/development.md) for the expected workflow.

## License

No license has been selected yet. Until one is added, all rights are reserved by the copyright
holders.
