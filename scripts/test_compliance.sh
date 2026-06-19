#!/usr/bin/env bash
# Integration test: MQTT compliance — 4 spec gaps
#   Gap 1: SUBACK granted-QoS byte (§3.9.3)
#   Gap 2: Retained message cleared by zero-length PUBLISH (§3.3.1.3)
#   Gap 3: Client-ID takeover with clean_session=1 discards QoS 2 inflight (§3.1.2.4)
#   Gap 4: Overlapping subscriptions deliver only one message copy (§4.7.2)
# Run from repo root: ./scripts/test_compliance.sh
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."
BROKER="$ROOT/build_out/mqtt_broker"
CLI="$ROOT/build_out/mqtt_cli"
OVERLAP="$ROOT/build_out/overlap_sub_test"
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
    KEEP=5 LOG_ROOT=/tmp LOG_NAME_GLOB='mqtt_*_broker.log' "$SCRIPT_DIR/cleanup_logs.sh" >/dev/null 2>&1 || true
}
trap _cleanup EXIT

# ── build if needed ───────────────────────────────────────────────────────────
if [ ! -x "$BROKER" ] || [ ! -x "$CLI" ] || [ ! -x "$OVERLAP" ]; then
    echo "[setup] building..."
    make -f "$ROOT/Makefile.linux" -C "$ROOT" all test-helpers -s
fi

if _port_in_use; then
    echo "[setup] port 1883 in use; stop mosquitto first" >&2
    exit 1
fi

echo "[setup] starting broker..."
"$BROKER" >/tmp/mqtt_compliance_broker.log 2>&1 &
BROKER_PID=$!
sleep 0.5

if ! kill -0 "$BROKER_PID" 2>/dev/null; then
    echo "[setup] broker failed to start" >&2
    cat /tmp/mqtt_compliance_broker.log >&2
    exit 1
fi
echo "[setup] broker PID=$BROKER_PID"
echo ""

# ── Gap 1: SUBACK granted-QoS byte (§3.9.3) ─────────────────────────────────
echo "--- Gap 1: SUBACK granted-QoS (§3.9.3) ---"

for q in 0 1 2; do
    OUT="$(timeout 1 "$CLI" sub -t "compliance/suback/q${q}" -q "${q}" 2>&1 || true)"
    _assert_contains "SUBACK granted QoS ${q}" "granted ${q}" "$OUT"
done

# ── Gap 2: Retained message cleared by zero-length PUBLISH (§3.3.1.3) ───────
echo "--- Gap 2: retain cleared by empty PUBLISH (§3.3.1.3) ---"

"$CLI" pub -t "compliance/ret/clear" -m "hello" -r >/dev/null
sleep 0.2

RET_BEFORE="$(timeout 1 "$CLI" sub -t "compliance/ret/clear" 2>/dev/null || true)"
_assert_contains "retained message delivered" "compliance/ret/clear hello" "$RET_BEFORE"

"$CLI" pub -t "compliance/ret/clear" -m "" -r >/dev/null
sleep 0.2

RET_AFTER="$(timeout 1 "$CLI" sub -t "compliance/ret/clear" 2>/dev/null || true)"
_assert_empty "retain cleared — no message on re-subscribe" "$RET_AFTER"

# ── Gap 3: Client-ID takeover with clean_session=1 discards QoS 2 inflight ───
echo "--- Gap 3: clean_session=1 takeover discards prior QoS 2 inflight (§3.1.2.4) ---"

# Step 1: persistent sub client on topic at QoS 2
"$CLI" sub -t "compliance/takeover/q2" -q 2 -s -i "compliance-takeover" \
    >/tmp/compliance_takeover_1.out 2>/tmp/compliance_takeover_1.err &
SUB1=$!
sleep 0.3

# Step 2: publish a QoS 2 message — it should arrive
"$CLI" pub -t "compliance/takeover/q2" -m "inflight-msg" -q 2 >/dev/null
sleep 0.3

# Step 3: kill the persistent sub client
kill $SUB1 2>/dev/null; wait $SUB1 2>/dev/null || true

# Step 4: publish another QoS 2 message while client is offline (stored in session)
"$CLI" pub -t "compliance/takeover/q2" -m "should-not-arrive" -q 2 >/dev/null
sleep 0.2

# Step 5: reconnect with SAME ID but clean_session=1 (no -s flag)
# Use timeout 2 so we have time to receive a new message as a positive signal
"$CLI" sub -t "compliance/takeover/q2" -q 2 -i "compliance-takeover" \
    >/tmp/compliance_takeover_2.out 2>/tmp/compliance_takeover_2.err &
SUB2=$!
sleep 0.3

# Step 6: verify "session resumed" is NOT printed (clean connect discarded old session)
STDERR2="$(cat /tmp/compliance_takeover_2.err)"
if echo "$STDERR2" | grep -q "session resumed"; then
    _fail "clean reconnect must NOT resume session — got: $STDERR2"
else
    _ok "clean reconnect does not resume prior session"
fi

# Step 7: publish fresh message — it MUST arrive (subscription active)
"$CLI" pub -t "compliance/takeover/q2" -m "new-msg" -q 2 >/dev/null
sleep 0.4

kill $SUB2 2>/dev/null; wait $SUB2 2>/dev/null || true

OUT2="$(cat /tmp/compliance_takeover_2.out)"
# "should-not-arrive" must not be in output
if echo "$OUT2" | grep -q "should-not-arrive"; then
    _fail "discarded-session message 'should-not-arrive' was incorrectly replayed"
else
    _ok "'should-not-arrive' not delivered after clean reconnect"
fi
_assert_contains "fresh message 'new-msg' delivered after clean reconnect" \
    "compliance/takeover/q2 new-msg" "$OUT2"

# ── Gap 4: Overlapping subscriptions deliver only one copy (§4.7.2) ──────────
echo "--- Gap 4: overlapping subscriptions deliver one copy (§4.7.2) ---"

# Start the overlap helper in the background; it subscribes then waits 2s
OVERLAP_OUT="$("$OVERLAP" &
OVERLAP_PID=$!
sleep 0.3
"$CLI" pub -t "overlap/test" -m "x" >/dev/null
sleep 0.5
# helper exits on its own after 2s timeout; wait for it
wait $OVERLAP_PID 2>/dev/null
)"

# overlap_sub_test prints "received N\n" to stdout
COUNT="$(echo "$OVERLAP_OUT" | grep -oP '(?<=received )\d+' || echo "")"
if [ "$COUNT" = "1" ]; then
    _ok "overlapping subscriptions: exactly one copy delivered (received 1)"
elif [ "$COUNT" = "2" ]; then
    _fail "overlapping subscriptions: duplicate delivery (received 2)"
else
    _fail "overlapping subscriptions: unexpected output: '$OVERLAP_OUT'"
fi

# ── summary ───────────────────────────────────────────────────────────────────
echo ""
echo "Results: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
