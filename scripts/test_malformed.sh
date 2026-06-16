#!/usr/bin/env bash
# Integration test: malformed/edge-case packet handling
# Verifies the broker does not crash or hang when it receives:
#   - A TCP connection that disconnects immediately
#   - A CONNECT with mismatched remaining length
#   - Garbage bytes (no valid MQTT header)
#   - A valid CONNECT then immediately large garbage
# Run from repo root: ./scripts/test_malformed.sh
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."
BROKER="$ROOT/build_out/mqtt_broker"
CLI="$ROOT/build_out/mqtt_cli"
PASS=0; FAIL=0
BROKER_PID=0

_ok()   { echo "  [PASS] $1"; PASS=$((PASS+1)); }
_fail() { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }

_port_in_use() { ss -tlnH "sport = :1883" 2>/dev/null | grep -q .; }

_broker_alive() { kill -0 "$BROKER_PID" 2>/dev/null; }

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
"$BROKER" >/tmp/mqtt_malformed_broker.log 2>&1 &
BROKER_PID=$!
sleep 0.5

if ! _broker_alive; then
    echo "[setup] broker failed to start" >&2
    cat /tmp/mqtt_malformed_broker.log >&2
    exit 1
fi
echo "[setup] broker PID=$BROKER_PID"
echo ""

_sanity() {
    "$CLI" pub -t "malformed/sanity" -m "ok" >/dev/null 2>&1
}

# ── Test 1: immediate TCP close (no CONNECT) ──────────────────────────────────
echo "--- Test 1: immediate TCP close ---"
# Just open and immediately close a connection
(echo -n "" | nc -q 0 127.0.0.1 1883) 2>/dev/null || \
(echo -n "" | nc -w 0 127.0.0.1 1883) 2>/dev/null || true
sleep 0.3
if _broker_alive && _sanity; then
    _ok "broker survives immediate TCP close"
else
    _fail "broker crashed after immediate TCP close"
fi

# ── Test 2: send garbage bytes ────────────────────────────────────────────────
echo "--- Test 2: garbage bytes on connection ---"
printf '\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF' | nc -q1 127.0.0.1 1883 >/dev/null 2>/dev/null || \
printf '\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF' | nc -w2 127.0.0.1 1883 >/dev/null 2>/dev/null || true
sleep 0.5
if _broker_alive && _sanity; then
    _ok "broker survives garbage bytes"
else
    _fail "broker crashed after garbage bytes"
fi

# ── Test 3: valid CONNECT then garbage ───────────────────────────────────────
echo "--- Test 3: valid CONNECT then garbage payload ---"
# Build a minimal valid CONNECT then append garbage
{
    # CONNECT packet: type=0x10, rem=12, "MQTT"(0,4,M,Q,T,T), level=4, flags=2, ka=0,0, id_len=2, id=AB
    printf '\x10\x0e\x00\x04MQTT\x04\x02\x00\x00\x00\x02AB'
    sleep 0.1
    # Now send garbage after CONNECT accepted
    printf '\xde\xad\xbe\xef\xde\xad\xbe\xef'
} | nc -q1 127.0.0.1 1883 >/dev/null 2>/dev/null || \
{
    printf '\x10\x0e\x00\x04MQTT\x04\x02\x00\x00\x00\x02AB'
    sleep 0.1
    printf '\xde\xad\xbe\xef\xde\xad\xbe\xef'
} | nc -w2 127.0.0.1 1883 >/dev/null 2>/dev/null || true
sleep 0.5
if _broker_alive && _sanity; then
    _ok "broker survives CONNECT followed by garbage"
else
    _fail "broker crashed after CONNECT followed by garbage"
fi

# ── Test 4: oversized remaining-length encoding ──────────────────────────────
echo "--- Test 4: CONNECT with malformed remaining length ---"
# Send a CONNECT type byte then 0xFF 0xFF 0xFF 0xFF (invalid 5-byte remaining len)
printf '\x10\xff\xff\xff\xff' | nc -q1 127.0.0.1 1883 >/dev/null 2>/dev/null || \
printf '\x10\xff\xff\xff\xff' | nc -w1 127.0.0.1 1883 >/dev/null 2>/dev/null || true
sleep 0.3
if _broker_alive && _sanity; then
    _ok "broker survives malformed remaining length"
else
    _fail "broker crashed after malformed remaining length"
fi

# ── Test 5: 10 rapid malformed connections ───────────────────────────────────
echo "--- Test 5: 10 rapid malformed connections ---"
for i in $(seq 1 10); do
    printf '\x00\x00\x00\x00' | nc -q0 127.0.0.1 1883 >/dev/null 2>/dev/null || \
    printf '\x00\x00\x00\x00' | nc -w1 127.0.0.1 1883 >/dev/null 2>/dev/null || true
done
sleep 0.5
if _broker_alive && _sanity; then
    _ok "broker survives 10 rapid malformed connections"
else
    _fail "broker crashed after rapid malformed connections"
fi

# ── Test 6: non-CONNECT as first packet closes connection ────────────────────
echo "--- Test 6: non-CONNECT as first packet (MQTT §3.1) ---"
# Send a PUBLISH as the very first packet — broker must close immediately
printf '\x30\x07\x00\x03t/xhi' | nc -q1 127.0.0.1 1883 >/dev/null 2>/dev/null || \
printf '\x30\x07\x00\x03t/xhi' | nc -w2 127.0.0.1 1883 >/dev/null 2>/dev/null || true
sleep 0.3
if _broker_alive && _sanity; then
    _ok "broker survives non-CONNECT first packet (MQTT §3.1)"
else
    _fail "broker crashed after non-CONNECT first packet"
fi

# ── Test 7: PUBLISH with QoS=3 (reserved) closes connection ─────────────────
echo "--- Test 7: PUBLISH with QoS=3 (reserved) ---"
# First connect, then send a PUBLISH with QoS bits = 11 (0x06 in fixed header)
# CONNECT: \x10\x0e\x00\x04MQTT\x04\x02\x00\x00\x00\x02AB
# PUBLISH QoS=3: type=0x36 (MQTT_PUBLISH=0x30 | QoS=3<<1=0x06), rem=9, topic 3-byte "t/x" + payload "hi"
{
    printf '\x10\x0e\x00\x04MQTT\x04\x02\x00\x00\x00\x02AB'
    sleep 0.2
    printf '\x36\x07\x00\x03t/xhi'
    sleep 0.3
} | nc -q1 127.0.0.1 1883 >/dev/null 2>/dev/null || \
{
    printf '\x10\x0e\x00\x04MQTT\x04\x02\x00\x00\x00\x02AB'
    sleep 0.2
    printf '\x36\x07\x00\x03t/xhi'
    sleep 0.3
} | nc -w2 127.0.0.1 1883 >/dev/null 2>/dev/null || true
sleep 0.5
if _broker_alive && _sanity; then
    _ok "broker survives PUBLISH with QoS=3 (MQTT §3.3.1.2)"
else
    _fail "broker crashed after PUBLISH with QoS=3"
fi

# ── summary ───────────────────────────────────────────────────────────────────
echo ""
echo "Results: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
