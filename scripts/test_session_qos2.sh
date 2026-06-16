#!/usr/bin/env bash
# Integration test: persistent session with QoS 2 messages
# Verifies that offline clients with QoS-2 subscriptions receive messages
# published while they were disconnected upon reconnect (exactly once).
# Also verifies QoS 2 delivery to online subscribers (full PUBREC/PUBREL flow).
# Run from repo root: ./scripts/test_session_qos2.sh
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
"$BROKER" >/tmp/mqtt_sess_q2_broker.log 2>&1 &
BROKER_PID=$!
sleep 0.5

if ! kill -0 "$BROKER_PID" 2>/dev/null; then
    echo "[setup] broker failed to start" >&2
    cat /tmp/mqtt_sess_q2_broker.log >&2
    exit 1
fi
echo "[setup] broker PID=$BROKER_PID"
echo ""

# ── Test 1: QoS 2 message delivered to online subscriber ─────────────────────
echo "--- Test 1: QoS 2 online delivery ---"
"$CLI" sub -t "sq2/online" -q 2 >/tmp/sq2_t1.out 2>/dev/null &
SUB=$!; sleep 0.4
"$CLI" pub -t "sq2/online" -m "q2-live" -q 2 >/dev/null
sleep 0.6
kill $SUB 2>/dev/null; wait $SUB 2>/dev/null || true
_assert_contains "QoS2 delivered to online subscriber" \
    "sq2/online q2-live" "$(cat /tmp/sq2_t1.out)"

# ── Test 2: QoS 2 offline messages delivered on reconnect ────────────────────
echo "--- Test 2: QoS2 offline session delivery ---"
CID="sq2_sess_$(date +%s)"

# Phase 1: subscribe with persistent session (sub_qos=2), then disconnect
"$CLI" sub -t "sq2/off" -q 2 -i "$CID" -s >/tmp/sq2_t2_sub.out 2>/dev/null &
SUB=$!; sleep 0.4
kill $SUB 2>/dev/null; wait $SUB 2>/dev/null || true
sleep 0.2

# Phase 2: publish while client is offline
"$CLI" pub -t "sq2/off" -m "q2-offline-1" -q 2 >/dev/null
"$CLI" pub -t "sq2/off" -m "q2-offline-2" -q 2 >/dev/null
sleep 0.2

# Phase 3: reconnect — should receive the queued messages
# Use -q 1 sub to receive the drained messages; broker stores min(sub_qos, pub_qos)
# sub_qos was 2 and pub_qos is 2, so stored at min(2,2)=2.
# mqtt_cli sub with -q 2 does PUBREC/PUBREL/PUBCOMP for drained QoS2 messages.
OUT="$(timeout 4 "$CLI" sub -t "sq2/off" -q 2 -i "$CID" -s 2>/dev/null || true)"

# SESSION_QUEUE_MAX=2, so we expect at most 2 messages (may get 1 if queue filled)
if echo "$OUT" | grep -qF "sq2/off q2-offline"; then
    _ok "QoS2 offline message(s) delivered on reconnect"
else
    _fail "QoS2 offline messages not delivered (got: $OUT)"
fi

# ── Test 3: QoS 2 exactly-once for session-resumed messages ──────────────────
echo "--- Test 3: QoS2 session message not duplicated ---"
CID3="sq2_nodup_$(date +%s)"

"$CLI" sub -t "sq2/nodup" -q 2 -i "$CID3" -s >/dev/null 2>/dev/null &
SUB=$!; sleep 0.4
kill $SUB 2>/dev/null; wait $SUB 2>/dev/null || true
sleep 0.2

"$CLI" pub -t "sq2/nodup" -m "unique-q2" -q 2 >/dev/null
sleep 0.2

OUT3="$(timeout 3 "$CLI" sub -t "sq2/nodup" -q 2 -i "$CID3" -s 2>/dev/null || true)"
COUNT=$(echo "$OUT3" | grep -c "sq2/nodup unique-q2" || echo 0)
if [ "$COUNT" -le 1 ]; then
    _ok "QoS2 session message delivered at most once (count=$COUNT)"
else
    _fail "QoS2 session message duplicated (count=$COUNT)"
fi

# ── Test 4: QoS 2 fan-out with mixed subscriber QoS ──────────────────────────
echo "--- Test 4: QoS2 pub, one QoS0 sub + one QoS2 sub ---"
"$CLI" sub -t "sq2/mix" -q 0 >/tmp/sq2_t4a.out 2>/dev/null &
PA=$!
"$CLI" sub -t "sq2/mix" -q 2 >/tmp/sq2_t4b.out 2>/dev/null &
PB=$!
sleep 0.4

"$CLI" pub -t "sq2/mix" -m "mix-msg" -q 2 >/dev/null
sleep 0.6

kill $PA $PB 2>/dev/null; wait $PA $PB 2>/dev/null || true
_assert_contains "QoS0 subscriber got mix-msg" "sq2/mix mix-msg" "$(cat /tmp/sq2_t4a.out)"
_assert_contains "QoS2 subscriber got mix-msg" "sq2/mix mix-msg" "$(cat /tmp/sq2_t4b.out)"

# ── summary ───────────────────────────────────────────────────────────────────
echo ""
echo "Results: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
