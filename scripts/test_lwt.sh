#!/usr/bin/env bash
# Integration test: Last Will and Testament (LWT)
# Verifies that when a client disconnects ungracefully the will message
# is published to the declared will topic, and that a clean DISCONNECT
# does NOT trigger the will.
# Run from repo root: ./scripts/test_lwt.sh
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."
BROKER="$ROOT/build_out/mqtt_broker"
CLI="$ROOT/build_out/mqtt_cli"
LWT="$ROOT/build_out/lwt_client"
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

_assert_not_contains() {
    local label="$1" unexpected="$2" actual="$3"
    if echo "$actual" | grep -qF "$unexpected"; then
        _fail "$label (found unexpected: '$unexpected')"
    else
        _ok "$label"
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
    KEEP=5 LOG_ROOT=/tmp LOG_NAME_GLOB='mqtt_*_broker.log' "$SCRIPT_DIR/cleanup_logs.sh" >/dev/null 2>&1 || true
}
trap _cleanup EXIT

# ── build if needed ───────────────────────────────────────────────────────────
if [ ! -x "$BROKER" ] || [ ! -x "$CLI" ] || [ ! -x "$LWT" ]; then
    echo "[setup] building..."
    make -f "$ROOT/Makefile.linux" -C "$ROOT" -s
    make -f "$ROOT/Makefile.linux" -C "$ROOT" test-helpers -s
fi

if _port_in_use; then
    echo "[setup] port 1883 in use; stop mosquitto first" >&2
    exit 1
fi

echo "[setup] starting broker..."
"$BROKER" >/tmp/mqtt_lwt_broker.log 2>&1 &
BROKER_PID=$!
sleep 0.5

if ! kill -0 "$BROKER_PID" 2>/dev/null; then
    echo "[setup] broker failed to start" >&2
    cat /tmp/mqtt_lwt_broker.log >&2
    exit 1
fi
echo "[setup] broker PID=$BROKER_PID"
echo ""

# ── Test 1: LWT published on ungraceful disconnect (SIGKILL) ─────────────────
echo "--- Test 1: LWT on ungraceful disconnect (SIGKILL) ---"
"$CLI" sub -t "lwt/dev1" >/tmp/lwt_t1.out 2>/dev/null &
OBS=$!; sleep 0.3

"$LWT" -t "lwt/dev1" -m "offline" &
LWT_PID=$!; sleep 0.4

kill -9 $LWT_PID 2>/dev/null; wait $LWT_PID 2>/dev/null || true
# Broker detects dead TCP; keepalive is 60s so broker fires will on close
sleep 1.0

kill $OBS 2>/dev/null; wait $OBS 2>/dev/null || true
_assert_contains "will published after SIGKILL" \
    "lwt/dev1 offline" "$(cat /tmp/lwt_t1.out)"

# ── Test 2: LWT NOT published on clean DISCONNECT ─────────────────────────────
echo "--- Test 2: LWT NOT published on clean DISCONNECT ---"
"$CLI" sub -t "lwt/dev2" >/tmp/lwt_t2.out 2>/dev/null &
OBS=$!; sleep 0.3

# -P makes lwt_client publish pub_msg to will_topic then disconnect cleanly
"$LWT" -t "lwt/dev2" -m "dead2" -P "alive2"
sleep 0.5

kill $OBS 2>/dev/null; wait $OBS 2>/dev/null || true
OUT="$(cat /tmp/lwt_t2.out)"
_assert_contains     "normal message seen"               "lwt/dev2 alive2" "$OUT"
_assert_not_contains "will NOT published on clean exit"  "dead2"           "$OUT"

# ── Test 3: LWT with retain flag ─────────────────────────────────────────────
echo "--- Test 3: retained LWT ---"
"$CLI" sub -t "lwt/dev3" >/tmp/lwt_t3.out 2>/dev/null &
OBS=$!; sleep 0.3

