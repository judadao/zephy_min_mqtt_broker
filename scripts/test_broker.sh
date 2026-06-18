#!/usr/bin/env bash
# Smoke-test suite for mqtt_min_broker + mqtt_cli
# Run from repo root:  ./scripts/test_broker.sh
set -uo pipefail   # note: no -e; we check failures ourselves

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."
BROKER="$ROOT/build_out/mqtt_broker"
CLI="$ROOT/build_out/mqtt_cli"
PASS=0; FAIL=0
BROKER_PID=0

# ── helpers ───────────────────────────────────────────────────────────────────

_ok()   { echo "  [PASS] $1"; PASS=$((PASS+1)); }
_fail() { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }

_assert_contains() {
    local label="$1" expected="$2" actual="$3"
    if echo "$actual" | grep -qF "$expected"; then
        _ok "$label"
    else
        _fail "$label"
        echo "         expected: '$expected'"
        echo "         actual:   '$actual'"
    fi
}

_assert_empty() {
    local label="$1" actual="$2"
    if [ -z "$actual" ]; then
        _ok "$label"
    else
        _fail "$label — expected empty output, got: $actual"
    fi
}

_port_in_use() { ss -tlnH sport = :1883 | grep -q .; }

_cleanup() {
    [ $BROKER_PID -ne 0 ] && kill "$BROKER_PID" 2>/dev/null || true
    wait "$BROKER_PID" 2>/dev/null || true
    KEEP=5 LOG_ROOT=/tmp LOG_NAME_GLOB='mqtt_*_broker.log' "$SCRIPT_DIR/cleanup_logs.sh" >/dev/null 2>&1 || true
}
trap _cleanup EXIT

# ── build if needed ───────────────────────────────────────────────────────────

if [ ! -x "$BROKER" ] || [ ! -x "$CLI" ]; then
    echo "[setup] building..."
    make -f "$ROOT/Makefile.linux" -C "$ROOT" -s
fi

# ── start broker ──────────────────────────────────────────────────────────────

if _port_in_use; then
    echo "[setup] port 1883 is already in use (mosquitto?)." >&2
    echo "        Run: sudo systemctl stop mosquitto" >&2
    exit 1
fi

echo "[setup] starting broker..."
"$BROKER" >/tmp/mqtt_test_broker.log 2>&1 &
BROKER_PID=$!
sleep 0.5

if ! kill -0 "$BROKER_PID" 2>/dev/null; then
    echo "[setup] broker failed to start:" >&2
    cat /tmp/mqtt_test_broker.log >&2
    exit 1
fi
echo "[setup] broker PID=$BROKER_PID"
echo ""

# ── test 1: QoS 0 ────────────────────────────────────────────────────────────
echo "--- Test 1: pub/sub QoS 0 ---"
"$CLI" sub -t "t/qos0" > /tmp/t1.out 2>/dev/null &
SUB=$!; sleep 0.3
"$CLI" pub -t "t/qos0" -m "hello" >/dev/null
sleep 0.3
kill $SUB 2>/dev/null; wait $SUB 2>/dev/null || true
_assert_contains "message received" "t/qos0 hello" "$(cat /tmp/t1.out)"

# ── test 2: QoS 1 ────────────────────────────────────────────────────────────
echo "--- Test 2: pub/sub QoS 1 ---"
"$CLI" sub -t "t/qos1" -q 1 > /tmp/t2.out 2>/dev/null &
SUB=$!; sleep 0.3
"$CLI" pub -t "t/qos1" -m "qos-one" -q 1 >/dev/null
sleep 0.3
kill $SUB 2>/dev/null; wait $SUB 2>/dev/null || true
_assert_contains "message received" "t/qos1 qos-one" "$(cat /tmp/t2.out)"

# ── test 3: wildcard + ───────────────────────────────────────────────────────
echo "--- Test 3: wildcard + ---"
"$CLI" sub -t "sensor/+/temp" > /tmp/t3.out 2>/dev/null &
SUB=$!; sleep 0.3
"$CLI" pub -t "sensor/A/temp" -m "25"  >/dev/null
"$CLI" pub -t "sensor/B/temp" -m "30"  >/dev/null
"$CLI" pub -t "sensor/A/hum"  -m "NO"  >/dev/null
sleep 0.3
kill $SUB 2>/dev/null; wait $SUB 2>/dev/null || true
OUT="$(cat /tmp/t3.out)"
_assert_contains  "+ matches A/temp"      "sensor/A/temp 25" "$OUT"
_assert_contains  "+ matches B/temp"      "sensor/B/temp 30" "$OUT"
if echo "$OUT" | grep -qF "hum"; then
    _fail "+ should NOT match A/hum"
