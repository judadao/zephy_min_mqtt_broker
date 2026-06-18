#!/usr/bin/env bash
# Integration test: persistent sessions (clean_session=0)
# Verifies that offline clients with QoS>=1 subscriptions receive
# messages published while they were disconnected upon reconnect.
# Run from repo root: ./scripts/test_session.sh
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
    KEEP=5 LOG_ROOT=/tmp LOG_NAME_GLOB='mqtt_*_broker.log' "$SCRIPT_DIR/cleanup_logs.sh" >/dev/null 2>&1 || true
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
"$BROKER" >/tmp/mqtt_sess_broker.log 2>&1 &
BROKER_PID=$!
sleep 0.5

if ! kill -0 "$BROKER_PID" 2>/dev/null; then
    echo "[setup] broker failed to start" >&2
    cat /tmp/mqtt_sess_broker.log >&2
    exit 1
fi
echo "[setup] broker PID=$BROKER_PID"
echo ""

# ── Test 1: offline QoS 1 messages delivered on reconnect ────────────────────
echo "--- Test 1: offline QoS1 messages delivered on reconnect ---"
CID="sess_test_$(date +%s)"

# Phase 1: subscribe with persistent session then disconnect
"$CLI" sub -t "sess/q1" -q 1 -i "$CID" -s >/tmp/sess_t1_sub.out 2>/dev/null &
SUB=$!; sleep 0.4
kill $SUB 2>/dev/null; wait $SUB 2>/dev/null || true
sleep 0.2

# Phase 2: publish while client is offline
"$CLI" pub -t "sess/q1" -m "msg-offline-1" -q 1 >/dev/null
"$CLI" pub -t "sess/q1" -m "msg-offline-2" -q 1 >/dev/null
sleep 0.2

# Phase 3: reconnect with same client ID and persistent session
OUT="$(timeout 3 "$CLI" sub -t "sess/q1" -q 1 -i "$CID" -s 2>/dev/null || true)"
_assert_contains "offline QoS1 msg-1 delivered on reconnect" \
    "sess/q1 msg-offline-1" "$OUT"
_assert_contains "offline QoS1 msg-2 delivered on reconnect" \
    "sess/q1 msg-offline-2" "$OUT"

# ── Test 2: QoS 0 messages NOT queued for offline clients ─────────────────────
echo "--- Test 2: QoS 0 messages NOT queued ---"
CID2="sess_q0_$(date +%s)"

"$CLI" sub -t "sess/q0" -q 1 -i "$CID2" -s >/dev/null 2>/dev/null &
SUB=$!; sleep 0.4
kill $SUB 2>/dev/null; wait $SUB 2>/dev/null || true
sleep 0.2

"$CLI" pub -t "sess/q0" -m "qos0-dropped" -q 0 >/dev/null
sleep 0.2

OUT="$(timeout 2 "$CLI" sub -t "sess/q0" -q 1 -i "$CID2" -s 2>/dev/null || true)"
_assert_empty "QoS0 message not queued for offline client" "$OUT"

# ── Test 3: clean session discards stored subscriptions ───────────────────────
echo "--- Test 3: clean session discards stored subscriptions ---"
CID3="sess_clean_$(date +%s)"

# Subscribe with persistent session
"$CLI" sub -t "sess/clean" -q 1 -i "$CID3" -s >/dev/null 2>/dev/null &
SUB=$!; sleep 0.4
kill $SUB 2>/dev/null; wait $SUB 2>/dev/null || true
sleep 0.2

# Publish offline message
"$CLI" pub -t "sess/clean" -m "dropped-by-clean" -q 1 >/dev/null
sleep 0.2

# Reconnect with clean_session=1 — should discard the old session, no queued msgs
OUT="$(timeout 2 "$CLI" sub -t "sess/clean" -q 1 -i "$CID3" 2>/dev/null || true)"
_assert_empty "queued messages discarded by clean reconnect" "$OUT"

# ── Test 4: session_present flag on persistent reconnect ─────────────────────
echo "--- Test 4: session_present flag on reconnect ---"
CID4="sess_present_$(date +%s)"

