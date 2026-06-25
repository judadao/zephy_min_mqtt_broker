#!/usr/bin/env bash
# Validate Mode A admission fallback with two bridged Linux brokers:
# B admits two local clients, rejects the third, and the third falls back to A.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."
OUT="$ROOT/build_out/admission_fallback"
CLI="$ROOT/build_out/mqtt_cli"

A_MQTT=21884
A_P2P=24886
B_MQTT=21883
B_P2P=24884
TOPIC="admission/fallback"
WAIT_SEC=${WAIT_SEC:-12}
SUB_PROP_SEC=${SUB_PROP_SEC:-4}

PASS=0
FAIL=0
A_PID=0
B_PID=0
SUB_PIDS=()

_ok() { echo "  [PASS] $1"; PASS=$((PASS + 1)); }
_fail() { echo "  [FAIL] $1"; FAIL=$((FAIL + 1)); }

_cleanup() {
    for p in "${SUB_PIDS[@]}"; do
        kill "$p" 2>/dev/null || true
    done
    wait "${SUB_PIDS[@]}" 2>/dev/null || true
    [ "$A_PID" -ne 0 ] && kill "$A_PID" 2>/dev/null || true
    [ "$B_PID" -ne 0 ] && kill "$B_PID" 2>/dev/null || true
    wait "$A_PID" "$B_PID" 2>/dev/null || true
}
trap _cleanup EXIT

wait_for_tcp() {
    port=$1
    deadline=$((SECONDS + WAIT_SEC))
    while [ "$SECONDS" -lt "$deadline" ]; do
        timeout 1 bash -c "</dev/tcp/127.0.0.1/$port" >/dev/null 2>&1 && return 0
        sleep 0.2
    done
    return 1
}

wait_for_match() {
    file=$1
    pattern=$2
    deadline=$((SECONDS + WAIT_SEC))
    while [ "$SECONDS" -lt "$deadline" ]; do
        grep -q -- "$pattern" "$file" 2>/dev/null && return 0
        sleep 0.2
    done
    return 1
}

build_broker() {
    out=$1
    mqtt_port=$2
    p2p_port=$3
    admission=$4

    gcc -Wall -Wextra -std=c11 -g -D_POSIX_C_SOURCE=200809L \
        -DCONFIG_MQTT_P2P_DYNAMIC \
        -DCONFIG_MQTT_P2P_STATIC_SEEDS_ONLY \
        -DMQTT_BROKER_PORT="$mqtt_port" \
        -DP2P_PORT="$p2p_port" \
        -DMQTT_ADMISSION_MAX_CLIENTS="$admission" \
        -I"$ROOT/include" -I"$ROOT" \
        "$ROOT/src/broker.c" "$ROOT/src/client.c" "$ROOT/src/main.c" \
        "$ROOT/src/packet.c" "$ROOT/src/session.c" "$ROOT/src/topic.c" \
        "$ROOT/src/p2p_discover.c" "$ROOT/src/p2p_election.c" \
        "$ROOT/src/p2p_peer.c" "$ROOT/src/p2p_router.c" \
        "$ROOT/src/p2p_shard.c" "$ROOT/platform/posix/platform_posix.c" \
        -o "$out" -lpthread
}

mkdir -p "$OUT"

if [ ! -x "$CLI" ]; then
    make -C "$ROOT" -f Makefile.linux P2P=1 all
fi

echo "=== test_admission_fallback.sh ==="
echo "A broker: mqtt=$A_MQTT p2p=$A_P2P admission=8"
echo "B broker: mqtt=$B_MQTT p2p=$B_P2P admission=2"

build_broker "$OUT/broker_a" "$A_MQTT" "$A_P2P" 8
build_broker "$OUT/broker_b" "$B_MQTT" "$B_P2P" 2

fuser -k "$A_MQTT/tcp" "$A_P2P/tcp" "$B_MQTT/tcp" "$B_P2P/tcp" >/dev/null 2>&1 || true

"$OUT/broker_a" >"$OUT/broker_a.log" 2>&1 &
A_PID=$!
MQTT_P2P_PEERS="127.0.0.1:$A_P2P" "$OUT/broker_b" >"$OUT/broker_b.log" 2>&1 &
B_PID=$!

wait_for_tcp "$A_MQTT" && _ok "A MQTT listener is open" || _fail "A MQTT listener did not open"
wait_for_tcp "$B_MQTT" && _ok "B MQTT listener is open" || _fail "B MQTT listener did not open"
sleep "$SUB_PROP_SEC"

echo "--- Fill B local admission slots ---"
"$CLI" sub -h 127.0.0.1 -p "$B_MQTT" -i b_fill_1 -t "$TOPIC" >"$OUT/b_fill_1.log" 2>&1 &
SUB_PIDS+=($!)
"$CLI" sub -h 127.0.0.1 -p "$B_MQTT" -i b_fill_2 -t "$TOPIC" >"$OUT/b_fill_2.log" 2>&1 &
SUB_PIDS+=($!)
sleep "$SUB_PROP_SEC"

wait_for_match "$OUT/b_fill_1.log" "subscribed" && _ok "B filler client 1 admitted" || _fail "B filler client 1 not admitted"
wait_for_match "$OUT/b_fill_2.log" "subscribed" && _ok "B filler client 2 admitted" || _fail "B filler client 2 not admitted"

echo "--- Reject third client on B and fall back to A ---"
REJECT_OUT=$("$CLI" sub -h 127.0.0.1 -p "$B_MQTT" -i b_overflow -t "$TOPIC" 2>&1 >/dev/null || true)
echo "$REJECT_OUT" >"$OUT/b_overflow.err"
if echo "$REJECT_OUT" | grep -qi "server unavailable\\|code 3"; then
    _ok "B rejects overflow client with CONNACK server-unavailable"
else
    _fail "B overflow client was not rejected as server-unavailable: $REJECT_OUT"
fi

"$CLI" sub -h 127.0.0.1 -p "$A_MQTT" -i fallback_on_a -t "$TOPIC" >"$OUT/a_fallback.log" 2>&1 &
SUB_PIDS+=($!)
wait_for_match "$OUT/a_fallback.log" "subscribed" && _ok "overflow client falls back to A" || _fail "overflow client did not subscribe on A"
sleep "$SUB_PROP_SEC"

echo "--- Publish to A and verify A fallback plus B receive-only delivery ---"
MSG="admission-fallback-$(date +%s)"
"$CLI" pub -h 127.0.0.1 -p "$A_MQTT" -i pub_on_a -t "$TOPIC" -m "$MSG" >/dev/null 2>&1

wait_for_match "$OUT/a_fallback.log" "$MSG" && _ok "A fallback client receives publish" || _fail "A fallback client missed publish"
wait_for_match "$OUT/b_fill_1.log" "$MSG" && _ok "B existing client receives bridged publish" || _fail "B existing client missed bridged publish"

if kill -0 "$A_PID" 2>/dev/null && kill -0 "$B_PID" 2>/dev/null; then
    _ok "both brokers survived admission fallback test"
else
    _fail "one or both brokers exited"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
