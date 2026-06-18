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
#   TOPIC_COUNTS="200 10000" MESSAGE_COUNTS="50 500" ./scripts/bench_p2p_scale.sh
#   TARGET_P95_MS=10 ./scripts/bench_p2p_scale.sh
#   STRICT_ESP32=1 ./scripts/bench_p2p_scale.sh
#   ESP32_PROFILE=1 ./scripts/bench_p2p_scale.sh
#   MOSQUITTO_BENCH=0 ./scripts/bench_p2p_scale.sh
#   SENSOR_CLIENTS=1000 TOPIC_COUNTS="100 200" MESSAGE_COUNTS="50000" ./scripts/bench_p2p_scale.sh
#
# STRICT_ESP32=1 keeps MQTT_MAX_CLIENTS=8 and uses exactly P2P_PEER_MAX_BENCH
# instead of auto-raising it to the tested broker count. Use
# P2P_PEER_MAX_BENCH=5 or 10 to compare ESP32-like peer budgets. By default,
# MQTT_MAX_CLIENTS stays 8 while P2P_PEER_MAX is kept at least 10 and is
# auto-raised to count-1 for larger tests. ROUTER_COUNT=0 enables the
# reward-driven dynamic router budget search; set ROUTER_COUNT=N to force a
# fixed router budget in a given round.
#
# ESP32_PROFILE=1 turns on STRICT_ESP32 and clamps the benchmark to a tighter
# ESP32-like envelope: MQTT_MAX_CLIENTS<=8, P2P_PEER_MAX<=5, and
# STATIC_SEED_FANOUT<=2.
#
# MOSQUITTO_BENCH=1 runs a single local mosquitto instance with the same load
# before the mqtt_min_broker rounds, so the output can compare both
# implementations in one results.csv.
#
# SENSOR_CLIENTS models logical sensors. For single-host tests the script
# multiplexes them through SENSOR_CONNECTIONS MQTT connections, capped to 128
# by default, so large sensor-count scenarios do not mostly measure local
# thread scheduling. Set SENSOR_CONNECTIONS=$SENSOR_CLIENTS when deliberately
# testing one TCP client per sensor.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."
OUT="${OUT:-$ROOT/build_out/p2p_scale_bench}"

TOTAL_SUBS="${TOTAL_SUBS:-10000}"
BROKER_COUNTS="${BROKER_COUNTS:-2 5 10 20 50 100}"
MESSAGES="${MESSAGES:-300}"
TOPIC_COUNTS="${TOPIC_COUNTS:-$TOTAL_SUBS}"
MESSAGE_COUNTS="${MESSAGE_COUNTS:-$MESSAGES}"
TARGET_P95_MS="${TARGET_P95_MS:-10}"
MQTT_MAX_CLIENTS_BENCH="${MQTT_MAX_CLIENTS_BENCH:-8}"
P2P_PEER_MAX_BENCH="${P2P_PEER_MAX_BENCH:-10}"
ROUTER_COUNT="${ROUTER_COUNT:-0}"
STRICT_ESP32="${STRICT_ESP32:-0}"
ESP32_PROFILE="${ESP32_PROFILE:-0}"
EXTRA_CFLAGS="${EXTRA_CFLAGS:--DCONFIG_MQTT_P2P_STATIC_SEEDS_ONLY}"
BASE_MQTT_PORT="${BASE_MQTT_PORT:-19100}"
BASE_P2P_PORT="${BASE_P2P_PORT:-29100}"
BASE_DISCOVERY_PORT="${BASE_DISCOVERY_PORT:-30100}"
STARTUP_SEC="${STARTUP_SEC:-2}"
SYNC_SETTLE_SEC="${SYNC_SETTLE_SEC:-3}"
MOSQUITTO_BENCH="${MOSQUITTO_BENCH:-1}"
MOSQUITTO_BIN="${MOSQUITTO_BIN:-mosquitto}"
DISTRIBUTED_PUBLISHERS="${DISTRIBUTED_PUBLISHERS:-0}"
SCALE_MESSAGES_BY_BROKER="${SCALE_MESSAGES_BY_BROKER:-0}"
STATIC_SEED_FANOUT="${STATIC_SEED_FANOUT:-1}"
SENSOR_CLIENTS="${SENSOR_CLIENTS:-0}"
SENSOR_CONNECTIONS="${SENSOR_CONNECTIONS:-0}"
SENSOR_WORKERS="${SENSOR_WORKERS:-64}"
BENCH_KEEPALIVE="${BENCH_KEEPALIVE:-600}"
BENCH_DRAIN_TIMEOUT_SEC="${BENCH_DRAIN_TIMEOUT_SEC:-60}"

