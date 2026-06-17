#!/usr/bin/env bash
# P2P dynamic broker scale benchmark.
#
# Linux-only benchmark that starts N local P2P brokers, spreads a large number
# of subscriptions across them, publishes from broker 0 to remote subscribers,
# and reports end-to-end MQTT latency percentiles.
#
# Defaults target a 10k-subscription class test:
#   ./scripts/bench_p2p_scale.sh
#
# Useful knobs:
#   TOTAL_SUBS=10000 BROKER_COUNTS="2 5 10 20 50 100" MESSAGES=500 ./scripts/bench_p2p_scale.sh
#   TARGET_P95_MS=10 ./scripts/bench_p2p_scale.sh
#   STRICT_ESP32=1 ./scripts/bench_p2p_scale.sh
#
# STRICT_ESP32=1 keeps MQTT_MAX_CLIENTS=8 and uses exactly P2P_PEER_MAX_BENCH
# instead of auto-raising it to the tested broker count. Use
# P2P_PEER_MAX_BENCH=5 or 10 to compare ESP32-like peer budgets. By default,
# MQTT_MAX_CLIENTS stays 8 while P2P_PEER_MAX is kept at least 10 and is
# auto-raised to count-1 for larger tests. ROUTER_COUNT=0 means every broker
# in that test round is a router, which keeps the benchmark focused on network
# throughput instead of transient router-election convergence.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."
OUT="${OUT:-$ROOT/build_out/p2p_scale_bench}"

TOTAL_SUBS="${TOTAL_SUBS:-10000}"
BROKER_COUNTS="${BROKER_COUNTS:-2 5 10 20 50 100}"
MESSAGES="${MESSAGES:-300}"
TARGET_P95_MS="${TARGET_P95_MS:-10}"
MQTT_MAX_CLIENTS_BENCH="${MQTT_MAX_CLIENTS_BENCH:-8}"
P2P_PEER_MAX_BENCH="${P2P_PEER_MAX_BENCH:-10}"
ROUTER_COUNT="${ROUTER_COUNT:-0}"
STRICT_ESP32="${STRICT_ESP32:-0}"
EXTRA_CFLAGS="${EXTRA_CFLAGS:--DCONFIG_MQTT_P2P_STATIC_SEEDS_ONLY}"
BASE_MQTT_PORT="${BASE_MQTT_PORT:-19100}"
BASE_P2P_PORT="${BASE_P2P_PORT:-29100}"
BASE_DISCOVERY_PORT="${BASE_DISCOVERY_PORT:-30100}"
STARTUP_SEC="${STARTUP_SEC:-2}"
SYNC_SETTLE_SEC="${SYNC_SETTLE_SEC:-3}"

PASS_COUNT=""
PIDS=""
RESULTS_FILE="$OUT/results.csv"

_cleanup() {
    for pid in $PIDS; do
        kill "$pid" 2>/dev/null || true
    done
    for pid in $PIDS; do
        wait "$pid" 2>/dev/null || true
    done
    PIDS=""
    sleep 0.2
}
trap _cleanup EXIT

_port_in_use() {
    ss -tulnH "sport = :$1" 2>/dev/null | grep -q .
}

_require_free_ports() {
    local count="$1" discovery="$2"

    if _port_in_use "$discovery"; then
        echo "[setup] discovery port $discovery is already in use" >&2
        exit 1
    fi

    for ((i = 0; i < count; i++)); do
        local mqtt_port=$((BASE_MQTT_PORT + i))
        local p2p_port=$((BASE_P2P_PORT + i))
        if _port_in_use "$mqtt_port"; then
            echo "[setup] MQTT port $mqtt_port is already in use" >&2
            exit 1
        fi
        if _port_in_use "$p2p_port"; then
            echo "[setup] P2P port $p2p_port is already in use" >&2
            exit 1
        fi
    done
}

_ceil_div() {
    local n="$1" d="$2"
    echo $(((n + d - 1) / d))
}

