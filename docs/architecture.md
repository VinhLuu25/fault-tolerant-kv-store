# Architecture

## Design principles

The system is organized around narrow interfaces so that storage, consensus, transport, and
process lifecycle code can evolve and be tested independently. Dependencies should point inward:
protocol adapters and executables may depend on core interfaces, while the core must not depend on
a particular RPC framework or deployment environment.

## Planned components

1. **API/transport** validates client requests and translates protocol messages into commands.
2. **Consensus** orders mutating commands across a quorum of nodes.
3. **Storage** applies committed commands and serves reads according to the chosen consistency
   level.
4. **Persistence** records the replicated log, snapshots, and durable key-value state.
5. **Node runtime** owns configuration, membership, background work, and graceful shutdown.

The current `KeyValueStore` interface and `InMemoryKeyValueStore` implementation establish the
storage boundary. The in-memory implementation is thread-safe but intentionally non-durable; it is
not yet a distributed system.

## Expected request flow

```text
client -> RPC adapter -> node service -> consensus -> storage
                                      -> persistence
```

Read behavior, conflict handling, failure assumptions, and consensus protocol details will be
captured as architecture decision records before their implementations are introduced.

## Source boundaries

- Public interfaces belong under `include/ftkv/`.
- Implementations belong under `src/` and include public headers by their installed paths.
- Wire contracts belong under `proto/` and are versioned by package name.
- Tests mirror production boundaries under `tests/` and should avoid private implementation access.