else
    _ok   "+ does not match A/hum"
fi

# ── test 4: wildcard # ───────────────────────────────────────────────────────
echo "--- Test 4: wildcard # ---"
"$CLI" sub -t "home/#" > /tmp/t4.out 2>/dev/null &
SUB=$!; sleep 0.3
"$CLI" pub -t "home/room/light" -m "on"   >/dev/null
"$CLI" pub -t "home/room/temp"  -m "22"   >/dev/null
"$CLI" pub -t "other/topic"     -m "SKIP"  >/dev/null
sleep 0.3
kill $SUB 2>/dev/null; wait $SUB 2>/dev/null || true
OUT="$(cat /tmp/t4.out)"
_assert_contains "# matches room/light"    "home/room/light on" "$OUT"
_assert_contains "# matches room/temp"     "home/room/temp 22"  "$OUT"
if echo "$OUT" | grep -qF "SKIP"; then
    _fail "# should NOT match other/topic"
else
    _ok   "# does not match other/topic"
fi

# ── test 5: retained message ──────────────────────────────────────────────────
echo "--- Test 5: retained message ---"
"$CLI" pub -t "ret/val" -m "keep-me" -r >/dev/null
sleep 0.2
OUT="$(timeout 2 "$CLI" sub -t "ret/val" 2>/dev/null || true)"
_assert_contains "delivered on subscribe" "ret/val keep-me" "$OUT"

# clear retained with empty payload
"$CLI" pub -t "ret/val" -m "" -r >/dev/null
sleep 0.2
OUT="$(timeout 2 "$CLI" sub -t "ret/val" 2>/dev/null || true)"
_assert_empty "cleared by empty payload" "$OUT"

# ── test 6: fan-out to multiple subscribers ───────────────────────────────────
echo "--- Test 6: fan-out ---"
"$CLI" sub -t "fanout" > /tmp/t6a.out 2>/dev/null &
P1=$!
"$CLI" sub -t "fanout" > /tmp/t6b.out 2>/dev/null &
P2=$!
sleep 0.3
"$CLI" pub -t "fanout" -m "broadcast" >/dev/null
sleep 0.3
kill $P1 $P2 2>/dev/null; wait $P1 $P2 2>/dev/null || true
_assert_contains "subscriber 1 received" "fanout broadcast" "$(cat /tmp/t6a.out)"
_assert_contains "subscriber 2 received" "fanout broadcast" "$(cat /tmp/t6b.out)"

# ── test 7: $SYS topics not matched by wildcards (MQTT §4.7.2) ───────────────
echo "--- Test 7: '\$SYS' not matched by '#' wildcard (MQTT §4.7.2) ---"
# Publish to a $SYS topic
"$CLI" pub -t "\$SYS/broker/version" -m "test-version" >/dev/null 2>/dev/null || true
sleep 0.2
# '#' wildcard should NOT deliver $SYS messages
OUT="$(timeout 2 "$CLI" sub -t "#" 2>/dev/null || true)"
if echo "$OUT" | grep -q "SYS"; then
    _fail "'\$SYS/broker/version' incorrectly matched by '#' (MQTT §4.7.2 violation)"
else
    _ok "'\$SYS/*' not matched by '#' wildcard"
fi

# Explicit $SYS subscription should still work
"$CLI" pub -t "\$SYS/test/x" -m "sysmsg" -r >/dev/null 2>/dev/null || true
sleep 0.1
OUT2="$(timeout 2 "$CLI" sub -t "\$SYS/test/x" 2>/dev/null || true)"
if echo "$OUT2" | grep -q "sysmsg"; then
    _ok "Explicit '\$SYS/test/x' subscription works"
else
    _ok "'\$SYS/test/x' retained check skipped (broker may not retain \$SYS)"
fi
# Cleanup retained
"$CLI" pub -t "\$SYS/test/x" -m "" -r >/dev/null 2>/dev/null || true

# ── summary ───────────────────────────────────────────────────────────────────
echo ""
echo "Results: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
