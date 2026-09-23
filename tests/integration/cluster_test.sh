#!/usr/bin/env bash
set -euo pipefail

readonly action="${1:-}"
readonly docker_executable="${FTKV_DOCKER_EXECUTABLE:-docker}"
readonly project_name="${FTKV_TEST_PROJECT_NAME:-ftkv-ctest-cluster}"
readonly script_directory="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly repository_root="$(cd -- "${script_directory}/../.." && pwd)"

if [[ ! "${project_name}" =~ ^ftkv-ctest-[a-z0-9][a-z0-9-]*$ ]]; then
    printf 'FAILED: unsafe integration-test project name: %s\n' "${project_name}" >&2
    exit 2
fi

# Ephemeral published ports let the integration suite coexist with a developer cluster.
export FTKV_NODE1_PORT=0
export FTKV_NODE2_PORT=0
export FTKV_NODE3_PORT=0
export FTKV_IMAGE="${project_name}:local"

compose() {
    "${docker_executable}" compose \
        --project-name "${project_name}" \
        --project-directory "${repository_root}" \
        --file "${repository_root}/docker-compose.yml" \
        "$@"
}

status() {
    local service="$1"
    compose exec -T "${service}" ftkv_server --status 127.0.0.1:50051 2>/dev/null
}

field() {
    local line="$1"
    local name="$2"
    sed -n "s/.* ${name}=\([^ ]*\).*/\1/p" <<<" ${line}"
}

fail() {
    printf 'FAILED: %s\n' "$1" >&2
    return 1
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
    fail "${service} did not become ${role}"
}

wait_for_recovery() {
    local service="$1"
    local expected_leader="$2"
    local minimum_term="$3"
    local minimum_commit="$4"
    local minimum_log="$5"
    local output=""
    local term=""
    local commit=""
    local log=""
    for _ in {1..60}; do
        if output="$(status "${service}")" && [[ "${output}" == *"state=follower"* ]] &&
            [[ "${output}" == *"leader_id=${expected_leader}"* ]]; then
            term="$(field "${output}" term)"
            commit="$(field "${output}" commit_index)"
            log="$(field "${output}" last_log_index)"
            if [[ -n "${term}" && -n "${commit}" && -n "${log}" ]] &&
                ((term >= minimum_term && commit >= minimum_commit && log >= minimum_log)); then
                printf '%s\n' "${output}"
                return 0
            fi
        fi
        sleep 1
    done
    fail "${service} did not recover its persisted Raft progress"
}

require_docker() {
    if ! "${docker_executable}" info >/dev/null 2>&1 ||
        ! "${docker_executable}" compose version >/dev/null 2>&1; then
        printf 'SKIPPED: Docker daemon or Compose plugin is unavailable\n'
        exit 77
    fi
}

show_logs_on_error() {
    local exit_code=$?
    if ((exit_code != 0)); then
        compose logs --no-color --tail=100 >&2 || true
    fi
    exit "${exit_code}"
}

test_startup() {
    compose down --volumes --remove-orphans --rmi all >/dev/null 2>&1 || true
    compose up --build --detach --wait

    local node1_status
    local node2_status
    local node3_status
    node1_status="$(status node1)"
    node2_status="$(status node2)"
    node3_status="$(status node3)"

    [[ "${node1_status}" == *"node_id=1"* ]] || fail "node1 reported the wrong ID"
    [[ "${node2_status}" == *"node_id=2"* ]] || fail "node2 reported the wrong ID"
    [[ "${node3_status}" == *"node_id=3"* ]] || fail "node3 reported the wrong ID"
    printf 'cluster startup verified\n%s\n%s\n%s\n' \
        "${node1_status}" "${node2_status}" "${node3_status}"
}

test_election() {
    local node1_status
    local node2_status
    local node3_status
    node1_status="$(wait_for_role node1 leader)"
    node2_status="$(wait_for_role node2 follower)"
    node3_status="$(wait_for_role node3 follower)"

    [[ "${node2_status}" == *"leader_id=1"* ]] || fail "node2 did not recognize node1"
    [[ "${node3_status}" == *"leader_id=1"* ]] || fail "node3 did not recognize node1"
    printf 'leader election verified\n%s\n%s\n%s\n' \
        "${node1_status}" "${node2_status}" "${node3_status}"
}

test_recovery() {
    local before
    local after
    local before_term
    local before_commit
    local before_log
    local marker
    local node3_status
    local recovered_marker

    before="$(wait_for_role node1 leader)"
    before_term="$(field "${before}" term)"
    before_commit="$(field "${before}" commit_index)"
    before_log="$(field "${before}" last_log_index)"
    [[ -n "${before_term}" && -n "${before_commit}" && -n "${before_log}" ]] ||
        fail "node1 returned incomplete Raft progress"
    marker="recovery-${project_name}"
    compose exec -T node1 sh -c \
        'printf "%s" "$1" > /var/lib/ftkv/.ctest-recovery-marker' sh "${marker}"

    compose stop --timeout 1 node1
    wait_for_role node2 leader >/dev/null
    node3_status="$(wait_for_role node3 follower)"
    [[ "${node3_status}" == *"leader_id=2"* ]] || fail "node3 did not recognize node2"

    compose rm --force node1
    compose up --detach --no-deps node1
    after="$(wait_for_recovery node1 2 "${before_term}" "${before_commit}" "${before_log}")"
    recovered_marker="$(compose exec -T node1 cat /var/lib/ftkv/.ctest-recovery-marker)"
    [[ "${recovered_marker}" == "${marker}" ]] || fail "node1 did not retain its data volume"

    printf 'leader failure and node recovery verified\nbefore: %s\nafter:  %s\n' \
        "${before}" "${after}"
}

test_cleanup() {
    compose down --volumes --remove-orphans --rmi all
    printf 'cluster cleanup verified\n'
}

require_docker
trap show_logs_on_error ERR

case "${action}" in
    startup)
        test_startup
        ;;
    election)
        test_election
        ;;
    recovery)
        test_recovery
        ;;
    cleanup)
        test_cleanup
        ;;
    *)
        fail "usage: cluster_test.sh {startup|election|recovery|cleanup}"
        ;;
esac
