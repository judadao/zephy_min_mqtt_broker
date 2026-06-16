#!/usr/bin/env bash
# Integration test: HTTP dashboard REST API (built with DASHBOARD=1)
# Tests:
#   1. GET / returns HTML dashboard
#   2. GET /api/status returns JSON with clients/subscriptions/retained
#   3. POST /api/publish delivers a message to a subscriber
#   4. GET /api/unknown returns 404
# Run from repo root: ./scripts/test_dashboard.sh
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."
DASH_DIR="$ROOT/build_out/dashboard_test"
BROKER="$DASH_DIR/mqtt_broker_dash"
CLI="$ROOT/build_out/mqtt_cli"
PASS=0; FAIL=0
BROKER_PID=0

MQTT_PORT=11884
HTTP_PORT=18080

_ok()   { echo "  [PASS] $1"; PASS=$((PASS+1)); }
_fail() { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }

_assert_contains() {
    local label="$1" expected="$2" actual="$3"
    if echo "$actual" | grep -qF "$expected"; then
        _ok "$label"
    else
        _fail "$label"
        echo "         expected: '$expected'"
        echo "         got (first 200): '${actual:0:200}'"
    fi
}

_port_in_use() { ss -tlnH "sport = :$1" 2>/dev/null | grep -q .; }

_cleanup() {
    [ $BROKER_PID -ne 0 ] && kill "$BROKER_PID" 2>/dev/null || true
    wait "$BROKER_PID" 2>/dev/null || true
}
trap _cleanup EXIT

# ── build dashboard broker ────────────────────────────────────────────────────
mkdir -p "$DASH_DIR"

if _port_in_use "$MQTT_PORT" || _port_in_use "$HTTP_PORT"; then
    echo "[setup] port $MQTT_PORT or $HTTP_PORT in use" >&2; exit 1
fi

echo "[setup] building dashboard broker (mqtt=$MQTT_PORT http=$HTTP_PORT)..."
gcc -Wall -Wextra -std=c11 -g -D_POSIX_C_SOURCE=200809L \
    -Iinclude -I. \
    -DCONFIG_MQTT_HTTP_DASHBOARD \
    -DMQTT_BROKER_PORT="$MQTT_PORT" \
    src/broker.c src/client.c src/main.c src/packet.c \
    src/session.c src/topic.c src/http.c \
    platform/posix/platform_posix.c \
    -o "$BROKER" -lpthread 2>/dev/null || {
        echo "[setup] build failed" >&2; exit 1
    }

# Patch main.c's hard-coded http_server_start(8080) is a problem — we need to
# use the default port. Let's rebuild with the default port 8080 but use a
# different MQTT port so there's no conflict. The HTTP port 8080 must be free.
if _port_in_use 8080; then
    echo "[setup] port 8080 in use; cannot run dashboard test" >&2; exit 1
fi

# Rebuild without the custom HTTP port (use default 8080)
gcc -Wall -Wextra -std=c11 -g -D_POSIX_C_SOURCE=200809L \
    -Iinclude -I. \
    -DCONFIG_MQTT_HTTP_DASHBOARD \
    -DMQTT_BROKER_PORT="$MQTT_PORT" \
    src/broker.c src/client.c src/main.c src/packet.c \
    src/session.c src/topic.c src/http.c \
    platform/posix/platform_posix.c \
    -o "$BROKER" -lpthread 2>/dev/null || {
        echo "[setup] build failed" >&2; exit 1
    }

HTTP_PORT=8080

echo "[setup] starting dashboard broker (mqtt=$MQTT_PORT http=$HTTP_PORT)..."
"$BROKER" >/tmp/mqtt_dash_broker.log 2>&1 &
BROKER_PID=$!
sleep 0.8

if ! kill -0 "$BROKER_PID" 2>/dev/null; then
    echo "[setup] broker failed to start" >&2
    cat /tmp/mqtt_dash_broker.log >&2
    exit 1
fi
echo "[setup] broker PID=$BROKER_PID"
echo ""

