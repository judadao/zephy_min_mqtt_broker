#!/usr/bin/env bash
# Validate fallback ingress MQTT QoS 0/1/2 handshakes and mesh delivery.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."
OUT="${FALLBACK_QOS_OUT:-$ROOT/build_out/fallback_qos}"

NODE_A_MQTT="${NODE_A_MQTT:-19883}"
NODE_A_FALLBACK="${NODE_A_FALLBACK:-19884}"
NODE_A_P2P="${NODE_A_P2P:-49884}"
NODE_B_MQTT="${NODE_B_MQTT:-19885}"
NODE_B_FALLBACK="${NODE_B_FALLBACK:-19886}"
NODE_B_P2P="${NODE_B_P2P:-49885}"
DISCOVERY_PORT="${DISCOVERY_PORT:-49886}"
WAIT_SEC="${WAIT_SEC:-8}"

PASS=0
FAIL=0
A_PID=0
B_PID=0
SUB_PID=0

_ok() { echo "  [PASS] $1"; PASS=$((PASS + 1)); }
_fail() { echo "  [FAIL] $1"; FAIL=$((FAIL + 1)); }

_cleanup() {
    [ "$SUB_PID" -ne 0 ] && kill "$SUB_PID" 2>/dev/null || true
    [ "$A_PID" -ne 0 ] && kill "$A_PID" 2>/dev/null || true
    [ "$B_PID" -ne 0 ] && kill "$B_PID" 2>/dev/null || true
    wait "$SUB_PID" 2>/dev/null || true
    wait "$A_PID" 2>/dev/null || true
    wait "$B_PID" 2>/dev/null || true
}
trap _cleanup EXIT INT TERM

_port_in_use() {
    ss -tulnH "sport = :$1" 2>/dev/null | grep -q .
}

_require_free_ports() {
    for p in "$NODE_A_MQTT" "$NODE_A_FALLBACK" "$NODE_A_P2P" \
             "$NODE_B_MQTT" "$NODE_B_FALLBACK" "$NODE_B_P2P" "$DISCOVERY_PORT"; do
        if _port_in_use "$p"; then
            echo "[setup] port $p is already in use" >&2
            exit 1
        fi
    done
}

_build_cli() {
    gcc -Wall -Wextra -std=c11 -g -D_POSIX_C_SOURCE=200809L \
        -I"$ROOT/include" -I"$ROOT" \
        "$ROOT/tools/mqtt_cli.c" \
        "$ROOT/src/packet.c" \
        "$ROOT/platform/posix/platform_posix.c" \
        -o "$OUT/mqtt_cli" -lpthread
}

_build_broker() {
    local out="$1" p2p_port="$2"
    gcc -Wall -Wextra -std=c11 -g -D_POSIX_C_SOURCE=200809L \
        -DCONFIG_MQTT_P2P_DYNAMIC \
        -DCONFIG_MQTT_P2P_STATIC_SEEDS_ONLY \
        -DP2P_PORT="$p2p_port" \
        -DP2P_DISCOVERY_PORT="$DISCOVERY_PORT" \
        -I"$ROOT/include" -I"$ROOT" \
        "$ROOT/tests/fallback_listener_broker.c" \
        "$ROOT/src/broker.c" \
        "$ROOT/src/client.c" \
        "$ROOT/src/packet.c" \
        "$ROOT/src/session.c" \
        "$ROOT/src/topic.c" \
        "$ROOT/src/p2p_discover.c" \
        "$ROOT/src/p2p_election.c" \
        "$ROOT/src/p2p_peer.c" \
        "$ROOT/src/p2p_router.c" \
        "$ROOT/src/p2p_shard.c" \
        "$ROOT/platform/posix/platform_posix.c" \
        -o "$out" -lpthread
}

_wait_for_tcp() {
    local host="$1" port="$2" deadline=$((SECONDS + WAIT_SEC))
    while [ "$SECONDS" -lt "$deadline" ]; do
        timeout 1 bash -c "</dev/tcp/$host/$port" >/dev/null 2>&1 && return 0
        sleep 0.2
    done
    return 1
}

