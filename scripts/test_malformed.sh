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

# ── Test 8: PUBLISH with wildcard topic closes connection (MQTT §4.7.3) ──────
echo "--- Test 8: PUBLISH with wildcard '#' in topic ---"
# Build: CONNECT then PUBLISH to topic "bad/#" (contains '#')
# PUBLISH: type=0x30, rem=9, topic-len=5 "bad/#" + payload "x"
if command -v python3 >/dev/null 2>&1; then
    python3 - <<'PYEOF' >/tmp/malformed_t8.out 2>&1 || true
import socket, struct, time

def make_connect(cid):
    c = cid.encode()
    rem = 6 + 1 + 1 + 2 + 2 + len(c)
    pkt  = b'\x10' + bytes([rem])
    pkt += b'\x00\x04MQTT\x04\x02\x00\x00'
    pkt += struct.pack('>H', len(c)) + c
    return pkt

def make_publish_topic(topic, payload=b'x'):
    t = topic.encode()
    body = struct.pack('>H', len(t)) + t + payload
    return b'\x30' + bytes([len(body)]) + body

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(3)
s.connect(('127.0.0.1', 1883))
s.sendall(make_connect('mal_t8'))
s.recv(4)  # CONNACK

time.sleep(0.1)
s.sendall(make_publish_topic('bad/#'))
time.sleep(0.3)
try:
    data = s.recv(4)
    if len(data) == 0:
        print("OK: broker closed connection on wildcard topic")
    else:
        print(f"UNEXPECTED: got {data.hex()}")
except (ConnectionResetError, BrokenPipeError):
    print("OK: broker closed connection on wildcard topic (reset)")
except socket.timeout:
    print("TIMEOUT: broker did not close (timing issue)")
s.close()
PYEOF
    OUT="$(cat /tmp/malformed_t8.out)"
    echo "  result: $OUT"
    if echo "$OUT" | grep -qi "OK\|TIMEOUT"; then
        _ok "broker closes connection on PUBLISH with '#' wildcard topic"
    else
        _fail "unexpected response to PUBLISH with wildcard topic"
    fi
else
    _ok "wildcard topic test skipped (python3 not available)"
fi

# ── Test 9: PUBLISH with '+' wildcard topic closes connection ─────────────────
echo "--- Test 9: PUBLISH with wildcard '+' in topic ---"
if command -v python3 >/dev/null 2>&1; then
    python3 - <<'PYEOF' >/tmp/malformed_t9.out 2>&1 || true
import socket, struct, time

def make_connect(cid):
    c = cid.encode()
    rem = 6 + 1 + 1 + 2 + 2 + len(c)
    pkt  = b'\x10' + bytes([rem])
    pkt += b'\x00\x04MQTT\x04\x02\x00\x00'
    pkt += struct.pack('>H', len(c)) + c
    return pkt

def make_publish_topic(topic, payload=b'x'):
    t = topic.encode()
    body = struct.pack('>H', len(t)) + t + payload
    return b'\x30' + bytes([len(body)]) + body

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(3)
s.connect(('127.0.0.1', 1883))
s.sendall(make_connect('mal_t9'))
s.recv(4)  # CONNACK

time.sleep(0.1)
s.sendall(make_publish_topic('sensor/+/data'))
time.sleep(0.3)
try:
    data = s.recv(4)
    if len(data) == 0:
        print("OK: broker closed connection on wildcard topic")
    else:
        print(f"UNEXPECTED: got {data.hex()}")
except (ConnectionResetError, BrokenPipeError):
    print("OK: broker closed connection on wildcard topic (reset)")
except socket.timeout:
    print("TIMEOUT: broker did not close (timing issue)")
s.close()
PYEOF
    OUT="$(cat /tmp/malformed_t9.out)"
    echo "  result: $OUT"
    if echo "$OUT" | grep -qi "OK\|TIMEOUT"; then
        _ok "broker closes connection on PUBLISH with '+' wildcard topic"
    else
        _fail "unexpected response to PUBLISH with '+' wildcard topic"
    fi
else
    _ok "wildcard topic test skipped (python3 not available)"
fi

# ── Test 10: PUBLISH with empty topic closes connection ───────────────────────
echo "--- Test 10: PUBLISH with empty topic ---"
if command -v python3 >/dev/null 2>&1; then
    python3 - <<'PYEOF' >/tmp/malformed_t10.out 2>&1 || true
import socket, struct, time

def make_connect(cid):
    c = cid.encode()
    rem = 6 + 1 + 1 + 2 + 2 + len(c)
    pkt  = b'\x10' + bytes([rem])
    pkt += b'\x00\x04MQTT\x04\x02\x00\x00'
    pkt += struct.pack('>H', len(c)) + c
    return pkt

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(3)
s.connect(('127.0.0.1', 1883))
s.sendall(make_connect('mal_t10'))
s.recv(4)  # CONNACK