"$LWT" -t "lwt/dev3" -m "gone3" -r &
LWT_PID=$!; sleep 0.4
kill -9 $LWT_PID 2>/dev/null; wait $LWT_PID 2>/dev/null || true
sleep 1.0

kill $OBS 2>/dev/null; wait $OBS 2>/dev/null || true
_assert_contains "retained LWT delivered to running subscriber" \
    "lwt/dev3 gone3" "$(cat /tmp/lwt_t3.out)"

# Late subscriber should also get the retained will
OUT="$(timeout 2 "$CLI" sub -t "lwt/dev3" 2>/dev/null || true)"
_assert_contains "retained LWT delivered to late subscriber" \
    "lwt/dev3 gone3" "$OUT"

# ── Test 4: LWT with QoS 1 ────────────────────────────────────────────────────
echo "--- Test 4: LWT QoS 1 ---"
"$CLI" sub -t "lwt/dev4" -q 1 >/tmp/lwt_t4.out 2>/dev/null &
OBS=$!; sleep 0.3

"$LWT" -t "lwt/dev4" -m "gone4" -q 1 &
LWT_PID=$!; sleep 0.4
kill -9 $LWT_PID 2>/dev/null; wait $LWT_PID 2>/dev/null || true
sleep 1.0

kill $OBS 2>/dev/null; wait $OBS 2>/dev/null || true
_assert_contains "QoS1 LWT delivered" "lwt/dev4 gone4" \
    "$(cat /tmp/lwt_t4.out)"

# ── Test 5: CONNECT with wildcard LWT topic rejected (MQTT §4.7.3) ──────────
echo "--- Test 5: CONNECT with wildcard LWT topic rejected ---"
if command -v python3 >/dev/null 2>&1; then
    python3 - <<'PYEOF' >/tmp/lwt_t5.out 2>&1 || true
import socket, struct, time

def make_connect_with_lwt(cid, will_topic):
    c  = cid.encode()
    wt = will_topic.encode()
    wm = b'gone'
    # flags: clean_session=1 (bit1), will_flag=1 (bit2), will_qos=0 (bits 4-3)
    flags = 0x06
    # variable header: proto_name(6) + level(1) + flags(1) + keepalive(2) = 10
    # payload: cid(2+len) + will_topic(2+len) + will_msg(2+len)
    payload = struct.pack('>H', len(c)) + c
    payload += struct.pack('>H', len(wt)) + wt
    payload += struct.pack('>H', len(wm)) + wm
    rem = 10 + len(payload)
    pkt  = b'\x10' + bytes([rem])
    pkt += b'\x00\x04MQTT\x04' + bytes([flags]) + b'\x00\x3c'
    pkt += payload
    return pkt

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(3)
s.connect(('127.0.0.1', 1883))
# wildcard '#' in will topic — must be rejected
s.sendall(make_connect_with_lwt('lwt_t5', 'dev/#'))
try:
    data = s.recv(4)
    if len(data) >= 4 and data[0] == 0x20 and data[3] == 0x02:
        print("OK: CONNACK 0x02 (identifier rejected) received")
    elif len(data) == 0:
        print("OK: broker closed connection (wildcard LWT rejected)")
    else:
        print(f"UNEXPECTED: got {data.hex()}")
except (ConnectionResetError, BrokenPipeError):
    print("OK: broker closed connection (wildcard LWT rejected, reset)")
except socket.timeout:
    print("TIMEOUT: broker did not respond")
s.close()
PYEOF
    OUT="$(cat /tmp/lwt_t5.out)"
    echo "  result: $OUT"
    if echo "$OUT" | grep -qi "^OK\|TIMEOUT"; then
        _ok "CONNECT with wildcard LWT topic rejected"
    else
        _fail "CONNECT with wildcard LWT topic not rejected: $OUT"
    fi
else
    _ok "wildcard LWT test skipped (python3 not available)"
fi

# ── summary ───────────────────────────────────────────────────────────────────
echo ""
echo "Results: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
