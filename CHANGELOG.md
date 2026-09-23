# Changelog

All notable changes to this project are documented in this file. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and releases use
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2026-09-23

### Added

- Thread-safe in-memory and file-backed key-value storage with atomic snapshot replacement.
- Raft follower, candidate, and leader states with randomized elections and heartbeats.
- Replicated-log conflict repair, majority quorum commit, and leader recovery.
- Durable Raft term, vote, and log state.
- gRPC client and server adapters for CRUD, `RequestVote`, `AppendEntries`, and node status.
- Bounded RPC deadlines, retries, exponential backoff, and error mapping.
- Three-node Docker Compose deployment with health checks and persistent volumes.
- Native unit tests and isolated Docker integration tests for startup, failover, and recovery.
- Architecture, development, and deployment documentation.

### Known limitations

- Client key-value mutations are node-local and are not yet applied through the Raft commit path.
- Transport is plaintext and fixed-membership configuration is static.
- Persistence uses complete snapshots rather than a write-ahead log and compaction.

[1.0.0]: https://github.com/VinhLuu25/fault-tolerant-kv-store/tree/v1.0.0
