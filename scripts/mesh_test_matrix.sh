#!/usr/bin/env bash
# Mesh test matrix for ESP32-like broker pools.
#
# Runs a small set of repeatable scenarios:
# - single broker baseline
# - 5-broker mesh baseline
# - 5-broker node-down fault
# - 5-broker node-restart fault
# - 5-broker dense connectivity
# - 10-broker stress
#
# Output is CSV on stdout and a copy in build_out/mesh_test_matrix/results.csv.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."
OUT="${OUT:-$ROOT/build_out/mesh_test_matrix}"
BENCH="${BENCH:-$SCRIPT_DIR/bench_p2p_docker_scale.sh}"

BASE_NETWORK_NAME="${BASE_NETWORK_NAME:-mqtt-mesh-matrix}"
BASE_NETWORK_SUBNET_OCTET="${BASE_NETWORK_SUBNET_OCTET:-60}"
TOPICS="${TOPICS:-100}"
MESSAGES="${MESSAGES:-1000}"
SENSOR_CLIENTS="${SENSOR_CLIENTS:-0}"
SENSOR_CONNECTIONS="${SENSOR_CONNECTIONS:-0}"
SENSOR_WORKERS="${SENSOR_WORKERS:-64}"
STARTUP_SEC="${STARTUP_SEC:-2}"
SYNC_SETTLE_SEC="${SYNC_SETTLE_SEC:-10}"
TARGET_P95_MS="${TARGET_P95_MS:-10}"

mkdir -p "$OUT"
RESULTS_FILE="$OUT/results.csv"
: > "$RESULTS_FILE"
echo "scenario,broker_count,sent,received,lost,delivery_pct,msg_per_sec,p95_ms,max_ms,pass_p95" | tee -a "$RESULTS_FILE"

run_case() {
    local scenario="$1"
    shift

    local tmp
    tmp="$(mktemp)"
    if ! env \
        OUT="$OUT/$scenario" \
        TOPIC_COUNTS="$TOPICS" \
        MESSAGE_COUNTS="$MESSAGES" \
        SENSOR_CLIENTS="$SENSOR_CLIENTS" \
        SENSOR_CONNECTIONS="$SENSOR_CONNECTIONS" \
        SENSOR_WORKERS="$SENSOR_WORKERS" \
        STARTUP_SEC="$STARTUP_SEC" \
        SYNC_SETTLE_SEC="$SYNC_SETTLE_SEC" \
        TARGET_P95_MS="$TARGET_P95_MS" \
        "$@" \
        "$BENCH" >"$tmp" 2>&1; then
        cat "$tmp" >&2
        rm -f "$tmp"
        return 1
    fi

    local line
    line="$(grep '^mqtt_min_broker_docker,' "$tmp" | tail -n 1 || true)"
    cat "$tmp" >&2
    rm -f "$tmp"

    if [ -z "$line" ]; then
        echo "[fail] no result line for $scenario" >&2
        return 1
    fi

    IFS=, read -r impl broker_count total_subs messages received lost setup_sec elapsed_sec msg_per_sec min_ms p50_ms p95_ms p99_ms max_ms pass_p95 <<EOF
$line
EOF
    delivery_pct="$(awk -v r="$received" -v m="$messages" 'BEGIN { if (m > 0) printf "%.1f", (r * 100.0) / m; else print "0.0" }')"
    echo "$scenario,$broker_count,$messages,$received,$lost,$delivery_pct,$msg_per_sec,$p95_ms,$max_ms,$pass_p95" | tee -a "$RESULTS_FILE"
}

run_case "single_1" \
    NETWORK_NAME="${BASE_NETWORK_NAME}-single1" \
    NETWORK_SUBNET="172.${BASE_NETWORK_SUBNET_OCTET}.0.0/16" \
    IP_PREFIX="172.${BASE_NETWORK_SUBNET_OCTET}.1" \
    BROKER_COUNTS="1" \
    ESP32_PROFILE=0 \
    DISTRIBUTED_PUBLISHERS=1 \
    SCALE_MESSAGES_BY_BROKER=1 \
    SENSOR_CLIENTS=0