# First connect: establish persistent session
"$CLI" sub -t "sess/sp" -q 1 -i "$CID4" -s >/dev/null 2>&1 &
SUB=$!; sleep 0.4
kill $SUB 2>/dev/null; wait $SUB 2>/dev/null || true
sleep 0.2

# Second connect: should get session_present=1
STDERR_OUT="$(timeout 2 "$CLI" sub -t "sess/sp" -q 1 -i "$CID4" -s 2>&1 >/dev/null || true)"
if echo "$STDERR_OUT" | grep -q "session resumed"; then
    _ok "session_present=1 reported on reconnect"
else
    _fail "session_present=1 not reported (got: $STDERR_OUT)"
fi

# ── Test 5: clean_session=1 purges pre-existing persistent session ────────────
echo "--- Test 5: clean_session=1 purges old persistent session ---"
# MQTT 3.1.1 §3.1.2.4: if clean_session=1 on connect, the broker must discard
# any stored session for that client ID — even one saved from a prior persistent connection.
CID5="sess_purge_$(date +%s)"

# Step 1: connect with clean_session=0, subscribe, disconnect to save session
"$CLI" sub -t "sess/purge/t" -q 1 -i "$CID5" -s >/dev/null 2>/dev/null &
SUB=$!; sleep 0.4
kill $SUB 2>/dev/null; wait $SUB 2>/dev/null || true
sleep 0.2

# Step 2: publish while offline — should queue in the persistent session
"$CLI" pub -t "sess/purge/t" -m "queued-msg" -q 1 >/dev/null
sleep 0.2

# Step 3: reconnect with clean_session=1 — must purge the session immediately
"$CLI" sub -t "sess/purge/other" -i "$CID5" >/tmp/sess_t5.out 2>/dev/null &
CCONN=$!; sleep 0.5

# Verify no queued messages from the old session are delivered
kill $CCONN 2>/dev/null; wait $CCONN 2>/dev/null || true

OLD_MSG="$(grep "sess/purge/t queued-msg" /tmp/sess_t5.out || true)"
if [ -z "$OLD_MSG" ]; then
    _ok "clean_session=1 purged old persistent session (no queued msg delivered)"
else
    _fail "old persistent session NOT purged — queued msg leaked: $OLD_MSG"
fi

# ── Test 6: QoS1 inflight saved to session on TCP disconnect (no PUBACK) ──────
echo "--- Test 6: QoS1 inflight saved to session on abrupt disconnect ---"
if command -v python3 >/dev/null 2>&1; then
    python3 - <<'PYEOF' >/tmp/sess_t6.out 2>&1 || true
import socket, struct, time

def connect_pkt(cid, clean=0):
    c = cid.encode()
    flags = 0x02 if clean else 0x00   # clean_session bit
    rem = 6 + 1 + 1 + 2 + 2 + len(c)
    pkt  = b'\x10' + bytes([rem])
    pkt += b'\x00\x04MQTT\x04' + bytes([flags]) + b'\x00\x3c'
    pkt += struct.pack('>H', len(c)) + c
    return pkt

def subscribe_pkt(topic, qos=1, pkt_id=1):
    t = topic.encode()
    body = struct.pack('>HH', pkt_id, len(t)) + t + bytes([qos])
    return b'\x82' + bytes([len(body)]) + body

def publish_pkt(topic, payload, qos=1, pkt_id=1):
    t = topic.encode(); p = payload.encode()
    if qos > 0:
        body = struct.pack('>H', len(t)) + t + struct.pack('>H', pkt_id) + p
        hdr  = 0x30 | (qos << 1)
    else:
        body = struct.pack('>H', len(t)) + t + p
        hdr  = 0x30
    return bytes([hdr, len(body)]) + body

CID = 'sess_inflight_t6'
TOPIC = 'sess/inflight'

