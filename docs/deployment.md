# Local three-node deployment

The Compose deployment runs three fixed-membership Raft nodes on an isolated bridge network. Node
1 has the shortest randomized election window, making it the preferred initial leader candidate;
nodes 2 and 3 start as followers. Raft still determines leadership through a majority vote.

## Start the cluster

```bash
docker compose up --build --detach --wait
docker compose ps
```

Each container listens on port `50051`. The ports exposed on the host are:

| Service | Node ID | Expected steady role | Host endpoint | Persistent volume |
| --- | ---: | --- | --- | --- |
| `node1` | 1 | leader | `127.0.0.1:50051` | `node1-data` |
| `node2` | 2 | follower | `127.0.0.1:50052` | `node2-data` |
| `node3` | 3 | follower | `127.0.0.1:50053` | `node3-data` |

Inspect the role and replicated-log progress from inside a container:

```bash
docker compose exec node1 ftkv_server --status 127.0.0.1:50051
docker compose logs --follow
```

The named volumes store `raft.snapshot` and `kv.snapshot` beneath `/var/lib/ftkv`. A normal
`docker compose down` preserves them; use `docker compose down --volumes` only when intentionally
resetting all local state.

## Configuration

Copy `.env.example` to `.env` to override host ports or election timing. Each service also receives
the following runtime variables directly from `docker-compose.yml`:

- `FTKV_NODE_ID`: stable member ID.
- `FTKV_LISTEN_ADDRESS`: gRPC bind address.
- `FTKV_DATA_DIR`: durable state directory.
- `FTKV_PEERS`: comma-separated `node_id=host:port` membership list.
- `FTKV_HEARTBEAT_INTERVAL_MS`: leader heartbeat interval.
- `FTKV_ELECTION_TIMEOUT_MIN_MS` and `FTKV_ELECTION_TIMEOUT_MAX_MS`: randomized election window.

Election windows must remain greater than the heartbeat interval. The staggered defaults make
local startup deterministic without bypassing the Raft election protocol.

## Restart and recovery check

The integration script starts the cluster, verifies the expected roles, restarts node 3, and checks
that its term and log progress recover from the persistent volume:

```bash
./docker/test_cluster.sh
```

To perform the same operation manually:

```bash
docker compose restart node3
docker compose exec node3 ftkv_server --status 127.0.0.1:50051
```
