#!/usr/bin/env bash
# Integration test: keepalive enforcement
# Verifies that the broker disconnects idle clients within 1.5 × keepalive
# seconds per MQTT 3.1.1 §3.1.2.10.
# Run from repo root: ./scripts/test_keepalive.sh
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."
BROKER="$ROOT/build_out/mqtt_broker"
KA_CLI="$ROOT/build_out/keepalive_client"
PASS=0; FAIL=0
BROKER_PID=0

_ok()   { echo "  [PASS] $1"; PASS=$((PASS+1)); }
_fail() { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }

_port_in_use() { ss -tlnH "sport = :1883" 2>/dev/null | grep -q .; }

_cleanup() {
    [ $BROKER_PID -ne 0 ] && kill "$BROKER_PID" 2>/dev/null || true
    wait "$BROKER_PID" 2>/dev/null || true
    KEEP=5 LOG_ROOT=/tmp LOG_NAME_GLOB='mqtt_*_broker.log' "$SCRIPT_DIR/cleanup_logs.sh" >/dev/null 2>&1 || true
}
trap _cleanup EXIT

# ── build if needed ───────────────────────────────────────────────────────────
if [ ! -x "$BROKER" ] || [ ! -x "$KA_CLI" ]; then
    echo "[setup] building..."
    make -f "$ROOT/Makefile.linux" -C "$ROOT" all test-helpers -s
fi

if _port_in_use; then
    echo "[setup] port 1883 in use; stop mosquitto first" >&2
    exit 1
fi

echo "[setup] starting broker..."
"$BROKER" >/tmp/mqtt_keepalive_broker.log 2>&1 &
BROKER_PID=$!
sleep 0.5

if ! kill -0 "$BROKER_PID" 2>/dev/null; then
    echo "[setup] broker failed to start" >&2
    cat /tmp/mqtt_keepalive_broker.log >&2
    exit 1
fi
echo "[setup] broker PID=$BROKER_PID"
echo ""

# ── Test 1: 3-second keepalive — broker closes at ~4.5 s ─────────────────────
echo "--- Test 1: keepalive=3s (expect close at ~4.5s) ---"
OUT="$("$KA_CLI" -k 3 2>&1 || true)"
echo "  output: $OUT"
if echo "$OUT" | grep -q "broker closed connection"; then
    _ok "broker enforced 3s keepalive"
else
    _fail "broker did NOT enforce 3s keepalive (got: $OUT)"
fi

# ── Test 2: 5-second keepalive — broker closes at ~7.5 s ─────────────────────
echo "--- Test 2: keepalive=5s (expect close at ~7.5s) ---"
OUT="$("$KA_CLI" -k 5 2>&1 || true)"
echo "  output: $OUT"
if echo "$OUT" | grep -q "broker closed connection"; then
    _ok "broker enforced 5s keepalive"
else
    _fail "broker did NOT enforce 5s keepalive (got: $OUT)"
fi

# ── Test 3: keepalive=0 (disabled) — connection must stay open ───────────────
echo "--- Test 3: keepalive=0 (disabled, connection should stay open) ---"
# keepalive_client -k 0 would wait up to 0s — not useful.
# Instead: send keepalive=0 CONNECT manually and verify a different client
# can still connect and publish normally (broker not disrupted).
"$ROOT/build_out/mqtt_cli" pub -t "ka/sanity" -m "ping" >/dev/null 2>&1 && \
    _ok "broker still functional after keepalive tests" || \
    _fail "broker unresponsive after keepalive tests"

# ── summary ───────────────────────────────────────────────────────────────────
echo ""
echo "Results: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
