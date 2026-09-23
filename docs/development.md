# Development guide

## Configure, build, and test

Use an out-of-source build so generated files do not pollute the repository:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

If Protobuf and gRPC are installed under a custom prefix, add
`-DCMAKE_PREFIX_PATH=/path/to/grpc/prefix` to the configure command.

For a release build, replace `Debug` with `Release`. Strict compiler warnings are enabled by
default and can be disabled only for exceptional toolchain compatibility with
`-DFTKV_ENABLE_WARNINGS=OFF`.

## Formatting

The root `.clang-format` is authoritative. Format changed C++ files before committing:

```bash
clang-format -i include/ftkv/store/*.hpp src/*.cpp src/store/*.cpp tests/*.cpp
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
