# Local Deployment Runbook

This runbook operates the three-node Docker Compose environment. It is designed for development,
demonstration, and failure testing on one machine; it is not a production security or scheduling
configuration.

## Topology

Compose creates one bridge network and three fixed-membership Raft nodes. Every container listens
on gRPC port `50051`, while host port mappings provide separate local endpoints.

| Service | Node ID | Preferred startup role | Host endpoint | Named volume |
| --- | ---: | --- | --- | --- |
| `node1` | 1 | Leader candidate | `127.0.0.1:50051` | `node1-data` |
| `node2` | 2 | Follower | `127.0.0.1:50052` | `node2-data` |
| `node3` | 3 | Follower | `127.0.0.1:50053` | `node3-data` |

Node 1 has the shortest randomized election window. This makes it the preferred initial candidate,
but it becomes leader only after receiving a majority vote. If it fails, the remaining two nodes
can elect a replacement.

## Prerequisites

- Docker Engine or Docker Desktop
- Docker Compose plugin with support for `docker compose up --wait`
- Ports `50051`, `50052`, and `50053` available, unless overridden

Confirm the tools and daemon are available:

```bash
docker version
docker compose version
```

## Configuration

The committed defaults work without additional files. To customize them:

```bash
cp .env.example .env
```

| Variable | Default | Purpose |
| --- | --- | --- |
| `FTKV_IMAGE` | `ftkv-store:local` | Local image name and tag |
| `FTKV_NODE1_PORT` | `50051` | Node 1 host port |
| `FTKV_NODE2_PORT` | `50052` | Node 2 host port |
| `FTKV_NODE3_PORT` | `50053` | Node 3 host port |
| `FTKV_HEARTBEAT_INTERVAL_MS` | `100` | Leader heartbeat interval |
| `FTKV_NODE1_ELECTION_TIMEOUT_MIN_MS` | `300` | Node 1 minimum election timeout |
| `FTKV_NODE1_ELECTION_TIMEOUT_MAX_MS` | `400` | Node 1 maximum election timeout |
| `FTKV_NODE2_ELECTION_TIMEOUT_MIN_MS` | `700` | Node 2 minimum election timeout |
| `FTKV_NODE2_ELECTION_TIMEOUT_MAX_MS` | `850` | Node 2 maximum election timeout |
| `FTKV_NODE3_ELECTION_TIMEOUT_MIN_MS` | `1100` | Node 3 minimum election timeout |
| `FTKV_NODE3_ELECTION_TIMEOUT_MAX_MS` | `1250` | Node 3 maximum election timeout |

Each service also receives node-specific runtime settings from `docker-compose.yml`:

- `FTKV_NODE_ID`: stable Raft member ID.
- `FTKV_LISTEN_ADDRESS`: gRPC bind address inside the container.
- `FTKV_DATA_DIR`: location of persistent node state.
- `FTKV_PEERS`: comma-separated `node_id=service:port` membership list.

Election timeouts must remain greater than the heartbeat interval. Keep enough separation between
node windows if deterministic local leader selection matters.

## Build and start

Build the multi-stage image, start the cluster, and wait for all health checks:

```bash
docker compose up --build --detach --wait
docker compose ps
```

Expected result: all three services report `healthy`. The health probe calls the internal status
RPC with a bounded deadline; it confirms process and gRPC readiness, not cluster leadership.

## Verify leadership

Query each node from inside its container:

```bash
docker compose exec node1 ftkv_server --status 127.0.0.1:50051
docker compose exec node2 ftkv_server --status 127.0.0.1:50051
docker compose exec node3 ftkv_server --status 127.0.0.1:50051
```

A stable default deployment should show node 1 as leader and nodes 2 and 3 as followers with
`leader_id=1`. Inspect lifecycle and election events with:

```bash
docker compose logs --follow --tail=100
```

## Persistence

Each node mounts a dedicated named volume at `/var/lib/ftkv`:

- `raft.snapshot` stores the durable Raft term, vote, and replicated log.
- `kv.snapshot` stores node-local key-value data after the first mutation.

Atomic file replacement prevents a failed save from publishing partially updated in-memory state.
The current snapshot format does not provide a write-ahead log, explicit `fsync` guarantees, or
cross-process file locking.

A normal shutdown preserves named volumes:

```bash
docker compose down
```

Starting the stack again reattaches the same volumes:

```bash
docker compose up --detach --wait
```

## Failure and recovery exercises

Restart a follower and verify that it rejoins the leader:

```bash
docker compose restart node3
docker compose exec node3 ftkv_server --status 127.0.0.1:50051
```

Simulate leader failure and observe quorum failover:

```bash
docker compose stop node1
sleep 2
docker compose exec node2 ftkv_server --status 127.0.0.1:50051
docker compose start node1
sleep 1
docker compose exec node1 ftkv_server --status 127.0.0.1:50051
```

With the default timing, node 2 should become leader and the recovered node 1 should step down to
follower after learning the higher term.

## Automated deployment tests

The standalone smoke test starts the default project, checks roles, restarts node 3, and verifies
Raft progress after recovery:

```bash
./docker/test_cluster.sh
```

The CTest integration fixture is more isolated and comprehensive. It uses a unique project name,
ephemeral host ports, and test-only volumes; stops and removes the leader; verifies node 2 takes
over; recreates node 1; checks persistent volume recovery; and removes all test resources.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure --label-regex integration
```

## Troubleshooting

### A host port is already allocated

Override ports in `.env`, then recreate the stack:

```dotenv
FTKV_NODE1_PORT=51051
FTKV_NODE2_PORT=51052
FTKV_NODE3_PORT=51053
```

```bash
docker compose down
docker compose up --detach --wait
```

### No leader is elected

Check that all peers are running and inspect election logs:

```bash
docker compose ps
docker compose logs --tail=200 node1 node2 node3
```

Verify that every election timeout is larger than the heartbeat interval and that all services are
attached to the `raft` network.

### A node repeatedly fails health checks

Inspect the node logs and configuration:

```bash
docker compose logs --tail=200 node1
docker compose config
```

Corrupt snapshots intentionally fail fast rather than silently discarding durable state. Reset the
development data only when losing the local cluster state is acceptable.

## Reset or remove the deployment

Stop containers but keep state:

```bash
docker compose down
```

Delete containers, network, and all three data volumes:

```bash
docker compose down --volumes
```

The second command permanently removes local cluster state. The image can be removed separately
with `docker image rm ftkv-store:local` if it is no longer needed.

## Current production gaps

- Client mutations are not yet routed through Raft or replicated between key-value stores.
- Transport uses plaintext credentials and has no peer authentication.
- Membership is fixed at process startup.
- Persistence uses full snapshots instead of a write-ahead log and compaction.
- Metrics, traces, rate limiting, admission control, and automated backups are not implemented.

These constraints make the deployment appropriate for local systems engineering and testing, not
for untrusted networks or production data.
