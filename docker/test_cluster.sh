#!/usr/bin/env bash
set -euo pipefail

status() {
    local service="$1"
    docker compose exec -T "${service}" \
        ftkv_server --status 127.0.0.1:50051 2>/dev/null
}

field() {
    local line="$1"
    local name="$2"
    sed -n "s/.* ${name}=\([^ ]*\).*/\1/p" <<<"${line}"
}

wait_for_role() {
    local service="$1"
    local role="$2"
    local output=""
    for _ in {1..60}; do
        if output="$(status "${service}")" && [[ "${output}" == *"state=${role}"* ]]; then
            printf '%s\n' "${output}"
            return 0
        fi
        sleep 1
    done
    printf 'timed out waiting for %s to become %s\n' "${service}" "${role}" >&2
    docker compose logs --no-color --tail=100 "${service}" >&2
    return 1
}

wait_for_recovery() {
    local minimum_term="$1"
    local minimum_commit="$2"
    local minimum_log="$3"
    local output=""
    local term=""
    local commit=""
    local log=""
    for _ in {1..60}; do
        if output="$(status node3)" && [[ "${output}" == *"state=follower"* ]] &&
            [[ "${output}" == *"leader_id=1"* ]]; then
            term="$(field " ${output}" term)"
            commit="$(field " ${output}" commit_index)"
            log="$(field " ${output}" last_log_index)"
            if [[ -n "${term}" && -n "${commit}" && -n "${log}" ]] &&
                ((term >= minimum_term && commit >= minimum_commit && log >= minimum_log)); then
                printf '%s\n' "${output}"
                return 0
            fi
        fi
        sleep 1
    done
    printf 'node3 did not recover its persisted Raft progress\n' >&2
    docker compose logs --no-color --tail=100 node3 >&2
    return 1
}

trap 'docker compose logs --no-color --tail=100 >&2' ERR

docker compose up --build --detach --wait

node1_status="$(wait_for_role node1 leader)"
node2_status="$(wait_for_role node2 follower)"
node3_before="$(wait_for_role node3 follower)"

before_term="$(field " ${node3_before}" term)"
before_commit="$(field " ${node3_before}" commit_index)"
before_log="$(field " ${node3_before}" last_log_index)"

docker compose restart node3
node3_after="$(wait_for_recovery "${before_term}" "${before_commit}" "${before_log}")"

printf 'cluster ready\n%s\n%s\nnode3 before restart: %s\nnode3 after restart:  %s\n' \
    "${node1_status}" "${node2_status}" "${node3_before}" "${node3_after}"
