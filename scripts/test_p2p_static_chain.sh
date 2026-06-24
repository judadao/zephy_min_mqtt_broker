#!/usr/bin/env bash
# Static-seed P2P chain test.  Starts N Linux brokers in a line and verifies
# MQTT publish delivery in both directions across the chain.
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${P2P_CHAIN_OUT:-$ROOT_DIR/build_out/p2p_static_chain_test}"
NODE_COUNT="${NODE_COUNT:-3}"
MQTT_BASE="${MQTT_BASE:-21883}"
P2P_BASE="${P2P_BASE:-24884}"
DISCOVERY_PORT="${DISCOVERY_PORT:-25884}"
WAIT_START_SEC="${WAIT_START_SEC:-10}"
WAIT_ROUTE_SEC="${WAIT_ROUTE_SEC:-5}"
WAIT_MSG_SEC="${WAIT_MSG_SEC:-10}"

PASS=0
FAIL=0
PIDS=""
SUB_PID=""

ok() { PASS=$((PASS + 1)); printf '  [PASS] %s\n' "$1"; }
fail() { FAIL=$((FAIL + 1)); printf '  [FAIL] %s\n' "$1"; }

cleanup() {
    for pid in $PIDS $SUB_PID; do
        [ -n "$pid" ] && kill "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

if [ "$NODE_COUNT" -lt 3 ]; then
    echo "error: NODE_COUNT must be at least 3" >&2
    exit 1
fi

mkdir -p "$OUT"

wait_for_match() {
    local file=$1
    local pattern=$2
    local timeout=$3
    local start
    local now

    start=$(date +%s)
    while :; do
        grep -q "$pattern" "$file" 2>/dev/null && return 0
        now=$(date +%s)
        [ $((now - start)) -ge "$timeout" ] && return 1
        sleep 0.1
    done
}

publish_until_match() {
    local host=$1
    local port=$2
    local topic=$3
    local payload=$4
    local file=$5
    local timeout=$6
    local start
    local now

    start=$(date +%s)
    while :; do
        "$ROOT_DIR/build_out/mqtt_cli" pub -h "$host" -p "$port" -t "$topic" -m "$payload" >/dev/null 2>&1 || true
        wait_for_match "$file" "$payload" 1 && return 0
        now=$(date +%s)
        [ $((now - start)) -ge "$timeout" ] && return 1
        sleep 0.5
    done
}

peer_list_for() {
    local idx=$1
    local peers=""

    if [ "$idx" -gt 1 ]; then
        peers="127.0.0.1:$((P2P_BASE + ((idx - 2) * 2)))"
    fi
    if [ "$idx" -lt "$NODE_COUNT" ]; then
        [ -n "$peers" ] && peers="$peers,"
        peers="${peers}127.0.0.1:$((P2P_BASE + ((idx - 1) * 2)))"
    fi
    printf '%s\n' "$peers"
}

build_broker() {
    local idx=$1
    local mqtt_port=$((MQTT_BASE + idx - 1))
    local p2p_port=$((P2P_BASE + ((idx - 1) * 2)))
    local out="$OUT/broker_$idx"

    gcc -Wall -Wextra -std=c11 -g -D_POSIX_C_SOURCE=200809L \
        -DCONFIG_MQTT_P2P_DYNAMIC \
        -DCONFIG_MQTT_P2P_STATIC_SEEDS_ONLY \
        -DMQTT_BROKER_PORT="$mqtt_port" \
        -DP2P_PORT="$p2p_port" \
        -DP2P_DISCOVERY_PORT="$DISCOVERY_PORT" \
        -I"$ROOT_DIR/include" -I"$ROOT_DIR" \
        "$ROOT_DIR/src/broker.c" "$ROOT_DIR/src/client.c" \
        "$ROOT_DIR/src/main.c" "$ROOT_DIR/src/packet.c" \
        "$ROOT_DIR/src/session.c" "$ROOT_DIR/src/topic.c" \
        "$ROOT_DIR/src/p2p_discover.c" "$ROOT_DIR/src/p2p_election.c" \
        "$ROOT_DIR/src/p2p_peer.c" "$ROOT_DIR/src/p2p_router.c" \
        "$ROOT_DIR/src/p2p_shard.c" \
        "$ROOT_DIR/platform/posix/platform_posix.c" \
        -o "$out" -lpthread
}

run_direction() {
    local sub_idx=$1
    local pub_idx=$2
    local payload=$3
    local topic="chain/roundtrip/$sub_idx/$pub_idx"
    local sub_port=$((MQTT_BASE + sub_idx - 1))
    local pub_port=$((MQTT_BASE + pub_idx - 1))
    local out_file="$OUT/sub_${sub_idx}_from_${pub_idx}.out"
    local err_file="$OUT/sub_${sub_idx}_from_${pub_idx}.err"

    : >"$out_file"
    : >"$err_file"
    "$ROOT_DIR/build_out/mqtt_cli" sub -h 127.0.0.1 -p "$sub_port" -t "$topic" >"$out_file" 2>"$err_file" &
    SUB_PID=$!
    wait_for_match "$err_file" "subscribed" "$WAIT_MSG_SEC" || true
    sleep "$WAIT_ROUTE_SEC"

    if publish_until_match 127.0.0.1 "$pub_port" "$topic" "$payload" "$out_file" "$WAIT_MSG_SEC"; then
        ok "broker$sub_idx receives publish from broker$pub_idx"
    else
        fail "broker$sub_idx did not receive publish from broker$pub_idx"
    fi
    kill "$SUB_PID" 2>/dev/null || true
    wait "$SUB_PID" 2>/dev/null || true
    SUB_PID=""
}

echo "=== test_p2p_static_chain.sh ==="
echo "  nodes:          $NODE_COUNT"
echo "  MQTT base port: $MQTT_BASE"
echo "  P2P base port:  $P2P_BASE"

make -C "$ROOT_DIR" -f Makefile.linux P2P=1 all >/dev/null || exit 1

ports=""
for i in $(seq 1 "$NODE_COUNT"); do
    ports="$ports $((MQTT_BASE + i - 1))/tcp $((P2P_BASE + ((i - 1) * 2)))/tcp"
    build_broker "$i" || exit 1
done
fuser -k $ports >/dev/null 2>&1 || true
sleep 0.3

for i in $(seq 1 "$NODE_COUNT"); do
    peers=$(peer_list_for "$i")
    MQTT_P2P_PEERS="$peers" "$OUT/broker_$i" >"$OUT/broker_$i.log" 2>&1 &
    PIDS="$PIDS $!"
done

printf '  Waiting %ds for chain formation...\n' "$WAIT_START_SEC"
sleep "$WAIT_START_SEC"

alive=0
for pid in $PIDS; do
    kill -0 "$pid" 2>/dev/null && alive=$((alive + 1)) || true
done
if [ "$alive" -eq "$NODE_COUNT" ]; then
    ok "all $NODE_COUNT brokers started"
else
    fail "only $alive/$NODE_COUNT brokers started"
fi

run_direction 1 "$NODE_COUNT" "from-last-to-first"
run_direction "$NODE_COUNT" 1 "from-first-to-last"

echo ""
echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