_build_node() {
    local out="$1"
    local mqtt_port="$2"
    local p2p_port="$3"
    local discovery_port="$4"
    local topic_slots="$5"
    local remote_slots_per_node="$6"
    local peer_max="$7"
    local router_count="$8"

    gcc -Wall -Wextra -Wno-stringop-truncation -std=c11 -O2 -g -D_POSIX_C_SOURCE=200809L \
        $EXTRA_CFLAGS \
        -I"$ROOT/include" -I"$ROOT" \
        -DCONFIG_MQTT_P2P_DYNAMIC \
        -DMQTT_BROKER_PORT="$mqtt_port" \
        -DP2P_PORT="$p2p_port" \
        -DP2P_DISCOVERY_PORT="$discovery_port" \
        -DMQTT_MAX_CLIENTS="$MQTT_MAX_CLIENTS_BENCH" \
        -DTOPIC_MAX_SUBS="$topic_slots" \
        -DP2P_PEER_MAX="$peer_max" \
        -DP2P_ROUTER_COUNT="$router_count" \
        -DP2P_REMOTE_SUBS_PER_NODE="$remote_slots_per_node" \
        "$ROOT/src/broker.c" \
        "$ROOT/src/client.c" \
        "$ROOT/src/main.c" \
        "$ROOT/src/packet.c" \
        "$ROOT/src/session.c" \
        "$ROOT/src/topic.c" \
        "$ROOT/src/p2p_discover.c" \
        "$ROOT/src/p2p_election.c" \
        "$ROOT/src/p2p_peer.c" \
        "$ROOT/src/p2p_router.c" \
        "$ROOT/platform/posix/platform_posix.c" \
        -o "$out" -lpthread
}

_run_broker() {
    local bin="$1" log="$2" seeds="${3:-}"

    if command -v stdbuf >/dev/null 2>&1; then
        MQTT_P2P_PEERS="$seeds" stdbuf -oL -eL "$bin" > "$log" 2>&1 &
    else
        MQTT_P2P_PEERS="$seeds" "$bin" > "$log" 2>&1 &
    fi
    echo $!
}

_dump_count_logs() {
    local count="$1"

    for log in "$OUT"/broker_"$count"_*.log; do
        [ -f "$log" ] || continue
        echo "--- ${log##*/} ---" >&2
        tail -n 40 "$log" >&2
    done
}

_wait_for_mqtt_ports() {
    local ports_csv="$1"

    python3 - "$ports_csv" <<'PY'
import socket
import sys
import time

ports = [int(p) for p in sys.argv[1].split(",") if p]
deadline = time.monotonic() + 10.0
pending = set(ports)

while pending and time.monotonic() < deadline:
    for port in list(pending):
        try:
            sock = socket.create_connection(("127.0.0.1", port), timeout=0.2)
            sock.close()
            pending.remove(port)
        except OSError:
            pass
    if pending:
        time.sleep(0.1)

if pending:
    print(",".join(str(p) for p in sorted(pending)))
    sys.exit(1)
PY
}

