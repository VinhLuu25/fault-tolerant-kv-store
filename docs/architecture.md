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

The `KeyValueStore` interface establishes the storage boundary. `InMemoryKeyValueStore` remains a
lightweight non-durable implementation. The internal `KvStore` adds thread-safe, file-backed
persistence through an injectable `Persistence` interface. Mutations are written as versioned
snapshots before becoming visible in memory, which preserves the last committed state if a save
fails. Snapshot replacement is atomic on supported local filesystems, but the format does not yet
provide a write-ahead log, cross-process locking, or `fsync`-level crash guarantees.

The Raft core under `src/raft/` implements fixed-membership leader election, randomized election
timeouts, heartbeats, replicated-log conflict repair, and current-term majority commits. It is an
event-driven state machine: the node runtime advances time with `tick()`, delivers inbound RPCs
with `step()`, and sends messages returned by `take_messages()`. This keeps consensus independent
of a particular clock or networking library. Persistent term, vote, and log state are accessed
through `RaftStorage`; a production runtime must provide a durable implementation before exposing
the node to clients.

The gRPC layer under `src/grpc/` exposes separate public key-value and internal Raft services from
the versioned `proto/kvstore.proto` contract. Client calls always carry deadlines and retry only
transient status codes with bounded exponential backoff. The server maps storage and consensus
failures to gRPC statuses and uses the Raft request adapter without draining unrelated outbound
consensus messages. TLS credentials and peer authentication remain runtime integration work.

## Expected request flow

```text
client -> RPC adapter -> node service -> consensus -> storage
                                      -> persistence
```

Linearizable read behavior, membership changes, snapshots, and transport failure assumptions will
be captured as architecture decision records before their implementations are introduced.

## Source boundaries

- Public interfaces belong under `include/ftkv/`.
- Implementations belong under `src/` and include public headers by their installed paths.
- Wire contracts belong under `proto/` and are versioned by package name.
- Tests mirror production boundaries under `tests/` and should avoid private implementation access.
