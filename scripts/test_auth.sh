#!/usr/bin/env bash
# Integration test: username/password authentication
# Builds the broker with AUTH enabled and verifies that:
#   - valid credentials are accepted
#   - wrong credentials are rejected (CONNACK 0x04)
#   - anonymous connect is rejected when auth is required
# Run from repo root: ./scripts/test_auth.sh
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."
AUTH_DIR="$ROOT/build_out/auth_test"
BROKER="$AUTH_DIR/mqtt_broker_auth"
CLI="$ROOT/build_out/mqtt_cli"   # auth does not affect cli behaviour
PASS=0; FAIL=0
BROKER_PID=0
AUTH_PORT=11883  # use a non-default port to avoid conflicts

_ok()   { echo "  [PASS] $1"; PASS=$((PASS+1)); }
_fail() { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }

_port_in_use() { ss -tlnH "sport = :$1" 2>/dev/null | grep -q .; }

_cleanup() {
    [ $BROKER_PID -ne 0 ] && kill "$BROKER_PID" 2>/dev/null || true
    wait "$BROKER_PID" 2>/dev/null || true
}
trap _cleanup EXIT

# ── build auth broker ─────────────────────────────────────────────────────────
mkdir -p "$AUTH_DIR"

if _port_in_use "$AUTH_PORT"; then
    echo "[setup] port $AUTH_PORT in use" >&2; exit 1
fi

echo "[setup] building auth broker (user=admin pass=secret port=$AUTH_PORT)..."
gcc -Wall -Wextra -std=c11 -g -D_POSIX_C_SOURCE=200809L \
    -Iinclude -I. \
    -DCONFIG_MQTT_AUTH_ENABLED \
    -DCONFIG_MQTT_AUTH_USERNAME='"admin"' \
    -DCONFIG_MQTT_AUTH_PASSWORD='"secret"' \
    -DMQTT_BROKER_PORT="$AUTH_PORT" \
    src/broker.c src/client.c src/main.c src/packet.c \
    src/session.c src/topic.c \
    platform/posix/platform_posix.c \
    -o "$BROKER" -lpthread 2>/dev/null || {
        echo "[setup] build failed" >&2; exit 1
    }

echo "[setup] starting auth broker..."
"$BROKER" >/tmp/mqtt_auth_broker.log 2>&1 &
BROKER_PID=$!
sleep 0.5

if ! kill -0 "$BROKER_PID" 2>/dev/null; then
    echo "[setup] broker failed to start" >&2
    cat /tmp/mqtt_auth_broker.log >&2
    exit 1
fi
echo "[setup] broker PID=$BROKER_PID"
echo ""

# ── Test 1: valid credentials accepted ───────────────────────────────────────
echo "--- Test 1: valid credentials accepted ---"
"$CLI" sub -p "$AUTH_PORT" -t "auth/t" -u admin -P secret >/tmp/auth_t1.out 2>/dev/null &
SUB=$!; sleep 0.3
"$CLI" pub -p "$AUTH_PORT" -t "auth/t" -m "hi" -u admin -P secret >/dev/null
sleep 0.3
kill $SUB 2>/dev/null; wait $SUB 2>/dev/null || true
if grep -qF "auth/t hi" /tmp/auth_t1.out; then
    _ok "valid credentials: message delivered"
else
    _fail "valid credentials: message not delivered"
fi

# ── Test 2: wrong password rejected ──────────────────────────────────────────
echo "--- Test 2: wrong password rejected ---"
STDERR="$("$CLI" pub -p "$AUTH_PORT" -t "auth/t" -m "x" \
               -u admin -P wrongpass 2>&1 >/dev/null || true)"
if echo "$STDERR" | grep -qi "bad credentials\|refused\|error"; then
    _ok "wrong password: connection refused"
else
    _fail "wrong password: should have been refused (got: $STDERR)"
fi

# ── Test 3: wrong username rejected ──────────────────────────────────────────
echo "--- Test 3: wrong username rejected ---"
STDERR="$("$CLI" pub -p "$AUTH_PORT" -t "auth/t" -m "x" \
               -u wronguser -P secret 2>&1 >/dev/null || true)"
if echo "$STDERR" | grep -qi "bad credentials\|refused\|error"; then
    _ok "wrong username: connection refused"
else
    _fail "wrong username: should have been refused (got: $STDERR)"
fi

# ── Test 4: anonymous connect rejected ───────────────────────────────────────
echo "--- Test 4: anonymous connect rejected ---"
STDERR="$("$CLI" pub -p "$AUTH_PORT" -t "auth/t" -m "x" 2>&1 >/dev/null || true)"
if echo "$STDERR" | grep -qi "bad credentials\|refused\|not authorized\|error"; then
    _ok "anonymous connect: rejected by auth broker"
else
    _fail "anonymous connect: should have been refused (got: $STDERR)"
fi

# ── Test 5: valid sub, valid pub → QoS 1 delivery ───────────────────────────
echo "--- Test 5: QoS1 pub/sub with valid auth ---"
"$CLI" sub -p "$AUTH_PORT" -t "auth/q1" -q 1 \
       -u admin -P secret >/tmp/auth_t5.out 2>/dev/null &
SUB=$!; sleep 0.3
"$CLI" pub -p "$AUTH_PORT" -t "auth/q1" -m "qos1-auth" -q 1 \
       -u admin -P secret >/dev/null
sleep 0.4
kill $SUB 2>/dev/null; wait $SUB 2>/dev/null || true
if grep -qF "auth/q1 qos1-auth" /tmp/auth_t5.out; then
    _ok "QoS1 with valid auth: delivered"
else
    _fail "QoS1 with valid auth: not delivered"
fi

# ── summary ───────────────────────────────────────────────────────────────────
echo ""
echo "Results: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