_run_python_load() {
    local ports_csv="$1"
    local total_subs="$2"
    local messages="$3"
    local target_ms="$4"
    local sync_settle="$5"

    python3 - "$ports_csv" "$total_subs" "$messages" "$target_ms" "$sync_settle" <<'PY'
import select
import socket
import statistics
import sys
import time

ports = [int(p) for p in sys.argv[1].split(",") if p]
total_subs = int(sys.argv[2])
messages = int(sys.argv[3])
target_ms = float(sys.argv[4])
sync_settle = float(sys.argv[5])

HOST = "127.0.0.1"
KEEPALIVE = 60

MQTT_CONNECT = 0x10
MQTT_CONNACK = 0x20
MQTT_PUBLISH = 0x30
MQTT_SUBSCRIBE = 0x80
MQTT_SUBACK = 0x90
MQTT_DISCONNECT = 0xE0

def enc_rem(n):
    out = bytearray()
    while True:
        b = n % 128
        n //= 128
        if n:
            b |= 0x80
        out.append(b)
        if not n:
            return bytes(out)

def send_all(sock, data):
    sock.sendall(data)

def recv_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise RuntimeError("socket closed")
        buf.extend(chunk)
    return bytes(buf)

def recv_pkt(sock):
    first = recv_exact(sock, 1)[0]
    mult = 1
    rem = 0
    while True:
        b = recv_exact(sock, 1)[0]
        rem += (b & 0x7f) * mult
        if not (b & 0x80):
            break
        mult *= 128
        if mult > 128 * 128 * 128:
            raise RuntimeError("bad remaining length")
    return first, recv_exact(sock, rem) if rem else b""

def utf8_field(s):
    b = s.encode()
    return len(b).to_bytes(2, "big") + b

def connect_client(port, client_id):
    sock = socket.create_connection((HOST, port), timeout=30)
    payload = utf8_field(client_id)
    vh = b"\x00\x04MQTT" + bytes([4, 2]) + KEEPALIVE.to_bytes(2, "big")
    pkt = bytes([MQTT_CONNECT]) + enc_rem(len(vh) + len(payload)) + vh + payload
    send_all(sock, pkt)
    typ, body = recv_pkt(sock)
    if typ != MQTT_CONNACK or len(body) < 2 or body[1] != 0:
        raise RuntimeError(f"CONNECT failed on port {port}: type={typ:#x} body={body.hex()}")
    return sock

def subscribe_batch(sock, topics, pkt_id):
    body = bytearray(pkt_id.to_bytes(2, "big"))
    for topic in topics:
        body.extend(utf8_field(topic))
        body.append(0)
    pkt = bytes([MQTT_SUBSCRIBE | 0x02]) + enc_rem(len(body)) + bytes(body)
    send_all(sock, pkt)
    while True:
        typ, body = recv_pkt(sock)
        if typ == MQTT_SUBACK:
            if len(body) < 3 or body[0:2] != pkt_id.to_bytes(2, "big"):
                raise RuntimeError("bad SUBACK")
            if any(x == 0x80 for x in body[2:]):
                raise RuntimeError("subscription refused")
            return

def publish_qos0(sock, topic, payload):
    body = utf8_field(topic) + payload
    pkt = bytes([MQTT_PUBLISH]) + enc_rem(len(body)) + body
    send_all(sock, pkt)

def parse_publish(body):
    if len(body) < 2:
        return None, None
    tlen = int.from_bytes(body[0:2], "big")
    if len(body) < 2 + tlen:
        return None, None
    topic = body[2:2 + tlen].decode(errors="replace")
    payload = body[2 + tlen:]
    return topic, payload

def percentile(values, pct):
    if not values:
        return 0.0
    values = sorted(values)
    idx = int((len(values) - 1) * pct / 100.0)
    return values[idx]

broker_count = len(ports)
if broker_count < 2:
    raise RuntimeError("need at least 2 brokers for remote P2P latency")

topics_by_broker = [[] for _ in ports]
for i in range(total_subs):
    owner = i % broker_count
    topics_by_broker[owner].append(f"bench/{owner}/{i}")

sub_socks = []
topic_owner = {}
pkt_id = 1
setup_start = time.monotonic()
for owner, port in enumerate(ports):
    sock = connect_client(port, f"bench-sub-{owner}")
    sub_socks.append(sock)
    batch = []
    for topic in topics_by_broker[owner]:
        topic_owner[topic] = owner
        batch.append(topic)
        if len(batch) == 8:
            subscribe_batch(sock, batch, pkt_id)
            pkt_id = 1 if pkt_id == 65535 else pkt_id + 1
            batch = []
    if batch:
        subscribe_batch(sock, batch, pkt_id)
        pkt_id = 1 if pkt_id == 65535 else pkt_id + 1

setup_sec = time.monotonic() - setup_start
time.sleep(sync_settle)

pub_sock = connect_client(ports[0], "bench-pub")
remote_topics = [t for owner, ts in enumerate(topics_by_broker) if owner != 0 for t in ts]
if not remote_topics:
    raise RuntimeError("no remote topics to publish")

send_times = {}
lat_ms = []
received = 0
send_start = time.monotonic()
for seq in range(messages):
    topic = remote_topics[seq % len(remote_topics)]
    now_ns = time.time_ns()
    payload = f"{seq},{now_ns}".encode()
    send_times[seq] = now_ns
    publish_qos0(pub_sock, topic, payload)

deadline = time.monotonic() + max(5.0, messages * 0.05)
while received < messages and time.monotonic() < deadline:
    ready, _, _ = select.select(sub_socks, [], [], 0.25)
    for sock in ready:
        typ, body = recv_pkt(sock)
        if (typ & 0xf0) != MQTT_PUBLISH:
            continue
        _, payload = parse_publish(body)
        try:
            seq_s, sent_ns_s = payload.decode().split(",", 1)
            seq = int(seq_s)
            sent_ns = int(sent_ns_s)
        except Exception:
            continue
        if seq in send_times:
            lat_ms.append((time.time_ns() - sent_ns) / 1_000_000.0)
            received += 1
            del send_times[seq]

elapsed = time.monotonic() - send_start
lost = messages - received
throughput = received / elapsed if elapsed > 0 else 0.0

for sock in [pub_sock] + sub_socks:
    try:
        send_all(sock, bytes([MQTT_DISCONNECT, 0]))
        sock.close()
    except OSError:
        pass

lat_sorted = sorted(lat_ms)
min_ms = lat_sorted[0] if lat_sorted else 0.0
p50 = statistics.median(lat_sorted) if lat_sorted else 0.0
p95 = percentile(lat_sorted, 95)
p99 = percentile(lat_sorted, 99)
max_ms = lat_sorted[-1] if lat_sorted else 0.0
passed = 1 if received == messages and p95 <= target_ms else 0

print(
    f"RESULT,{broker_count},{total_subs},{messages},{received},{lost},"
    f"{setup_sec:.3f},{elapsed:.3f},{throughput:.2f},"
    f"{min_ms:.3f},{p50:.3f},{p95:.3f},{p99:.3f},{max_ms:.3f},{passed}"
)
PY
}