_wait_for_match() {
    local file="$1" pattern="$2" deadline=$((SECONDS + WAIT_SEC))
    while [ "$SECONDS" -lt "$deadline" ]; do
        grep -Fq "$pattern" "$file" 2>/dev/null && return 0
        sleep 0.2
    done
    return 1
}

_start_brokers() {
    MQTT_P2P_PEERS="127.0.0.1:$NODE_B_P2P" "$OUT/broker_a" \
        "$NODE_A_MQTT" "$NODE_A_FALLBACK" >"$OUT/a.log" 2>&1 &
    A_PID=$!
    MQTT_P2P_PEERS="127.0.0.1:$NODE_A_P2P" "$OUT/broker_b" \
        "$NODE_B_MQTT" "$NODE_B_FALLBACK" >"$OUT/b.log" 2>&1 &
    B_PID=$!

    _wait_for_tcp 127.0.0.1 "$NODE_A_MQTT" &&
    _wait_for_tcp 127.0.0.1 "$NODE_A_FALLBACK" &&
    _wait_for_tcp 127.0.0.1 "$NODE_B_MQTT" &&
    _wait_for_tcp 127.0.0.1 "$NODE_B_FALLBACK"
}

_run_delivery_case() {
    local name="$1" sub_port="$2" pub_port="$3" qos="$4" topic="fallback/qos/$1/$4"
    local msg="msg-$name-q$qos-$(date +%s%N)"
    local out="$OUT/sub-$name-q$qos.out"
    local err="$OUT/sub-$name-q$qos.err"

    "$OUT/mqtt_cli" sub -h 127.0.0.1 -p "$sub_port" \
        -i "sub_${name}_q${qos}" -t "$topic" -q "$qos" >"$out" 2>"$err" &
    SUB_PID=$!
    if ! _wait_for_match "$err" "subscribed"; then
        _fail "$name QoS$qos subscriber connected"
        return
    fi

    sleep 1
    if "$OUT/mqtt_cli" pub -h 127.0.0.1 -p "$pub_port" \
        -i "pub_${name}_q${qos}" -t "$topic" -m "$msg" -q "$qos" >/dev/null 2>"$OUT/pub-$name-q$qos.err"; then
        _ok "$name QoS$qos publisher handshake"
    else
        _fail "$name QoS$qos publisher handshake"
    fi

    if _wait_for_match "$out" "$topic $msg"; then
        local count
        count=$(grep -F "$topic $msg" "$out" | wc -l)
        if [ "$count" -eq 1 ]; then
            _ok "$name QoS$qos delivered once"
        else
            _fail "$name QoS$qos duplicate count=$count"
        fi
    else
        _fail "$name QoS$qos delivered once"
    fi

    kill "$SUB_PID" 2>/dev/null || true
    wait "$SUB_PID" 2>/dev/null || true
    SUB_PID=0
}

echo "[setup] building fallback QoS test binaries..."
mkdir -p "$OUT"
_require_free_ports
_build_cli || exit 1
_build_broker "$OUT/broker_a" "$NODE_A_P2P" || exit 1
_build_broker "$OUT/broker_b" "$NODE_B_P2P" || exit 1

echo "[setup] starting fallback mesh brokers..."
if _start_brokers; then
    _ok "brokers expose primary and fallback listeners"
else
    _fail "brokers expose primary and fallback listeners"
    cat "$OUT/a.log" "$OUT/b.log" >&2
    exit 1
fi

sleep 4

for qos in 0 1 2; do
    _run_delivery_case "local-fallback" "$NODE_A_FALLBACK" "$NODE_A_FALLBACK" "$qos"
    _run_delivery_case "remote-primary-to-fallback" "$NODE_A_FALLBACK" "$NODE_B_MQTT" "$qos"
    _run_delivery_case "remote-fallback-to-fallback" "$NODE_A_FALLBACK" "$NODE_B_FALLBACK" "$qos"
done

echo ""
echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
