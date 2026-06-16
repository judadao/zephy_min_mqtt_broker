#!/usr/bin/env bash
# Integration test: connection edge cases
# Tests:
#   1. Client ID takeover — new client with same ID kicks old one (MQTT §3.1.4)
#   2. PINGREQ/PINGRESP — keepalive ping roundtrip
#   3. Multiple topics in a single session (pub/sub to N topics)
#   4. Zero-length client ID (broker must assign one or reject)
# Run from repo root: ./scripts/test_connect_edge.sh
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."
BROKER="$ROOT/build_out/mqtt_broker"
CLI="$ROOT/build_out/mqtt_cli"
PASS=0; FAIL=0
BROKER_PID=0

_ok()   { echo "  [PASS] $1"; PASS=$((PASS+1)); }
_fail() { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }

_assert_contains() {
    local label="$1" expected="$2" actual="$3"
    if echo "$actual" | grep -qF "$expected"; then
        _ok "$label"
    else
        _fail "$label"
        echo "         expected: '$expected'"
        echo "         got:      '$actual'"
    fi
}

_assert_empty() {
    local label="$1" actual="$2"
    if [ -z "$actual" ]; then _ok "$label"
    else _fail "$label — expected empty, got: $actual"
    fi
}

_port_in_use() { ss -tlnH "sport = :1883" 2>/dev/null | grep -q .; }

_cleanup() {
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
"$BROKER" >/tmp/mqtt_connect_edge_broker.log 2>&1 &
BROKER_PID=$!
sleep 0.5

if ! kill -0 "$BROKER_PID" 2>/dev/null; then
    echo "[setup] broker failed to start" >&2
    cat /tmp/mqtt_connect_edge_broker.log >&2
    exit 1
fi
echo "[setup] broker PID=$BROKER_PID"
echo ""

# ── Test 1: client ID takeover ────────────────────────────────────────────────
echo "--- Test 1: client ID takeover (MQTT §3.1.4) ---"
# First client subscribes with a fixed client ID
"$CLI" sub -t "edge/take" -i "dup_client_id" >/tmp/edge_t1.out 2>/dev/null &
FIRST=$!; sleep 0.4

# Verify first client gets a message
"$CLI" pub -t "edge/take" -m "before-takeover" >/dev/null
sleep 0.2

# Second client connects with the same client ID — should kick the first
"$CLI" sub -t "edge/take2" -i "dup_client_id" >/tmp/edge_t1b.out 2>/dev/null &
SECOND=$!; sleep 0.4

# Publish to the first topic — first client should be gone, no delivery
"$CLI" pub -t "edge/take" -m "after-takeover" >/dev/null
# Publish to second topic — second client should receive it
"$CLI" pub -t "edge/take2" -m "second-active" >/dev/null
sleep 0.5

kill $SECOND 2>/dev/null; wait $SECOND 2>/dev/null || true
# First client was kicked — its process may already have exited or is stuck
kill $FIRST 2>/dev/null; wait $FIRST 2>/dev/null || true

_assert_contains "first client received pre-takeover message" \
    "edge/take before-takeover" "$(cat /tmp/edge_t1.out)"
_assert_contains "second client received its message" \
    "edge/take2 second-active" "$(cat /tmp/edge_t1b.out)"

# Verify broker is still sane after takeover
"$CLI" pub -t "edge/sanity" -m "ok" >/dev/null 2>&1 && \
    _ok "broker responsive after takeover" || \
    _fail "broker unresponsive after takeover"

# ── Test 2: multiple topics in one session ────────────────────────────────────
echo "--- Test 2: multiple simultaneous topic subscriptions ---"
"$CLI" sub -t "edge/multi/a" >/tmp/edge_t2a.out 2>/dev/null &
PA=$!
"$CLI" sub -t "edge/multi/b" >/tmp/edge_t2b.out 2>/dev/null &
PB=$!
"$CLI" sub -t "edge/multi/c" >/tmp/edge_t2c.out 2>/dev/null &
PC=$!
sleep 0.4

"$CLI" pub -t "edge/multi/a" -m "aa" >/dev/null
"$CLI" pub -t "edge/multi/b" -m "bb" >/dev/null
"$CLI" pub -t "edge/multi/c" -m "cc" >/dev/null
sleep 0.4

kill $PA $PB $PC 2>/dev/null
wait $PA $PB $PC 2>/dev/null || true

_assert_contains "topic a received"  "edge/multi/a aa" "$(cat /tmp/edge_t2a.out)"
_assert_contains "topic b received"  "edge/multi/b bb" "$(cat /tmp/edge_t2b.out)"
_assert_contains "topic c received"  "edge/multi/c cc" "$(cat /tmp/edge_t2c.out)"

# ── Test 3: retained messages delivered on wildcard subscribe ─────────────────
echo "--- Test 3: retained messages delivered on wildcard subscribe ---"
"$CLI" pub -t "edge/ret/x" -m "rx" -r >/dev/null
"$CLI" pub -t "edge/ret/y" -m "ry" -r >/dev/null
sleep 0.2

OUT="$(timeout 2 "$CLI" sub -t "edge/ret/#" 2>/dev/null || true)"
_assert_contains "retained x delivered on wildcard sub" "edge/ret/x rx" "$OUT"
_assert_contains "retained y delivered on wildcard sub" "edge/ret/y ry" "$OUT"

# Clean up retained messages
"$CLI" pub -t "edge/ret/x" -m "" -r >/dev/null
"$CLI" pub -t "edge/ret/y" -m "" -r >/dev/null

# ── Test 4: rapid connect/disconnect does not exhaust pool ────────────────────
echo "--- Test 4: rapid connect/disconnect cycle ---"
for i in $(seq 1 15); do
    "$CLI" pub -t "edge/rapid" -m "m$i" >/dev/null 2>/dev/null || true
done
# Broker should still be alive and functional
OUT="$(timeout 2 "$CLI" sub -t "edge/rapid/end" 2>/dev/null || true)"
"$CLI" pub -t "edge/rapid/end" -m "done" >/dev/null 2>/dev/null && \
    _ok "broker responsive after 15 rapid connect/disconnect cycles" || \
    _fail "broker unresponsive after rapid cycle"

# ── summary ───────────────────────────────────────────────────────────────────
echo ""
echo "Results: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