if [ "$ESP32_PROFILE" -eq 1 ]; then
    STRICT_ESP32=1
    MQTT_MAX_CLIENTS_BENCH="${MQTT_MAX_CLIENTS_BENCH:-8}"
    P2P_PEER_MAX_BENCH="${P2P_PEER_MAX_BENCH:-5}"
    [ "$P2P_PEER_MAX_BENCH" -gt 5 ] && P2P_PEER_MAX_BENCH=5
    [ "$MQTT_MAX_CLIENTS_BENCH" -gt 8 ] && MQTT_MAX_CLIENTS_BENCH=8
    [ "$STATIC_SEED_FANOUT" -gt 2 ] && STATIC_SEED_FANOUT=2
    [ "$STARTUP_SEC" -lt 2 ] && STARTUP_SEC=2
    [ "$SYNC_SETTLE_SEC" -lt 10 ] && SYNC_SETTLE_SEC=10
fi

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
    KEEP=5 LOG_ROOT="$OUT" "$SCRIPT_DIR/cleanup_logs.sh" >/dev/null 2>&1 || true
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

_seed_list() {
    local index="$1"
    local first=1
    local seeds=""
    local start=0

    if [ "$index" -le 0 ]; then
        echo ""
        return
    fi
    if [ "$STATIC_SEED_FANOUT" -gt 0 ] && [ "$index" -gt "$STATIC_SEED_FANOUT" ]; then
        start=$((index - STATIC_SEED_FANOUT))
    fi
    for ((s = start; s < index; s++)); do
        if [ "$first" -eq 1 ]; then
            first=0
        else
            seeds="$seeds,"
        fi
        seeds="${seeds}127.0.0.1:$((BASE_P2P_PORT + s))"
    done
    echo "$seeds"
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
        "$ROOT/src/p2p_shard.c" \
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

_run_mosquitto() {
    local port="$1" log="$2" conf="$3"

    cat > "$conf" <<EOF
listener $port 127.0.0.1
allow_anonymous true
persistence false
log_type error
EOF
    if command -v stdbuf >/dev/null 2>&1; then
        stdbuf -oL -eL "$MOSQUITTO_BIN" -c "$conf" > "$log" 2>&1 &
    else
        "$MOSQUITTO_BIN" -c "$conf" > "$log" 2>&1 &
    fi
    echo $!
}

_dump_count_logs() {
    local scenario="$1"
    local count="$2"

    for log in "$OUT"/broker_"$scenario"_"$count"_*.log; do
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

    python3 - "$ports_csv" "$total_subs" "$messages" "$target_ms" "$sync_settle" \
        "$DISTRIBUTED_PUBLISHERS" "$SCALE_MESSAGES_BY_BROKER" "$SENSOR_CLIENTS" \
        "$SENSOR_CONNECTIONS" "$SENSOR_WORKERS" "$BENCH_KEEPALIVE" \
        "$BENCH_DRAIN_TIMEOUT_SEC" "$ESP32_PROFILE" <<'PY'
import socket
import statistics
import sys
import threading
import time

ports = [int(p) for p in sys.argv[1].split(",") if p]
total_subs = int(sys.argv[2])
messages = int(sys.argv[3])
target_ms = float(sys.argv[4])
sync_settle = float(sys.argv[5])
distributed_publishers = int(sys.argv[6]) != 0
scale_messages_by_broker = int(sys.argv[7]) != 0
sensor_clients = int(sys.argv[8])
sensor_connections = int(sys.argv[9])
sensor_workers = int(sys.argv[10])
bench_keepalive = int(sys.argv[11])
drain_timeout_sec = float(sys.argv[12])
esp32_profile = int(sys.argv[13]) != 0

HOST = "127.0.0.1"
KEEPALIVE = bench_keepalive

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
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
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

pub_socks = []
pub_ports = []
if sensor_clients > 0:
    if sensor_connections <= 0:
        sensor_connections = min(sensor_clients, 128)
    sensor_connections = max(1, min(sensor_connections, sensor_clients))
    pub_count = sensor_connections
    for i in range(pub_count):
        port_idx = i % broker_count
        pub_socks.append(connect_client(ports[port_idx], f"bench-sensor-conn-{i}"))
        pub_ports.append(port_idx)
else:
    pub_count = broker_count if distributed_publishers else 1
    for i in range(pub_count):
        pub_socks.append(connect_client(ports[i], f"bench-pub-{i}"))
        pub_ports.append(i)

remote_topics_by_pub = []
for pub_idx in range(pub_count):
    port_idx = pub_ports[pub_idx]
    remote = [t for owner, ts in enumerate(topics_by_broker)
              if owner != port_idx for t in ts]
    if not remote:
        remote = [t for ts in topics_by_broker for t in ts]
    remote_topics_by_pub.append(remote)

base_messages = messages
if scale_messages_by_broker:
    messages = base_messages * broker_count

send_times = [0] * messages
lat_ms = []
recv_count = [0]
recv_errors = []
recv_lock = threading.Lock()
stop_receivers = threading.Event()

def receiver_worker(sock):
    sock.settimeout(0.1)
    while not stop_receivers.is_set():
        try:
            typ, body = recv_pkt(sock)
        except socket.timeout:
            continue
        except Exception as exc:
            if not stop_receivers.is_set():
                recv_errors.append(exc)
            return
        if (typ & 0xf0) != MQTT_PUBLISH:
            continue
        _, payload = parse_publish(body)
        try:
            fields = payload.decode().split(",", 2)
            seq_s, sent_ns_s = fields[0], fields[1]
            seq = int(seq_s)
            int(sent_ns_s)
        except Exception:
            continue
        if 0 <= seq < messages:
            sent_at = send_times[seq]
            if sent_at:
                send_times[seq] = 0
                latency = (time.time_ns() - sent_at) / 1_000_000.0
                done = False
                with recv_lock:
                    lat_ms.append(latency)
                    recv_count[0] += 1
                    done = recv_count[0] >= messages
                if done:
                    stop_receivers.set()

recv_threads = [threading.Thread(target=receiver_worker, args=(sock,))
                for sock in sub_socks]
for thread in recv_threads:
    thread.start()

send_start = time.monotonic()
if sensor_clients > 0 and pub_count > 1:
    worker_count = max(1, min(sensor_workers, pub_count))
    start_event = threading.Event()
    errors = []

    def sensor_worker(worker_idx):
        try:
            start_event.wait()
            for conn_idx in range(worker_idx, pub_count, worker_count):
                topics = remote_topics_by_pub[conn_idx]
                for seq in range(conn_idx, messages, pub_count):
                    sensor_id = seq % sensor_clients
                    now_ns = time.time_ns()
                    payload = f"{seq},{now_ns},{sensor_id}".encode()
                    send_times[seq] = now_ns
                    topic = topics[(sensor_id + seq // sensor_clients) % len(topics)]
                    publish_qos0(pub_socks[conn_idx], topic, payload)
        except Exception as exc:
            errors.append(exc)

    threads = [threading.Thread(target=sensor_worker, args=(i,))
               for i in range(worker_count)]
    for thread in threads:
        thread.start()
    start_event.set()
    for thread in threads:
        thread.join()
    if errors:
        raise errors[0]

    for sock in pub_socks:
        try:
            send_all(sock, bytes([MQTT_DISCONNECT, 0]))
            sock.close()
        except OSError:
            pass
    pub_socks = []
elif distributed_publishers and pub_count > 1:
    start_event = threading.Event()
    errors = []

    def publish_worker(pub_idx):
        topics = remote_topics_by_pub[pub_idx]
        try:
            start_event.wait()
            for seq in range(pub_idx, messages, pub_count):
                now_ns = time.time_ns()
                payload = f"{seq},{now_ns}".encode()
                send_times[seq] = now_ns
                topic = topics[(seq // pub_count) % len(topics)]
                publish_qos0(pub_socks[pub_idx], topic, payload)
        except Exception as exc:
            errors.append(exc)

    threads = [threading.Thread(target=publish_worker, args=(i,))
               for i in range(pub_count)]
    for thread in threads:
        thread.start()
    start_event.set()
    for thread in threads:
        thread.join()
    if errors:
        raise errors[0]
else:
    for seq in range(messages):
        pub_idx = seq % pub_count
        topics = remote_topics_by_pub[pub_idx]
        topic = topics[(seq // pub_count) % len(topics)]
        now_ns = time.time_ns()
        payload = f"{seq},{now_ns}".encode()
        send_times[seq] = now_ns
        publish_qos0(pub_socks[pub_idx], topic, payload)

deadline = time.monotonic() + max(1.0, drain_timeout_sec)
while time.monotonic() < deadline:
    with recv_lock:
        if recv_count[0] >= messages:
            break
    if recv_errors:
        raise recv_errors[0]
    time.sleep(0.005)

elapsed = time.monotonic() - send_start
stop_receivers.set()
for thread in recv_threads:
    thread.join(timeout=1.0)

received = recv_count[0]
lost = messages - received
throughput = received / elapsed if elapsed > 0 else 0.0

for sock in pub_socks + sub_socks:
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
echo "implementation,broker_count,total_subs,messages,received,lost,setup_sec,elapsed_sec,msg_per_sec,min_ms,p50_ms,p95_ms,p99_ms,max_ms,pass_p95"
echo "implementation,broker_count,total_subs,messages,received,lost,setup_sec,elapsed_sec,msg_per_sec,min_ms,p50_ms,p95_ms,p99_ms,max_ms,pass_p95" > "$RESULTS_FILE"

for bench_topics in $TOPIC_COUNTS; do
    for bench_messages in $MESSAGE_COUNTS; do
        scenario="t${bench_topics}_m${bench_messages}"
        echo "# scenario topics=$bench_topics messages=$bench_messages" >&2

        if [ "$MOSQUITTO_BENCH" -eq 1 ]; then
            if ! command -v "$MOSQUITTO_BIN" >/dev/null 2>&1; then
                echo "[skip] mosquitto not found: $MOSQUITTO_BIN" >&2
            else
                _cleanup
                mqtt_port="$BASE_MQTT_PORT"
                if _port_in_use "$mqtt_port"; then
                    echo "[setup] MQTT port $mqtt_port is already in use" >&2
                    exit 1
                fi

                log="$OUT/mosquitto_${scenario}.log"
                conf="$OUT/mosquitto_${scenario}.conf"
                : > "$log"
                pid="$(_run_mosquitto "$mqtt_port" "$log" "$conf")"
                PIDS="$PIDS $pid"

                sleep "$STARTUP_SEC"
                if ! kill -0 "$pid" 2>/dev/null; then
                    echo "[fail] mosquitto exited during startup" >&2
                    tail -n 40 "$log" >&2
                    exit 1
                fi
                if ! missing="$(_wait_for_mqtt_ports "$mqtt_port")"; then
                    echo "[fail] mosquitto MQTT port not ready: $missing" >&2
                    tail -n 40 "$log" >&2
                    exit 1
                fi

                line="$(_run_python_load "$mqtt_port" "$bench_topics" "$bench_messages" "$TARGET_P95_MS" "$SYNC_SETTLE_SEC")"
                if [[ "$line" == RESULT,* ]]; then
                    csv="${line#RESULT,}"
                    echo "mosquitto,$csv"
                    echo "mosquitto,$csv" >> "$RESULTS_FILE"
                else
                    echo "$line"
                    echo "[fail] unexpected mosquitto benchmark output" >&2
                    exit 1
                fi
            fi
        fi

        for count in $BROKER_COUNTS; do
            _cleanup

            discovery_port=$((BASE_DISCOVERY_PORT + count))
            per_broker_subs="$(_ceil_div "$bench_topics" "$count")"
            topic_slots=$((per_broker_subs + 32))
            remote_slots_per_node=$((per_broker_subs + 32))
            router_count="$ROUTER_COUNT"
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
                bin="$OUT/broker_${scenario}_${count}_${i}"
                _build_node "$bin" "$mqtt_port" "$p2p_port" "$discovery_port" \
                            "$topic_slots" "$remote_slots_per_node" "$peer_max" "$router_count" || exit 1
                if [ -z "$ports_csv" ]; then
                    ports_csv="$mqtt_port"
                else
                    ports_csv="$ports_csv,$mqtt_port"
                fi
            done

            for ((i = 0; i < count; i++)); do
                bin="$OUT/broker_${scenario}_${count}_${i}"
                log="$OUT/broker_${scenario}_${count}_${i}.log"
                : > "$log"
                if [ "$i" -eq 0 ]; then
                    pid="$(_run_broker "$bin" "$log")"
                else
                    seed="$(_seed_list "$i")"
                    pid="$(_run_broker "$bin" "$log" "$seed")"
                fi
                PIDS="$PIDS $pid"
                sleep 0.15
            done

            sleep "$STARTUP_SEC"
            for pid in $PIDS; do
                if ! kill -0 "$pid" 2>/dev/null; then
                    echo "[fail] broker_count=$count broker exited during startup" >&2
                    _dump_count_logs "$scenario" "$count"
                    exit 1
                fi
            done
            if ! missing="$(_wait_for_mqtt_ports "$ports_csv")"; then
                echo "[fail] broker_count=$count MQTT ports not ready: $missing" >&2
                _dump_count_logs "$scenario" "$count"
                exit 1
            fi

            line="$(_run_python_load "$ports_csv" "$bench_topics" "$bench_messages" "$TARGET_P95_MS" "$SYNC_SETTLE_SEC")"
            if [[ "$line" == RESULT,* ]]; then
                csv="${line#RESULT,}"
                echo "mqtt_min_broker,$csv"
                echo "mqtt_min_broker,$csv" >> "$RESULTS_FILE"
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
    done
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
        row["implementation"] = row["implementation"]
        row["broker_count"] = int(row["broker_count"])
        row["msg_per_sec"] = float(row["msg_per_sec"])
        row["p95_ms"] = float(row["p95_ms"])
        row["p99_ms"] = float(row["p99_ms"])
        row["received"] = int(row["received"])
        row["lost"] = int(row["lost"])
        row["pass_p95"] = int(row["pass_p95"])
        rows.append(row)

print("# throughput summary")
print("# implementation,brokers,msg_per_sec,p95_ms,p99_ms,lost,pass_p95,trend_vs_previous_same_impl")
prev_by_impl = {}
best = None
first_pass = None
for row in rows:
    impl = row["implementation"]
    prev = prev_by_impl.get(impl)
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
        f"# {impl},{row['broker_count']},{row['msg_per_sec']:.2f},"
        f"{row['p95_ms']:.3f},{row['p99_ms']:.3f},"
        f"{row['lost']},{row['pass_p95']},{trend}"
    )
    prev_by_impl[impl] = row

if best:
    print(
        f"# best throughput: {best['implementation']} {best['broker_count']} brokers, "
        f"{best['msg_per_sec']:.2f} msg/s, p95={best['p95_ms']:.3f}ms"
    )
if first_pass:
    print(
        f"# first broker_count meeting p95 <= {target:.3f}ms: "
        f"{first_pass['implementation']} {first_pass['broker_count']}"
    )
else:
    print(f"# no tested broker_count met p95 <= {target:.3f}ms")

own_rows = [row for row in rows if row["implementation"] == "mqtt_min_broker"]
if len(own_rows) >= 2:
    nondecreasing = all(
        own_rows[i]["msg_per_sec"] >= own_rows[i - 1]["msg_per_sec"] * 0.95
        for i in range(1, len(own_rows))
    )
    if nondecreasing:
        print("# mqtt_min_broker throughput trend: increased or stayed within 5% tolerance")
    else:
        print("# mqtt_min_broker throughput trend: not monotonic; inspect broker logs and host CPU/network limits")
PY
