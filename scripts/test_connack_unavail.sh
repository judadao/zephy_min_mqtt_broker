#!/usr/bin/env bash
# Integration test: CONNACK SERVER_UNAVAILABLE when broker pool is full
# MQTT 3.1.1 §3.2.2.3: broker MUST send CONNACK 0x03 before closing when
# a new connection arrives and the client pool is exhausted.
# Run from repo root: ./scripts/test_connack_unavail.sh
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."
BROKER="$ROOT/build_out/mqtt_broker"
CLI="$ROOT/build_out/mqtt_cli"
PASS=0; FAIL=0
BROKER_PID=0
declare -a SUB_PIDS=()

_ok()   { echo "  [PASS] $1"; PASS=$((PASS+1)); }
_fail() { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }

_port_in_use() { ss -tlnH "sport = :1883" 2>/dev/null | grep -q .; }

_cleanup() {
    for p in "${SUB_PIDS[@]}"; do
        kill "$p" 2>/dev/null || true
    done
    wait "${SUB_PIDS[@]}" 2>/dev/null || true
    [ $BROKER_PID -ne 0 ] && kill "$BROKER_PID" 2>/dev/null || true
    wait "$BROKER_PID" 2>/dev/null || true
}
trap _cleanup EXIT

# ── build if needed ───────────────────────────────────────────────────────────
if [ ! -x "$BROKER" ] || [ ! -x "$CLI" ]; then
    echo "[setup] building..."
    make -f "$ROOT/Makefile.linux" -C "$ROOT" -s
fi

if _port_in_use; then
    echo "[setup] port 1883 in use; stop mosquitto first" >&2
    exit 1
fi

echo "[setup] starting broker..."
"$BROKER" >/tmp/mqtt_unavail_broker.log 2>&1 &
BROKER_PID=$!
sleep 0.5

if ! kill -0 "$BROKER_PID" 2>/dev/null; then
    echo "[setup] broker failed to start" >&2
    cat /tmp/mqtt_unavail_broker.log >&2
    exit 1
fi
echo "[setup] broker PID=$BROKER_PID"
echo ""

# MQTT_MAX_CLIENTS = 8 (include/broker.h)
MAX_CLIENTS=8

# ── Test 1: Fill pool to capacity ────────────────────────────────────────────
echo "--- Test 1: fill pool with ${MAX_CLIENTS} subscribers ---"
for i in $(seq 1 $MAX_CLIENTS); do
    "$CLI" sub -t "unavail/slot" -i "filler_${i}" >/dev/null 2>/dev/null &
    SUB_PIDS+=($!)
done
sleep 0.8  # allow all connections to be accepted

# Sanity: verify broker is alive with full pool
if kill -0 "$BROKER_PID" 2>/dev/null; then
    _ok "broker alive after ${MAX_CLIENTS} connections"
else
    _fail "broker crashed while filling pool"
fi

# ── Test 2: Extra connection gets CONNACK SERVER_UNAVAILABLE ─────────────────
echo "--- Test 2: 9th connection receives CONNACK code 3 (server unavailable) ---"
ERR_OUT=$("$CLI" pub -t "unavail/x" -m "hi" -i "extra_client" 2>&1 || true)
echo "  cli output: $ERR_OUT"

if echo "$ERR_OUT" | grep -qi "server unavailable"; then
    _ok "CONNACK SERVER_UNAVAILABLE received (pool full)"
elif echo "$ERR_OUT" | grep -qi "code 3"; then
    _ok "CONNACK code 3 received (pool full)"
else
    _fail "expected 'server unavailable' but got: $ERR_OUT"
fi

# ── Test 3: Broker still alive after rejecting the extra connection ───────────
echo "--- Test 3: broker stays alive after rejecting connection ---"
if kill -0 "$BROKER_PID" 2>/dev/null; then
    _ok "broker alive after rejecting ${MAX_CLIENTS+1}th connection"
else
    _fail "broker crashed after rejecting extra connection"
fi

# ── Test 4: After freeing a slot, next connection succeeds ───────────────────
echo "--- Test 4: free one slot and reconnect ---"
# Kill the first filler to free one slot
kill "${SUB_PIDS[0]}" 2>/dev/null || true
wait "${SUB_PIDS[0]}" 2>/dev/null || true
SUB_PIDS=("${SUB_PIDS[@]:1}")  # remove from array
sleep 0.5  # allow broker to clean up the slot

NEW_OUT=$("$CLI" pub -t "unavail/recover" -m "slot_free" -i "recover_client" 2>&1 || true)
if echo "$NEW_OUT" | grep -qi "server unavailable\|refused\|code [123]"; then
    _fail "connection still rejected after slot freed: $NEW_OUT"
else
    _ok "connection accepted after freeing a slot"
fi

# ── summary ───────────────────────────────────────────────────────────────────
echo ""
echo "Results: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