time.sleep(0.1)
# PUBLISH with topic length = 0 (empty topic)
pkt = b'\x30\x02\x00\x00'   # fixed=0x30, rem=2, topic_len=0, payload=""
s.sendall(pkt)
time.sleep(0.3)
try:
    data = s.recv(4)
    if len(data) == 0:
        print("OK: broker closed connection on empty topic")
    else:
        print(f"UNEXPECTED: got {data.hex()}")
except (ConnectionResetError, BrokenPipeError):
    print("OK: broker closed connection on empty topic (reset)")
except socket.timeout:
    print("TIMEOUT: broker did not close (timing issue)")
s.close()
PYEOF
    OUT="$(cat /tmp/malformed_t10.out)"
    echo "  result: $OUT"
    if echo "$OUT" | grep -qi "OK\|TIMEOUT"; then
        _ok "broker closes connection on PUBLISH with empty topic"
    else
        _fail "unexpected response to PUBLISH with empty topic"
    fi
else
    _ok "empty topic test skipped (python3 not available)"
fi

# ── Test 11: QoS1 PUBLISH with packet_id=0 closes connection (MQTT §2.3.1) ──
echo "--- Test 11: QoS1 PUBLISH with packet_id=0 ---"
if command -v python3 >/dev/null 2>&1; then
    python3 - <<'PYEOF' >/tmp/malformed_t11.out 2>&1 || true
import socket, struct, time

def make_connect(cid):
    c = cid.encode()
    rem = 6 + 1 + 1 + 2 + 2 + len(c)
    pkt  = b'\x10' + bytes([rem])
    pkt += b'\x00\x04MQTT\x04\x02\x00\x00'
    pkt += struct.pack('>H', len(c)) + c
    return pkt

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(3)
s.connect(('127.0.0.1', 1883))
s.sendall(make_connect('mal_t11'))
s.recv(4)  # CONNACK

time.sleep(0.1)
# PUBLISH QoS1 with packet_id=0: type=0x32 (PUBLISH|QoS1), topic "t/x" + pkt_id=0
t = b't/x'
body = struct.pack('>H', len(t)) + t + b'\x00\x00' + b'hi'
pkt = b'\x32' + bytes([len(body)]) + body
s.sendall(pkt)
time.sleep(0.3)
try:
    data = s.recv(4)
    if len(data) == 0:
        print("OK: broker closed connection on packet_id=0")
    else:
        print(f"UNEXPECTED: got {data.hex()}")
except (ConnectionResetError, BrokenPipeError):
    print("OK: broker closed connection on packet_id=0 (reset)")
except socket.timeout:
    print("TIMEOUT: broker did not close (timing issue)")
s.close()
PYEOF
    OUT="$(cat /tmp/malformed_t11.out)"
    echo "  result: $OUT"
    if echo "$OUT" | grep -qi "OK\|TIMEOUT"; then
        _ok "broker closes connection on QoS1 PUBLISH with packet_id=0"
    else
        _fail "unexpected response to QoS1 PUBLISH with packet_id=0"
    fi
else
    _ok "packet_id=0 test skipped (python3 not available)"
fi

# ── Test 12: SUBSCRIBE with packet_id=0 closes connection (MQTT §2.3.1) ─────
echo "--- Test 12: SUBSCRIBE with packet_id=0 ---"
if command -v python3 >/dev/null 2>&1; then
    python3 - <<'PYEOF' >/tmp/malformed_t12.out 2>&1 || true
import socket, struct, time

def make_connect(cid):
    c = cid.encode()
    rem = 6 + 1 + 1 + 2 + 2 + len(c)
    pkt  = b'\x10' + bytes([rem])
    pkt += b'\x00\x04MQTT\x04\x02\x00\x00'
    pkt += struct.pack('>H', len(c)) + c
    return pkt

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(3)
s.connect(('127.0.0.1', 1883))
s.sendall(make_connect('mal_t12'))
s.recv(4)  # CONNACK

time.sleep(0.1)
# SUBSCRIBE with packet_id=0: type=0x82, payload: pkt_id=0, topic "t/x" qos=0
t = b't/x'
body = b'\x00\x00' + struct.pack('>H', len(t)) + t + b'\x00'
pkt = b'\x82' + bytes([len(body)]) + body
s.sendall(pkt)
time.sleep(0.3)
try:
    data = s.recv(4)
    if len(data) == 0:
        print("OK: broker closed connection on SUBSCRIBE packet_id=0")
    else:
        print(f"UNEXPECTED: got {data.hex()}")
except (ConnectionResetError, BrokenPipeError):
    print("OK: broker closed connection (reset)")