run_case "mesh_5" \
    NETWORK_NAME="${BASE_NETWORK_NAME}-mesh5" \
    NETWORK_SUBNET="172.$((BASE_NETWORK_SUBNET_OCTET + 1)).0.0/16" \
    IP_PREFIX="172.$((BASE_NETWORK_SUBNET_OCTET + 1)).1" \
    BROKER_COUNTS="5" \
    ESP32_PROFILE=0 \
    MQTT_MAX_CLIENTS_BENCH=8 \
    P2P_PEER_MAX_BENCH=5 \
    ROUTER_COUNT=5 \
    STATIC_SEED_FANOUT=4 \
    DISTRIBUTED_PUBLISHERS=1 \
    SCALE_MESSAGES_BY_BROKER=1 \
    SENSOR_CLIENTS=0

run_case "mesh_5_node_down" \
    NETWORK_NAME="${BASE_NETWORK_NAME}-down" \
    NETWORK_SUBNET="172.$((BASE_NETWORK_SUBNET_OCTET + 2)).0.0/16" \
    IP_PREFIX="172.$((BASE_NETWORK_SUBNET_OCTET + 2)).1" \
    BROKER_COUNTS="5" \
    ESP32_PROFILE=0 \
    MQTT_MAX_CLIENTS_BENCH=8 \
    P2P_PEER_MAX_BENCH=5 \
    ROUTER_COUNT=5 \
    STATIC_SEED_FANOUT=4 \
    DISTRIBUTED_PUBLISHERS=1 \
    SCALE_MESSAGES_BY_BROKER=1 \
    SENSOR_CLIENTS=0 \
    FAIL_BROKER_COUNT=5 \
    FAIL_NODE_INDEX=2 \
    FAIL_ACTION=kill \
    FAIL_AFTER_SEC=3

run_case "mesh_5_restart" \
    NETWORK_NAME="${BASE_NETWORK_NAME}-restart" \
    NETWORK_SUBNET="172.$((BASE_NETWORK_SUBNET_OCTET + 3)).0.0/16" \
    IP_PREFIX="172.$((BASE_NETWORK_SUBNET_OCTET + 3)).1" \
    BROKER_COUNTS="5" \
    ESP32_PROFILE=0 \
    MQTT_MAX_CLIENTS_BENCH=8 \
    P2P_PEER_MAX_BENCH=5 \
    ROUTER_COUNT=5 \
    STATIC_SEED_FANOUT=4 \
    DISTRIBUTED_PUBLISHERS=1 \
    SCALE_MESSAGES_BY_BROKER=1 \
    SENSOR_CLIENTS=0 \
    FAIL_BROKER_COUNT=5 \
    FAIL_NODE_INDEX=2 \
    FAIL_ACTION=restart \
    FAIL_AFTER_SEC=3

run_case "mesh_5_dense" \
    NETWORK_NAME="${BASE_NETWORK_NAME}-dense" \
    NETWORK_SUBNET="172.$((BASE_NETWORK_SUBNET_OCTET + 4)).0.0/16" \
    IP_PREFIX="172.$((BASE_NETWORK_SUBNET_OCTET + 4)).1" \
    BROKER_COUNTS="5" \
    ESP32_PROFILE=0 \
    MQTT_MAX_CLIENTS_BENCH=8 \
    P2P_PEER_MAX_BENCH=5 \
    ROUTER_COUNT=5 \
    STATIC_SEED_FANOUT=4 \
    DISTRIBUTED_PUBLISHERS=1 \
    SCALE_MESSAGES_BY_BROKER=1 \
    SENSOR_CLIENTS=0

run_case "mesh_10" \
    NETWORK_NAME="${BASE_NETWORK_NAME}-mesh10" \
    NETWORK_SUBNET="172.$((BASE_NETWORK_SUBNET_OCTET + 5)).0.0/16" \
    IP_PREFIX="172.$((BASE_NETWORK_SUBNET_OCTET + 5)).1" \
    BROKER_COUNTS="10" \
    ESP32_PROFILE=1 \
    DISTRIBUTED_PUBLISHERS=1 \
    SCALE_MESSAGES_BY_BROKER=1 \
    SENSOR_CLIENTS=0
