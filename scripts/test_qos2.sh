#!/usr/bin/env bash
# Integration test: QoS 2 publish/subscribe
# Verifies the full PUBLISH→PUBREC→PUBREL→PUBCOMP exchange
# Run from repo root: ./scripts/test_qos2.sh
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
"$BROKER" >/tmp/mqtt_qos2_broker.log 2>&1 &
BROKER_PID=$!
sleep 0.5

if ! kill -0 "$BROKER_PID" 2>/dev/null; then
    echo "[setup] broker failed to start:" >&2
    cat /tmp/mqtt_qos2_broker.log >&2
    exit 1
fi
echo "[setup] broker PID=$BROKER_PID"
echo ""

# ── Test 1: basic QoS 2 delivery ─────────────────────────────────────────────
echo "--- Test 1: QoS 2 pub/sub basic ---"
"$CLI" sub -t "qos2/basic" -q 2 >/tmp/qos2_t1.out 2>/dev/null &
SUB=$!; sleep 0.3
"$CLI" pub -t "qos2/basic" -m "hello-qos2" -q 2 >/dev/null
sleep 0.5
kill $SUB 2>/dev/null; wait $SUB 2>/dev/null || true
_assert_contains "QoS2 message delivered" "qos2/basic hello-qos2" "$(cat /tmp/qos2_t1.out)"

# ── Test 2: exactly-once — no duplicates ─────────────────────────────────────
echo "--- Test 2: QoS 2 exactly-once (no duplicates) ---"
"$CLI" sub -t "qos2/dedup" -q 2 >/tmp/qos2_t2.out 2>/dev/null &
SUB=$!; sleep 0.3
"$CLI" pub -t "qos2/dedup" -m "unique" -q 2 >/dev/null
sleep 0.5
kill $SUB 2>/dev/null; wait $SUB 2>/dev/null || true
COUNT=$(grep -c "qos2/dedup unique" /tmp/qos2_t2.out 2>/dev/null || echo "0")
if [ "$COUNT" -eq 1 ]; then
    _ok "QoS2 exactly-once: message delivered exactly 1 time"
elif [ "$COUNT" -eq 0 ]; then
    _fail "QoS2 exactly-once: message not delivered"
else
    _fail "QoS2 exactly-once: message duplicated ($COUNT times)"
fi

# ── Test 3: QoS 2 fan-out to multiple subscribers ────────────────────────────
echo "--- Test 3: QoS 2 fan-out ---"
"$CLI" sub -t "qos2/fan" -q 2 >/tmp/qos2_t3a.out 2>/dev/null &
P1=$!
"$CLI" sub -t "qos2/fan" -q 2 >/tmp/qos2_t3b.out 2>/dev/null &
P2=$!
sleep 0.3
"$CLI" pub -t "qos2/fan" -m "broadcast2" -q 2 >/dev/null
sleep 0.5
kill $P1 $P2 2>/dev/null; wait $P1 $P2 2>/dev/null || true
_assert_contains "fan-out sub1" "qos2/fan broadcast2" "$(cat /tmp/qos2_t3a.out)"
_assert_contains "fan-out sub2" "qos2/fan broadcast2" "$(cat /tmp/qos2_t3b.out)"

# ── Test 4: QoS 2 retained message ───────────────────────────────────────────
echo "--- Test 4: QoS 2 retained ---"
"$CLI" pub -t "qos2/ret" -m "keep2" -q 2 -r >/dev/null
sleep 0.3
OUT="$(timeout 2 "$CLI" sub -t "qos2/ret" -q 2 2>/dev/null || true)"
_assert_contains "QoS2 retained delivered on subscribe" "qos2/ret keep2" "$OUT"

# clear it
"$CLI" pub -t "qos2/ret" -m "" -q 2 -r >/dev/null
sleep 0.3
OUT="$(timeout 2 "$CLI" sub -t "qos2/ret" -q 2 2>/dev/null || true)"
_assert_empty "QoS2 retained cleared" "$OUT"

# ── Test 5: QoS downgrade (pub QoS2, sub QoS0) ───────────────────────────────
echo "--- Test 5: QoS downgrade pub=2 sub=0 ---"
"$CLI" sub -t "qos2/down" -q 0 >/tmp/qos2_t5.out 2>/dev/null &
SUB=$!; sleep 0.3
"$CLI" pub -t "qos2/down" -m "downgraded" -q 2 >/dev/null
sleep 0.5
kill $SUB 2>/dev/null; wait $SUB 2>/dev/null || true
_assert_contains "QoS2→0 message still delivered" "qos2/down downgraded" \
    "$(cat /tmp/qos2_t5.out)"

# ── Test 6: multiple sequential QoS 2 messages ───────────────────────────────
echo "--- Test 6: multiple sequential QoS 2 messages ---"
"$CLI" sub -t "qos2/seq" -q 2 >/tmp/qos2_t6.out 2>/dev/null &
SUB=$!; sleep 0.3
for i in 1 2 3 4 5; do
    "$CLI" pub -t "qos2/seq" -m "msg$i" -q 2 >/dev/null
    sleep 0.1
done
sleep 0.5
kill $SUB 2>/dev/null; wait $SUB 2>/dev/null || true
OUT="$(cat /tmp/qos2_t6.out)"
ALL_OK=1
for i in 1 2 3 4 5; do
    if ! echo "$OUT" | grep -qF "qos2/seq msg$i"; then
        ALL_OK=0
    fi
done
if [ $ALL_OK -eq 1 ]; then
    _ok "all 5 sequential QoS2 messages delivered"
else
    _fail "some sequential QoS2 messages missing"
    echo "         got: $OUT"
fi

# ── summary ───────────────────────────────────────────────────────────────────
echo ""
echo "Results: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