mkdir -p "$OUT"
echo "broker_count,total_subs,messages,received,lost,setup_sec,elapsed_sec,msg_per_sec,min_ms,p50_ms,p95_ms,p99_ms,max_ms,pass_p95"
echo "broker_count,total_subs,messages,received,lost,setup_sec,elapsed_sec,msg_per_sec,min_ms,p50_ms,p95_ms,p99_ms,max_ms,pass_p95" > "$RESULTS_FILE"

for count in $BROKER_COUNTS; do
    if [ "$count" -lt 2 ]; then
        echo "[skip] broker_count=$count needs at least 2 brokers" >&2
        continue
    fi

    _cleanup

    discovery_port=$((BASE_DISCOVERY_PORT + count))
    per_broker_subs="$(_ceil_div "$TOTAL_SUBS" "$count")"
    topic_slots=$((per_broker_subs + 32))
    remote_slots_per_node=$((per_broker_subs + 32))
    router_count="$ROUTER_COUNT"
    [ "$router_count" -eq 0 ] && router_count="$count"
    [ "$router_count" -gt "$count" ] && router_count="$count"

    if [ "$STRICT_ESP32" -eq 1 ]; then
        peer_max="$P2P_PEER_MAX_BENCH"
    else
        peer_max="$P2P_PEER_MAX_BENCH"
        [ "$peer_max" -lt $((count - 1)) ] && peer_max=$((count - 1))
        [ "$peer_max" -lt 10 ] && peer_max=10
    fi

    _require_free_ports "$count" "$discovery_port"

    ports_csv=""
    for ((i = 0; i < count; i++)); do
        mqtt_port=$((BASE_MQTT_PORT + i))
        p2p_port=$((BASE_P2P_PORT + i))
        bin="$OUT/broker_${count}_${i}"
        _build_node "$bin" "$mqtt_port" "$p2p_port" "$discovery_port" \
                    "$topic_slots" "$remote_slots_per_node" "$peer_max" "$router_count" || exit 1
        if [ -z "$ports_csv" ]; then
            ports_csv="$mqtt_port"
        else
            ports_csv="$ports_csv,$mqtt_port"
        fi
    done

    for ((i = 0; i < count; i++)); do
        bin="$OUT/broker_${count}_${i}"
        log="$OUT/broker_${count}_${i}.log"
        : > "$log"
        if [ "$i" -eq 0 ]; then
            pid="$(_run_broker "$bin" "$log")"
        else
            seed="127.0.0.1:$BASE_P2P_PORT"
            pid="$(_run_broker "$bin" "$log" "$seed")"
        fi
        PIDS="$PIDS $pid"
        sleep 0.15
    done

    sleep "$STARTUP_SEC"
    for pid in $PIDS; do
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "[fail] broker_count=$count broker exited during startup" >&2
            _dump_count_logs "$count"
            exit 1
        fi
    done
    if ! missing="$(_wait_for_mqtt_ports "$ports_csv")"; then
        echo "[fail] broker_count=$count MQTT ports not ready: $missing" >&2
        _dump_count_logs "$count"
        exit 1
    fi

    line="$(_run_python_load "$ports_csv" "$TOTAL_SUBS" "$MESSAGES" "$TARGET_P95_MS" "$SYNC_SETTLE_SEC")"
    if [[ "$line" == RESULT,* ]]; then
        csv="${line#RESULT,}"
        echo "$csv"
        echo "$csv" >> "$RESULTS_FILE"
        pass="${csv##*,}"
        if [ "$pass" = "1" ] && [ -z "$PASS_COUNT" ]; then
            PASS_COUNT="$count"
        fi
    else
        echo "$line"
        echo "[fail] unexpected benchmark output for broker_count=$count" >&2
        exit 1
    fi