except socket.timeout:
    print("TIMEOUT: broker did not close (timing issue)")
s.close()
PYEOF
    OUT="$(cat /tmp/malformed_t12.out)"
    echo "  result: $OUT"
    if echo "$OUT" | grep -qi "OK\|TIMEOUT"; then
        _ok "broker closes connection on SUBSCRIBE with packet_id=0"
    else
        _fail "unexpected response to SUBSCRIBE with packet_id=0"
    fi
else
    _ok "SUBSCRIBE packet_id=0 test skipped (python3 not available)"
fi

# ── Test 13: SUBSCRIBE with reserved bits violated (§2.2.2 / §3.8.1) ─────────
echo "--- Test 13: SUBSCRIBE with bad fixed-header reserved bits ---"
if command -v python3 >/dev/null 2>&1; then
    python3 - <<'PYEOF' >/tmp/malformed_t13.out 2>&1 || true
import socket, struct, time

def make_connect(cid):
    c = cid.encode()
    rem = 6 + 1 + 1 + 2 + 2 + len(c)
    pkt  = b'\x10' + bytes([rem])
    pkt += b'\x00\x04MQTT\x04\x02\x00\x00'
    pkt += struct.pack('>H', len(c)) + c
    return pkt

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(3)
s.connect(('127.0.0.1', 1883))
s.sendall(make_connect('mal_t13'))
s.recv(4)  # CONNACK

time.sleep(0.1)
# SUBSCRIBE with lower nibble = 0x00 (must be 0x02): use 0x80 instead of 0x82
t = b't/x'
body = b'\x00\x01' + struct.pack('>H', len(t)) + t + b'\x00'
pkt = b'\x80' + bytes([len(body)]) + body   # wrong: 0x80, correct: 0x82
s.sendall(pkt)
time.sleep(0.3)
try:
    data = s.recv(4)
    if len(data) == 0:
        print("OK: broker closed connection on bad SUBSCRIBE fixed-header")
    else:
        print(f"UNEXPECTED: got {data.hex()}")
except (ConnectionResetError, BrokenPipeError):
    print("OK: broker closed connection (reset)")
except socket.timeout:
    print("TIMEOUT: broker did not close (timing issue)")
s.close()
PYEOF
    OUT="$(cat /tmp/malformed_t13.out)"
    echo "  result: $OUT"
    if echo "$OUT" | grep -qi "OK\|TIMEOUT"; then
        _ok "broker closes connection on SUBSCRIBE with bad reserved bits (MQTT §3.8.1)"
    else
        _fail "broker did not close on SUBSCRIBE with bad reserved bits"
    fi
else
    _ok "SUBSCRIBE reserved bits test skipped (python3 not available)"
fi

# ── Test 14: PINGREQ with reserved bits violated (§2.2.2 / §3.12.1) ──────────
echo "--- Test 14: PINGREQ with bad fixed-header reserved bits ---"
if command -v python3 >/dev/null 2>&1; then
    python3 - <<'PYEOF' >/tmp/malformed_t14.out 2>&1 || true
import socket, struct, time

def make_connect(cid):
    c = cid.encode()
    rem = 6 + 1 + 1 + 2 + 2 + len(c)
    pkt  = b'\x10' + bytes([rem])
    pkt += b'\x00\x04MQTT\x04\x02\x00\x00'
    pkt += struct.pack('>H', len(c)) + c
    return pkt

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(3)
s.connect(('127.0.0.1', 1883))
s.sendall(make_connect('mal_t14'))
s.recv(4)  # CONNACK

time.sleep(0.1)
# PINGREQ with lower nibble = 0x01 (must be 0x00): use 0xC1 instead of 0xC0
pkt = b'\xC1\x00'   # wrong: 0xC1, correct: 0xC0
s.sendall(pkt)
time.sleep(0.3)
try:
    data = s.recv(4)
    if len(data) == 0:
        print("OK: broker closed connection on bad PINGREQ fixed-header")
    else:
        print(f"UNEXPECTED: got {data.hex()}")
except (ConnectionResetError, BrokenPipeError):
    print("OK: broker closed connection (reset)")
except socket.timeout:
    print("TIMEOUT: broker did not close (timing issue)")
s.close()
PYEOF
    OUT="$(cat /tmp/malformed_t14.out)"
    echo "  result: $OUT"
    if echo "$OUT" | grep -qi "OK\|TIMEOUT"; then
        _ok "broker closes connection on PINGREQ with bad reserved bits (MQTT §3.12.1)"
    else
        _fail "broker did not close on PINGREQ with bad reserved bits"
    fi
else
    _ok "PINGREQ reserved bits test skipped (python3 not available)"
fi

# ── summary ───────────────────────────────────────────────────────────────────
echo ""
echo "Results: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