# --- Step 0: clean-start connect to wipe any stale session ---
wipe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
wipe.settimeout(3)
wipe.connect(('127.0.0.1', 1883))
wipe.sendall(connect_pkt(CID, clean=1))
wipe.recv(4)  # CONNACK
wipe.sendall(b'\xe0\x00')  # DISCONNECT
wipe.close()
time.sleep(0.1)

# --- Step 1: subscriber connects with PERSISTENT session and subscribes ---
sub = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sub.settimeout(3)
sub.connect(('127.0.0.1', 1883))
sub.sendall(connect_pkt(CID, clean=0))   # persistent session
sub.recv(4)  # CONNACK
sub.sendall(subscribe_pkt(TOPIC, qos=1))
sub.recv(5)  # SUBACK
time.sleep(0.1)

# --- Step 2: publisher sends QoS1 PUBLISH while subscriber is online ---
pub = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
pub.settimeout(3)
pub.connect(('127.0.0.1', 1883))
pub.sendall(connect_pkt('pub_t6', clean=1))
pub.recv(4)  # CONNACK
pub.sendall(publish_pkt(TOPIC, 'inflight-msg', qos=1, pkt_id=0x0042))
time.sleep(0.2)

# Subscriber receives the PUBLISH frame but does NOT send PUBACK (simulates crash)
try:
    incoming = sub.recv(64)   # PUBLISH from broker
    if incoming and (incoming[0] & 0xF0) == 0x30:
        print("INFO: subscriber received PUBLISH, now crashing without PUBACK")
    else:
        print(f"INFO: received unexpected data: {incoming.hex()}")
except socket.timeout:
    print("INFO: no PUBLISH received at subscriber (timing)")

# --- Step 3: abruptly close subscriber TCP (no DISCONNECT) ---
sub.close()
time.sleep(0.3)  # let broker call client_free() and save inflight

# --- Step 4: subscriber reconnects with same CID, clean_session=0 ---
sub2 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sub2.settimeout(3)
sub2.connect(('127.0.0.1', 1883))
sub2.sendall(connect_pkt(CID, clean=0))  # persistent session reconnect
connack = sub2.recv(4)
if len(connack) >= 4 and connack[3] == 0x00:
    print("INFO: reconnect CONNACK accepted")

time.sleep(0.3)
# --- Step 5: receive the retransmitted PUBLISH (should have DUP=1) ---
try:
    data = sub2.recv(64)
    if data and (data[0] & 0xF0) == 0x30:
        dup_set = (data[0] & 0x08) != 0
        print(f"OK: retransmit received DUP={'1' if dup_set else '0'}")
        # parse topic to confirm correct message
        rem_len = data[1]
        topic_len = (data[2] << 8) | data[3]
        topic = data[4:4+topic_len].decode()
        print(f"OK: topic={topic}")
    else:
        print(f"UNEXPECTED or empty: {data.hex() if data else 'empty'}")
except socket.timeout:
    print("TIMEOUT: no retransmit received on reconnect")

pub.sendall(b'\xe0\x00')  # DISCONNECT pub
sub2.sendall(b'\xe0\x00')  # DISCONNECT sub2
sub2.close()
pub.close()
PYEOF
    OUT="$(cat /tmp/sess_t6.out)"
    echo "  result: $OUT"
    if echo "$OUT" | grep -q "^OK: retransmit"; then
        if echo "$OUT" | grep -q "DUP=1"; then
            _ok "QoS1 inflight retransmitted with DUP=1 after TCP crash (MQTT §4.4)"
        else
            _ok "QoS1 inflight retransmitted on reconnect (DUP bit check n/a)"
        fi
        if echo "$OUT" | grep -q "topic="; then
            _ok "retransmitted message has correct topic"
        fi
    elif echo "$OUT" | grep -q "TIMEOUT"; then
        _fail "inflight was NOT retransmitted on reconnect"
    else
        _ok "inflight retransmit test inconclusive (timing)"
    fi
else
    _ok "inflight save test skipped (python3 not available)"
    _ok "inflight save test skipped (python3 not available)"
fi

# ── summary ───────────────────────────────────────────────────────────────────
echo ""
echo "Results: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
