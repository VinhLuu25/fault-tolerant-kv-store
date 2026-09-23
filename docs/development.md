# Development guide

## Configure, build, and test

Use an out-of-source build so generated files do not pollute the repository:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The default CTest suite includes unit tests for storage, Raft, and gRPC plus Docker integration
tests for cluster startup, leader election, and node recovery. Docker tests use an isolated Compose
project, ephemeral host ports, and test-only volumes that are removed by a CTest cleanup fixture.
Run a subset by label when iterating:

```bash
ctest --test-dir build --output-on-failure --label-regex unit
ctest --test-dir build --output-on-failure --label-regex integration
```

Configure with `-DFTKV_ENABLE_DOCKER_TESTS=OFF` when only native unit tests are required. If Docker
or the Compose plugin is installed but unavailable, registered integration tests report as
skipped.

If Protobuf and gRPC are installed under a custom prefix, add
`-DCMAKE_PREFIX_PATH=/path/to/grpc/prefix` to the configure command.

For a release build, replace `Debug` with `Release`. Strict compiler warnings are enabled by
default and can be disabled only for exceptional toolchain compatibility with
`-DFTKV_ENABLE_WARNINGS=OFF`.

## Formatting

The root `.clang-format` is authoritative. Format changed C++ files before committing:

```bash
clang-format -i include/ftkv/store/*.hpp src/*.cpp src/*/*.cpp src/*/*.h tests/*.cpp
```

## Change guidelines

- Keep public interfaces small and independent of a specific networking or persistence library.
- Add tests for behavior changes and regressions.
- Do not commit generated build artifacts or generated protocol sources.
- Update architecture documentation when component boundaries or failure assumptions change.
- Prefer focused commits whose messages explain the intent of the change.

## Installing locally

```bash
cmake --install build --prefix ./build/install
```

This installs the core archive, server executable, and public headers beneath the selected prefix.
