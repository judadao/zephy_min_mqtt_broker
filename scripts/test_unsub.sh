#!/usr/bin/env bash
# Integration test: UNSUBSCRIBE
# Verifies that after UNSUBSCRIBE the client stops receiving messages on that
# topic, while other subscribers are unaffected.
# Run from repo root: ./scripts/test_unsub.sh
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
"$BROKER" >/tmp/mqtt_unsub_broker.log 2>&1 &
BROKER_PID=$!
sleep 0.5

if ! kill -0 "$BROKER_PID" 2>/dev/null; then
    echo "[setup] broker failed to start" >&2
    cat /tmp/mqtt_unsub_broker.log >&2
    exit 1
fi
echo "[setup] broker PID=$BROKER_PID"
echo ""

# ── Test 1: basic UNSUBSCRIBE stops message delivery ─────────────────────────
echo "--- Test 1: after UNSUBSCRIBE, no more messages ---"
# Start a persistent subscriber that will receive one message, then unsub.
# The 'unsub' command: connect, SUBSCRIBE, send UNSUBSCRIBE, read UNSUBACK,
# then drain 1 second — any messages in that drain window are "unexpected".
STDERR_OUT="$("$CLI" unsub -t "unsub/t1" 2>&1 || true)"
sleep 0.2

# Now publish — the client has already unsubscribed, so it should not receive.
"$CLI" pub -t "unsub/t1" -m "should-not-arrive" >/dev/null
sleep 0.3

if echo "$STDERR_OUT" | grep -q "unsubscribed"; then
    _ok "UNSUBACK received — unsubscribe completed"
else
    _fail "UNSUBACK not received (got: $STDERR_OUT)"
fi
if echo "$STDERR_OUT" | grep -q "unexpected:"; then
    _fail "message arrived after UNSUBSCRIBE"
else
    _ok "no messages after UNSUBSCRIBE"
fi

# ── Test 2: another subscriber still receives after first unsubscribes ────────
echo "--- Test 2: fan-out unaffected by one client unsubscribing ---"
"$CLI" sub -t "unsub/t2" >/tmp/unsub_t2.out 2>/dev/null &
STABLE_SUB=$!; sleep 0.3

# The unsub client connects, subscribes, unsubscribes, then disconnects.
"$CLI" unsub -t "unsub/t2" >/dev/null 2>/dev/null
sleep 0.2

# Publish — only the stable subscriber should receive this.
"$CLI" pub -t "unsub/t2" -m "for-stable-sub" >/dev/null
sleep 0.3

kill $STABLE_SUB 2>/dev/null; wait $STABLE_SUB 2>/dev/null || true
_assert_contains "stable subscriber received message" \
    "unsub/t2 for-stable-sub" "$(cat /tmp/unsub_t2.out)"

# ── Test 3: re-subscribe after UNSUBSCRIBE works ──────────────────────────────
echo "--- Test 3: re-subscribe after UNSUBSCRIBE ---"
"$CLI" sub -t "unsub/t3" >/tmp/unsub_t3.out 2>/dev/null &
RESUB=$!; sleep 0.3

# Publish before unsubscribe — should arrive.
"$CLI" pub -t "unsub/t3" -m "before-unsub" >/dev/null
sleep 0.2

kill $RESUB 2>/dev/null; wait $RESUB 2>/dev/null || true

# Re-subscribe and receive a new message.
"$CLI" sub -t "unsub/t3" >/tmp/unsub_t3b.out 2>/dev/null &
RESUB2=$!; sleep 0.3
"$CLI" pub -t "unsub/t3" -m "after-resub" >/dev/null
sleep 0.3
kill $RESUB2 2>/dev/null; wait $RESUB2 2>/dev/null || true

_assert_contains "message before unsub delivered" \
    "unsub/t3 before-unsub" "$(cat /tmp/unsub_t3.out)"
_assert_contains "message after re-subscribe delivered" \
    "unsub/t3 after-resub" "$(cat /tmp/unsub_t3b.out)"

# ── Test 4: UNSUBACK packet ID matches UNSUBSCRIBE ───────────────────────────
echo "--- Test 4: UNSUBSCRIBE / UNSUBACK roundtrip (QoS 1 topic) ---"
STDERR_OUT2="$("$CLI" unsub -t "unsub/t4" -q 1 2>&1 || true)"
if echo "$STDERR_OUT2" | grep -q "unsubscribed from 'unsub/t4'"; then
    _ok "QoS1 UNSUBSCRIBE / UNSUBACK roundtrip"
else
    _fail "QoS1 UNSUBSCRIBE failed (got: $STDERR_OUT2)"
fi

# ── Test 5: UNSUBSCRIBE from wildcard subscription ───────────────────────────
echo "--- Test 5: unsubscribe from wildcard topic ---"
STDERR_OUT3="$("$CLI" unsub -t "unsub/wild/#" 2>&1 || true)"
if echo "$STDERR_OUT3" | grep -q "unsubscribed from 'unsub/wild/#'"; then
    _ok "wildcard UNSUBSCRIBE / UNSUBACK"
else
    _fail "wildcard UNSUBSCRIBE failed (got: $STDERR_OUT3)"
fi

# ── summary ───────────────────────────────────────────────────────────────────
echo ""
echo "Results: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