done

_cleanup

python3 - "$RESULTS_FILE" "$TARGET_P95_MS" <<'PY'
import csv
import sys

path = sys.argv[1]
target = float(sys.argv[2])

rows = []
with open(path, newline="") as f:
    for row in csv.DictReader(f):
        row["broker_count"] = int(row["broker_count"])
        row["msg_per_sec"] = float(row["msg_per_sec"])
        row["p95_ms"] = float(row["p95_ms"])
        row["p99_ms"] = float(row["p99_ms"])
        row["received"] = int(row["received"])
        row["lost"] = int(row["lost"])
        row["pass_p95"] = int(row["pass_p95"])
        rows.append(row)

print("# throughput summary")
print("# brokers,msg_per_sec,p95_ms,p99_ms,lost,pass_p95,trend_vs_previous")
prev = None
best = None
first_pass = None
for row in rows:
    trend = "baseline"
    if prev is not None:
        if row["msg_per_sec"] > prev["msg_per_sec"] * 1.05:
            trend = "increased"
        elif row["msg_per_sec"] < prev["msg_per_sec"] * 0.95:
            trend = "decreased"
        else:
            trend = "flat"
    if best is None or row["msg_per_sec"] > best["msg_per_sec"]:
        best = row
    if first_pass is None and row["pass_p95"] == 1:
        first_pass = row
    print(
        f"# {row['broker_count']},{row['msg_per_sec']:.2f},"
        f"{row['p95_ms']:.3f},{row['p99_ms']:.3f},"
        f"{row['lost']},{row['pass_p95']},{trend}"
    )
    prev = row

if best:
    print(
        f"# best throughput: {best['broker_count']} brokers, "
        f"{best['msg_per_sec']:.2f} msg/s, p95={best['p95_ms']:.3f}ms"
    )
if first_pass:
    print(
        f"# first broker_count meeting p95 <= {target:.3f}ms: "
        f"{first_pass['broker_count']}"
    )
else:
    print(f"# no tested broker_count met p95 <= {target:.3f}ms")

if len(rows) >= 2:
    nondecreasing = all(
        rows[i]["msg_per_sec"] >= rows[i - 1]["msg_per_sec"] * 0.95
        for i in range(1, len(rows))
    )
    if nondecreasing:
        print("# throughput trend: increased or stayed within 5% tolerance")
    else:
        print("# throughput trend: not monotonic; inspect broker logs and host CPU/network limits")
PY
