#!/usr/bin/env bash
# Dynamic broker smoke test: two local P2P-enabled brokers, one subscriber,
# and one publisher. Run from anywhere:
#   ./scripts/test_p2p_dynamic.sh
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."
OUT="$ROOT/build_out/p2p_test"

NODE_A_MQTT=18831
NODE_B_MQTT=18832
NODE_A_P2P=48841
NODE_B_P2P=48842
DISCOVERY_PORT=48850

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
    KEEP=5 LOG_ROOT="$OUT" "$SCRIPT_DIR/cleanup_logs.sh" >/dev/null 2>&1 || true
}
trap _cleanup EXIT

_port_in_use() {
    ss -tulnH "sport = :$1" 2>/dev/null | grep -q .
}

_require_free_ports() {
    for p in "$NODE_A_MQTT" "$NODE_B_MQTT" "$NODE_A_P2P" "$NODE_B_P2P" "$DISCOVERY_PORT"; do
        if _port_in_use "$p"; then
            echo "[setup] port $p is already in use" >&2
            exit 1
        fi
    done
}

_build_node() {
    local out="$1" mqtt_port="$2" p2p_port="$3"
    gcc -Wall -Wextra -std=c11 -g -D_POSIX_C_SOURCE=200809L \
        -I"$ROOT/include" -I"$ROOT" \
        -DCONFIG_MQTT_P2P_DYNAMIC \
        -DMQTT_BROKER_PORT="$mqtt_port" \
        -DP2P_PORT="$p2p_port" \
        -DP2P_DISCOVERY_PORT="$DISCOVERY_PORT" \
        "$ROOT/src/broker.c" \
        "$ROOT/src/client.c" \
        "$ROOT/src/main.c" \
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

_build_cli() {
    gcc -Wall -Wextra -std=c11 -g -D_POSIX_C_SOURCE=200809L \
        -I"$ROOT/include" -I"$ROOT" \
        "$ROOT/tools/mqtt_cli.c" \
        "$ROOT/src/packet.c" \
        "$ROOT/platform/posix/platform_posix.c" \
        -o "$OUT/mqtt_cli" -lpthread
}

_wait_for_log() {
    local file="$1" pattern="$2" tries=30
    while [ "$tries" -gt 0 ]; do
        if grep -q "$pattern" "$file" 2>/dev/null; then
            return 0
        fi
        sleep 0.5
        tries=$((tries - 1))
    done
    return 1
}

_run_broker() {
    local bin="$1" log="$2" seeds="${3:-}"
    if command -v stdbuf >/dev/null 2>&1; then
        MQTT_P2P_PEERS="$seeds" stdbuf -oL -eL "$bin" > "$log" 2>&1 &
    else
        MQTT_P2P_PEERS="$seeds" "$bin" > "$log" 2>&1 &
    fi
    echo $!
}

echo "[setup] building P2P test binaries..."
mkdir -p "$OUT"
_require_free_ports
_build_node "$OUT/broker_a" "$NODE_A_MQTT" "$NODE_A_P2P" || exit 1
_build_node "$OUT/broker_b" "$NODE_B_MQTT" "$NODE_B_P2P" || exit 1
_build_cli || exit 1

echo "[setup] starting broker A and B..."
B_PID=$(_run_broker "$OUT/broker_b" "$OUT/b.log")
sleep 0.5
A_PID=$(_run_broker "$OUT/broker_a" "$OUT/a.log" "127.0.0.1:$NODE_B_P2P")
sleep 1

if ! kill -0 "$A_PID" 2>/dev/null || ! kill -0 "$B_PID" 2>/dev/null; then
    _fail "brokers started"
    cat "$OUT/a.log" "$OUT/b.log" >&2
    exit 1
fi
_ok "brokers started"

if _wait_for_log "$OUT/a.log" "P2P peer connected" ||
   _wait_for_log "$OUT/b.log" "P2P peer connected"; then
    _ok "P2P peer connection established"
else
    _fail "P2P peer connection established"
    cat "$OUT/a.log" "$OUT/b.log" >&2
    exit 1
fi

echo "[test] B subscribes, A publishes..."
"$OUT/mqtt_cli" sub -p "$NODE_B_MQTT" -t "p2p/test" > "$OUT/sub.out" 2>"$OUT/sub.err" &
SUB_PID=$!
sleep 1
"$OUT/mqtt_cli" pub -p "$NODE_A_MQTT" -t "p2p/test" -m "dynamic-ok" >/dev/null 2>&1
sleep 1
kill "$SUB_PID" 2>/dev/null
wait "$SUB_PID" 2>/dev/null || true
SUB_PID=0

if grep -q "p2p/test dynamic-ok" "$OUT/sub.out"; then
    _ok "remote PUBLISH delivered to subscriber on other broker"
else
    _fail "remote PUBLISH delivered to subscriber on other broker"
    echo "[debug] subscriber output:" >&2
    cat "$OUT/sub.out" >&2
    echo "[debug] broker logs:" >&2
    cat "$OUT/a.log" "$OUT/b.log" >&2
fi

echo ""
echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