# ── Test 1: GET / returns HTML ────────────────────────────────────────────────
echo "--- Test 1: GET / returns HTML dashboard ---"
RESP=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:$HTTP_PORT/" 2>/dev/null || echo "000")
if [ "$RESP" = "200" ]; then
    _ok "GET / returns HTTP 200"
else
    _fail "GET / returned HTTP $RESP (expected 200)"
fi

HTML=$(curl -s "http://127.0.0.1:$HTTP_PORT/" 2>/dev/null || true)
_assert_contains "dashboard HTML contains 'MQTT Broker'" "MQTT Broker" "$HTML"

# ── Test 2: GET /api/status returns JSON ────────────────────────────────────
echo "--- Test 2: GET /api/status returns JSON ---"
STATUS=$(curl -s "http://127.0.0.1:$HTTP_PORT/api/status" 2>/dev/null || true)
echo "  status: ${STATUS:0:120}"

_assert_contains "status has 'uptime_ms'"     "uptime_ms"     "$STATUS"
_assert_contains "status has 'clients'"       "\"clients\":"  "$STATUS"
_assert_contains "status has 'subscriptions'" "subscriptions" "$STATUS"
_assert_contains "status has 'retained'"      "retained"      "$STATUS"

# ── Test 3: subscribe, then verify client appears in /api/status ─────────────
echo "--- Test 3: subscriber appears in /api/status ---"
"$CLI" sub -t "dash/t1" -p "$MQTT_PORT" -i "dash_sub_1" >/dev/null 2>/dev/null &
SUB=$!; sleep 0.5

STATUS2=$(curl -s "http://127.0.0.1:$HTTP_PORT/api/status" 2>/dev/null || true)
_assert_contains "status shows connected client" "dash_sub_1" "$STATUS2"
_assert_contains "status shows subscription"     "dash/t1"    "$STATUS2"

kill $SUB 2>/dev/null; wait $SUB 2>/dev/null || true

# ── Test 4: POST /api/publish delivers a message ─────────────────────────────
echo "--- Test 4: POST /api/publish delivers message ---"
"$CLI" sub -t "dash/api" -p "$MQTT_PORT" >/tmp/dash_t4.out 2>/dev/null &
SUB=$!; sleep 0.4

PUB_RESP=$(curl -s -X POST \
    -H "Content-Type: application/json" \
    -d '{"topic":"dash/api","payload":"hello-from-http","qos":0}' \
    "http://127.0.0.1:$HTTP_PORT/api/publish" 2>/dev/null || true)
echo "  pub response: $PUB_RESP"
sleep 0.4

kill $SUB 2>/dev/null; wait $SUB 2>/dev/null || true
_assert_contains "POST /api/publish response: ok=true" '"ok":true' "$PUB_RESP"
_assert_contains "message delivered via REST API" "dash/api hello-from-http" \
    "$(cat /tmp/dash_t4.out)"

# ── Test 5: GET /api/unknown returns 404 ────────────────────────────────────
echo "--- Test 5: GET /api/unknown returns 404 ---"
CODE=$(curl -s -o /dev/null -w "%{http_code}" \
    "http://127.0.0.1:$HTTP_PORT/api/does_not_exist" 2>/dev/null || echo "000")
if [ "$CODE" = "404" ]; then
    _ok "GET /api/unknown returns 404"
else
    _fail "GET /api/unknown returned $CODE (expected 404)"
fi

# ── Test 6: POST /api/publish with missing topic returns 400 ─────────────────
echo "--- Test 6: POST /api/publish missing topic returns 400 ---"
CODE=$(curl -s -o /dev/null -w "%{http_code}" -X POST \
    -H "Content-Type: application/json" \
    -d '{"payload":"no-topic"}' \
    "http://127.0.0.1:$HTTP_PORT/api/publish" 2>/dev/null || echo "000")
if [ "$CODE" = "400" ]; then
    _ok "POST without topic returns 400"
else
    _fail "POST without topic returned $CODE (expected 400)"
fi

# ── summary ───────────────────────────────────────────────────────────────────
echo ""
echo "Results: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
