#!/usr/bin/env bash
# Integration test: persistent sessions (clean_session=0)
# Verifies that offline clients with QoS>=1 subscriptions receive
# messages published while they were disconnected upon reconnect.
# Run from repo root: ./scripts/test_session.sh
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

_port_in_use() { ss -tlnH sport = :1883 | grep -q .; }

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
"$BROKER" >/tmp/mqtt_sess_broker.log 2>&1 &
BROKER_PID=$!
sleep 0.5

if ! kill -0 "$BROKER_PID" 2>/dev/null; then
    echo "[setup] broker failed to start" >&2
    cat /tmp/mqtt_sess_broker.log >&2
    exit 1
fi
echo "[setup] broker PID=$BROKER_PID"
echo ""

# ── Test 1: offline QoS 1 messages delivered on reconnect ────────────────────
echo "--- Test 1: offline QoS1 messages delivered on reconnect ---"
CID="sess_test_$(date +%s)"

# Phase 1: subscribe with persistent session then disconnect
"$CLI" sub -t "sess/q1" -q 1 -i "$CID" -s >/tmp/sess_t1_sub.out 2>/dev/null &
SUB=$!; sleep 0.4
kill $SUB 2>/dev/null; wait $SUB 2>/dev/null || true
sleep 0.2

# Phase 2: publish while client is offline
"$CLI" pub -t "sess/q1" -m "msg-offline-1" -q 1 >/dev/null
"$CLI" pub -t "sess/q1" -m "msg-offline-2" -q 1 >/dev/null
sleep 0.2

# Phase 3: reconnect with same client ID and persistent session
OUT="$(timeout 3 "$CLI" sub -t "sess/q1" -q 1 -i "$CID" -s 2>/dev/null || true)"
_assert_contains "offline QoS1 msg-1 delivered on reconnect" \
    "sess/q1 msg-offline-1" "$OUT"
_assert_contains "offline QoS1 msg-2 delivered on reconnect" \
    "sess/q1 msg-offline-2" "$OUT"

# ── Test 2: QoS 0 messages NOT queued for offline clients ─────────────────────
echo "--- Test 2: QoS 0 messages NOT queued ---"
CID2="sess_q0_$(date +%s)"

"$CLI" sub -t "sess/q0" -q 1 -i "$CID2" -s >/dev/null 2>/dev/null &
SUB=$!; sleep 0.4
kill $SUB 2>/dev/null; wait $SUB 2>/dev/null || true
sleep 0.2

"$CLI" pub -t "sess/q0" -m "qos0-dropped" -q 0 >/dev/null
sleep 0.2

OUT="$(timeout 2 "$CLI" sub -t "sess/q0" -q 1 -i "$CID2" -s 2>/dev/null || true)"
_assert_empty "QoS0 message not queued for offline client" "$OUT"

# ── Test 3: clean session discards stored subscriptions ───────────────────────
echo "--- Test 3: clean session discards stored subscriptions ---"
CID3="sess_clean_$(date +%s)"

# Subscribe with persistent session
"$CLI" sub -t "sess/clean" -q 1 -i "$CID3" -s >/dev/null 2>/dev/null &
SUB=$!; sleep 0.4
kill $SUB 2>/dev/null; wait $SUB 2>/dev/null || true
sleep 0.2

# Publish offline message
"$CLI" pub -t "sess/clean" -m "dropped-by-clean" -q 1 >/dev/null
sleep 0.2

# Reconnect with clean_session=1 — should discard the old session, no queued msgs
OUT="$(timeout 2 "$CLI" sub -t "sess/clean" -q 1 -i "$CID3" 2>/dev/null || true)"
_assert_empty "queued messages discarded by clean reconnect" "$OUT"

# ── Test 4: session_present flag on persistent reconnect ─────────────────────
echo "--- Test 4: session_present flag on reconnect ---"
CID4="sess_present_$(date +%s)"

# First connect: establish persistent session
"$CLI" sub -t "sess/sp" -q 1 -i "$CID4" -s >/dev/null 2>&1 &
SUB=$!; sleep 0.4
kill $SUB 2>/dev/null; wait $SUB 2>/dev/null || true
sleep 0.2

# Second connect: should get session_present=1
STDERR_OUT="$(timeout 2 "$CLI" sub -t "sess/sp" -q 1 -i "$CID4" -s 2>&1 >/dev/null || true)"
if echo "$STDERR_OUT" | grep -q "session resumed"; then
    _ok "session_present=1 reported on reconnect"
else
    _fail "session_present=1 not reported (got: $STDERR_OUT)"
fi

# ── Test 5: clean_session=1 purges pre-existing persistent session ────────────
echo "--- Test 5: clean_session=1 purges old persistent session ---"
# MQTT 3.1.1 §3.1.2.4: if clean_session=1 on connect, the broker must discard
# any stored session for that client ID — even one saved from a prior persistent connection.
CID5="sess_purge_$(date +%s)"

# Step 1: connect with clean_session=0, subscribe, disconnect to save session
"$CLI" sub -t "sess/purge/t" -q 1 -i "$CID5" -s >/dev/null 2>/dev/null &
SUB=$!; sleep 0.4
kill $SUB 2>/dev/null; wait $SUB 2>/dev/null || true
sleep 0.2

# Step 2: publish while offline — should queue in the persistent session
"$CLI" pub -t "sess/purge/t" -m "queued-msg" -q 1 >/dev/null
sleep 0.2

# Step 3: reconnect with clean_session=1 — must purge the session immediately
"$CLI" sub -t "sess/purge/other" -i "$CID5" >/tmp/sess_t5.out 2>/dev/null &
CCONN=$!; sleep 0.5

# Verify no queued messages from the old session are delivered
kill $CCONN 2>/dev/null; wait $CCONN 2>/dev/null || true

OLD_MSG="$(grep "sess/purge/t queued-msg" /tmp/sess_t5.out || true)"
if [ -z "$OLD_MSG" ]; then
    _ok "clean_session=1 purged old persistent session (no queued msg delivered)"
else
    _fail "old persistent session NOT purged — queued msg leaked: $OLD_MSG"
fi

# ── summary ───────────────────────────────────────────────────────────────────
echo ""
echo "Results: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
