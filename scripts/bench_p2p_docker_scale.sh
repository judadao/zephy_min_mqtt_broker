#!/usr/bin/env bash
# Docker-based P2P broker scale benchmark.
#
# Example:
#   SENSOR_CLIENTS=500 MESSAGE_COUNTS="10000" BROKER_COUNTS="1 2 5 10 20 50 100" \
#     ./scripts/bench_p2p_docker_scale.sh
#
# This simulates broker nodes with separate Docker network namespaces and IPs.
# It still shares one physical host, so throughput is useful for topology and
# routing stability checks, not a replacement for a true multi-machine test.
# Set ESP32_PROFILE=1 to constrain each broker container to an ESP32-like
# envelope: MQTT_MAX_CLIENTS<=8, P2P_PEER_MAX<=5, STATIC_SEED_FANOUT<=2, and
# small container process/CPU budgets.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."
OUT="${OUT:-$ROOT/build_out/p2p_docker_bench}"

BROKER_COUNTS="${BROKER_COUNTS:-1 2 5 10 20 50 100}"
TOPIC_COUNTS="${TOPIC_COUNTS:-100 200}"
MESSAGE_COUNTS="${MESSAGE_COUNTS:-10000}"
SENSOR_CLIENTS="${SENSOR_CLIENTS:-500}"
SENSOR_CONNECTIONS="${SENSOR_CONNECTIONS:-0}"
SENSOR_WORKERS="${SENSOR_WORKERS:-64}"
MQTT_MAX_CLIENTS_BENCH="${MQTT_MAX_CLIENTS_BENCH:-512}"
P2P_PEER_MAX_BENCH="${P2P_PEER_MAX_BENCH:-128}"
ROUTER_COUNT="${ROUTER_COUNT:-0}"
STATIC_SEED_FANOUT="${STATIC_SEED_FANOUT:-10}"
DISTRIBUTED_PUBLISHERS="${DISTRIBUTED_PUBLISHERS:-0}"
SCALE_MESSAGES_BY_BROKER="${SCALE_MESSAGES_BY_BROKER:-0}"
STRICT_ESP32="${STRICT_ESP32:-0}"
ESP32_PROFILE="${ESP32_PROFILE:-0}"
BROKER_DOCKER_CPUS="${BROKER_DOCKER_CPUS:-}"
BROKER_DOCKER_PIDS_LIMIT="${BROKER_DOCKER_PIDS_LIMIT:-}"
STARTUP_SEC="${STARTUP_SEC:-2}"
SYNC_SETTLE_SEC="${SYNC_SETTLE_SEC:-5}"
BENCH_KEEPALIVE="${BENCH_KEEPALIVE:-600}"
BENCH_DRAIN_TIMEOUT_SEC="${BENCH_DRAIN_TIMEOUT_SEC:-30}"
TARGET_P95_MS="${TARGET_P95_MS:-10}"
DOCKER_IMAGE_BASE="${DOCKER_IMAGE_BASE:-python:3-slim}"
NETWORK_NAME="${NETWORK_NAME:-mqtt-p2p-bench}"
NETWORK_SUBNET="${NETWORK_SUBNET:-172.31.0.0/16}"
IP_PREFIX="${IP_PREFIX:-172.31.1}"
WAIT_MQTT_TIMEOUT_SEC="${WAIT_MQTT_TIMEOUT_SEC:-30}"
KEEP_CONTAINERS_ON_EXIT="${KEEP_CONTAINERS_ON_EXIT:-0}"
LOADGEN_IN_DOCKER="${LOADGEN_IN_DOCKER:-1}"
FAIL_NODE_INDEX="${FAIL_NODE_INDEX:-}"
FAIL_ACTION="${FAIL_ACTION:-kill}"
FAIL_AFTER_SEC="${FAIL_AFTER_SEC:-0}"
FAIL_RECOVER_AFTER_SEC="${FAIL_RECOVER_AFTER_SEC:-0}"
FAIL_BROKER_COUNT="${FAIL_BROKER_COUNT:-}"
FAIL_JOB_PIDS=""

if [ "$ESP32_PROFILE" -eq 1 ]; then
    STRICT_ESP32=1
    MQTT_MAX_CLIENTS_BENCH="${MQTT_MAX_CLIENTS_BENCH:-8}"
    P2P_PEER_MAX_BENCH="${P2P_PEER_MAX_BENCH:-5}"
    [ "$MQTT_MAX_CLIENTS_BENCH" -gt 8 ] && MQTT_MAX_CLIENTS_BENCH=8
    [ "$P2P_PEER_MAX_BENCH" -gt 5 ] && P2P_PEER_MAX_BENCH=5
    [ "$STATIC_SEED_FANOUT" -gt 2 ] && STATIC_SEED_FANOUT=2
    [ "$STARTUP_SEC" -lt 2 ] && STARTUP_SEC=2
    [ "$SYNC_SETTLE_SEC" -lt 10 ] && SYNC_SETTLE_SEC=10
    [ -z "$BROKER_DOCKER_CPUS" ] && BROKER_DOCKER_CPUS="1.0"
    [ -z "$BROKER_DOCKER_PIDS_LIMIT" ] && BROKER_DOCKER_PIDS_LIMIT="24"
fi

RESULTS_FILE="$OUT/results.csv"
CONTAINERS=""

_cleanup() {
    if [ "$KEEP_CONTAINERS_ON_EXIT" -eq 1 ]; then
        return
    fi
    for pid in $FAIL_JOB_PIDS; do
        kill "$pid" >/dev/null 2>&1 || true
    done
    FAIL_JOB_PIDS=""
    for name in $CONTAINERS; do
        docker rm -f "$name" >/dev/null 2>&1 || true
    done
    CONTAINERS=""
    KEEP=5 LOG_ROOT="$OUT" "$SCRIPT_DIR/cleanup_logs.sh" >/dev/null 2>&1 || true
}
trap _cleanup EXIT

_ensure_network() {
    if docker network inspect "$NETWORK_NAME" >/dev/null 2>&1; then
        return
    fi
    docker network create --subnet "$NETWORK_SUBNET" "$NETWORK_NAME" >/dev/null
}

_reset_network() {
    _cleanup
    if docker network inspect "$NETWORK_NAME" >/dev/null 2>&1; then
        docker network rm "$NETWORK_NAME" >/dev/null 2>&1 || true
    fi
    _ensure_network
}

_ceil_div() {
    local n="$1" d="$2"
    echo $(((n + d - 1) / d))
}

_seed_list() {
    local index="$1"
    local seeds=""
    local first=1
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
        seeds="${seeds}${IP_PREFIX}.$((10 + s)):4884"
    done
    echo "$seeds"
}

_build_broker() {
    local out="$1"
    local topic_slots="$2"
    local remote_slots_per_node="$3"
    local peer_max="$4"
    local router_count="$5"

    gcc -Wall -Wextra -Wno-stringop-truncation -std=c11 -O2 -g -D_POSIX_C_SOURCE=200809L \
        -DCONFIG_MQTT_P2P_DYNAMIC \
        -DCONFIG_MQTT_P2P_STATIC_SEEDS_ONLY \
        -I"$ROOT/include" -I"$ROOT" \
        -DMQTT_BROKER_PORT=1883 \
        -DP2P_PORT=4884 \
        -DP2P_DISCOVERY_PORT=4885 \
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

_start_brokers() {
    local count="$1"
    local bin="$2"
    local scenario="$3"
    local docker_resource_args=()

    if [ -n "$BROKER_DOCKER_CPUS" ]; then
        docker_resource_args+=(--cpus "$BROKER_DOCKER_CPUS")
    fi
    if [ -n "$BROKER_DOCKER_PIDS_LIMIT" ]; then
        docker_resource_args+=(--pids-limit "$BROKER_DOCKER_PIDS_LIMIT")
    fi

    for ((i = 0; i < count; i++)); do
        local name="mqtt-p2p-${scenario}-${i}"
        local ip="${IP_PREFIX}.$((10 + i))"
        local log="$OUT/${name}.log"
        local seeds=""

        seeds="$(_seed_list "$i")"
        : > "$log"
        docker rm -f "$name" >/dev/null 2>&1 || true
        if ! docker run -d --name "$name" \
                "${docker_resource_args[@]}" \
                --network "$NETWORK_NAME" --ip "$ip" \
                -e MQTT_P2P_PEERS="$seeds" \
                -v "$bin:/mqtt_broker:ro" \
                "$DOCKER_IMAGE_BASE" /mqtt_broker > /dev/null; then
            echo "[fail] docker run failed for $name ip=$ip" >&2
            exit 1
        fi
        CONTAINERS="$CONTAINERS $name"
    done
}

_collect_logs() {
    local scenario="$1"

    mkdir -p "$OUT/logs_$scenario"
    for name in $CONTAINERS; do
        docker logs "$name" > "$OUT/logs_$scenario/$name.log" 2>&1 || true
    done
}

_schedule_failure() {
    local scenario="$1"
    local count="$2"
    local target_idx="$3"
    local action="$4"
    local after_sec="$5"
    local recover_after_sec="$6"

    if [ -z "$target_idx" ] || [ "$after_sec" -le 0 ]; then
        return
    fi
    if [ -n "$FAIL_BROKER_COUNT" ] && [ "$FAIL_BROKER_COUNT" -ne "$count" ]; then
        return
    fi
    if [ "$target_idx" -lt 0 ] || [ "$target_idx" -ge "$count" ]; then
        return
    fi

    local name="mqtt-p2p-${scenario}-${target_idx}"
    (
        sleep "$after_sec"
        if [ "$action" = "restart" ]; then
            docker restart "$name" >/dev/null 2>&1 || true
        else
            docker kill "$name" >/dev/null 2>&1 || true
            if [ "$recover_after_sec" -gt 0 ]; then
                sleep "$recover_after_sec"
                docker start "$name" >/dev/null 2>&1 || true
            fi
        fi
    ) &
    FAIL_JOB_PIDS="$FAIL_JOB_PIDS $!"
}

_wait_for_mqtt_hosts() {
    local hosts_csv="$1"

    local cmd
    if [ "$LOADGEN_IN_DOCKER" -eq 1 ]; then
        cmd=(docker run --rm -i --network "$NETWORK_NAME" "$DOCKER_IMAGE_BASE" python3 - "$hosts_csv" "$WAIT_MQTT_TIMEOUT_SEC")
    else
        cmd=(python3 - "$hosts_csv" "$WAIT_MQTT_TIMEOUT_SEC")
    fi

    "${cmd[@]}" <<'PY'
import socket
import sys
import time

hosts = [h for h in sys.argv[1].split(",") if h]
deadline = time.monotonic() + float(sys.argv[2])
pending = set(hosts)

while pending and time.monotonic() < deadline:
    for host in list(pending):
        try:
            sock = socket.create_connection((host, 1883), timeout=0.3)
            sock.close()
            pending.remove(host)
        except OSError:
            pass
    if pending:
        time.sleep(0.1)

if pending:
    print(",".join(sorted(pending)))
    sys.exit(1)
PY
}

_run_python_load() {
    local hosts_csv="$1"
    local total_subs="$2"
    local messages="$3"
    local target_ms="$4"
    local sync_settle="$5"

    local cmd
    if [ "$LOADGEN_IN_DOCKER" -eq 1 ]; then
        cmd=(docker run --rm -i --network "$NETWORK_NAME" "$DOCKER_IMAGE_BASE" python3 -)
    else
        cmd=(python3 -)
    fi

    "${cmd[@]}" "$hosts_csv" "$total_subs" "$messages" "$target_ms" "$sync_settle" \
        "$SENSOR_CLIENTS" "$SENSOR_CONNECTIONS" "$SENSOR_WORKERS" "$BENCH_KEEPALIVE" \
        "$BENCH_DRAIN_TIMEOUT_SEC" "$DISTRIBUTED_PUBLISHERS" "$SCALE_MESSAGES_BY_BROKER" \
        "$ESP32_PROFILE" <<'PY'
import socket
import statistics
import sys
import threading
import time

hosts = [h for h in sys.argv[1].split(",") if h]
total_subs = int(sys.argv[2])
messages = int(sys.argv[3])
target_ms = float(sys.argv[4])
sync_settle = float(sys.argv[5])
sensor_clients = int(sys.argv[6])
sensor_connections = int(sys.argv[7])
sensor_workers = int(sys.argv[8])
bench_keepalive = int(sys.argv[9])
drain_timeout_sec = float(sys.argv[10])
distributed_publishers = int(sys.argv[11]) != 0
scale_messages_by_broker = int(sys.argv[12]) != 0
esp32_profile = int(sys.argv[13]) != 0
sensor_divisor = max(1, sensor_clients)

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

def connect_client(host, client_id):
    sock = socket.create_connection((host, 1883), timeout=30)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    payload = utf8_field(client_id)
    vh = b"\x00\x04MQTT" + bytes([4, 2]) + KEEPALIVE.to_bytes(2, "big")
    pkt = bytes([MQTT_CONNECT]) + enc_rem(len(vh) + len(payload)) + vh + payload
    send_all(sock, pkt)
    typ, body = recv_pkt(sock)
    if typ != MQTT_CONNACK or len(body) < 2 or body[1] != 0:
        raise RuntimeError(f"CONNECT failed on {host}: type={typ:#x} body={body.hex()}")
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
    send_all(sock, bytes([MQTT_PUBLISH]) + enc_rem(len(body)) + body)

def parse_publish(body):
    if len(body) < 2:
        return None, None
    tlen = int.from_bytes(body[0:2], "big")
    if len(body) < 2 + tlen:
        return None, None
    return body[2:2 + tlen].decode(errors="replace"), body[2 + tlen:]

def percentile(values, pct):
    if not values:
        return 0.0
    values = sorted(values)
    return values[int((len(values) - 1) * pct / 100.0)]

broker_count = len(hosts)
topics_by_broker = [[] for _ in hosts]
for i in range(total_subs):
    owner = i % broker_count
    topics_by_broker[owner].append(f"bench/{owner}/{i}")

sub_socks = []
pkt_id = 1
setup_start = time.monotonic()
for owner, host in enumerate(hosts):
    sock = connect_client(host, f"bench-sub-{owner}")
    sub_socks.append(sock)
    batch = []
    for topic in topics_by_broker[owner]:
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

if sensor_clients <= 0:
    sensor_clients = 1
if sensor_connections <= 0:
    sensor_connections = min(sensor_clients, 128)
sensor_connections = max(1, min(sensor_connections, sensor_clients))

pub_socks = []
pub_hosts = []
if sensor_clients > 0:
    for i in range(sensor_connections):
        host_idx = i % broker_count
        pub_socks.append(connect_client(hosts[host_idx], f"bench-sensor-conn-{i}"))
        pub_hosts.append(host_idx)
else:
    pub_count = broker_count if distributed_publishers else 1
    for i in range(pub_count):
        host_idx = i % broker_count
        pub_socks.append(connect_client(hosts[host_idx], f"bench-pub-{i}"))
        pub_hosts.append(host_idx)

remote_topics_by_pub = []
for pub_idx, host_idx in enumerate(pub_hosts):
    remote = [t for owner, ts in enumerate(topics_by_broker)
              if owner != host_idx for t in ts]
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
            seq = int(fields[0])
        except Exception:
            continue
        if 0 <= seq < messages:
            sent_at = send_times[seq]
            if sent_at:
                send_times[seq] = 0
                latency = (time.time_ns() - sent_at) / 1_000_000.0
                with recv_lock:
                    lat_ms.append(latency)
                    recv_count[0] += 1
                    if recv_count[0] >= messages:
                        stop_receivers.set()

recv_threads = [threading.Thread(target=receiver_worker, args=(sock,))
                for sock in sub_socks]
for thread in recv_threads:
    thread.start()

send_start = time.monotonic()
worker_count = max(1, min(sensor_workers, len(pub_socks)))
start_event = threading.Event()
errors = []

def sensor_worker(worker_idx):
    try:
        start_event.wait()
        for conn_idx in range(worker_idx, len(pub_socks), worker_count):
            topics = remote_topics_by_pub[conn_idx]
            for seq in range(conn_idx, messages, len(pub_socks)):
                sensor_id = seq % sensor_divisor
                now_ns = time.time_ns()
                send_times[seq] = now_ns
                topic = topics[(sensor_id + seq // sensor_divisor) % len(topics)]
                publish_qos0(pub_socks[conn_idx], topic,
                             f"{seq},{now_ns},{sensor_id}".encode())
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

for sock in sub_socks:
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

if ! command -v docker >/dev/null 2>&1; then
    echo "docker not found" >&2
    exit 1
fi
if ! docker image inspect "$DOCKER_IMAGE_BASE" >/dev/null 2>&1; then
    echo "Docker image not found locally: $DOCKER_IMAGE_BASE" >&2
    echo "Pull it first or set DOCKER_IMAGE_BASE to an existing glibc image." >&2
    exit 1
fi

_reset_network

for bench_topics in $TOPIC_COUNTS; do
    for bench_messages in $MESSAGE_COUNTS; do
        scenario="t${bench_topics}_m${bench_messages}"
        echo "# scenario topics=$bench_topics messages=$bench_messages sensors=$SENSOR_CLIENTS" >&2

        for count in $BROKER_COUNTS; do
            _cleanup

            if [ "$count" -gt 245 ]; then
                echo "[fail] count=$count exceeds default /24 IP allocation" >&2
                exit 1
            fi

            per_broker_subs="$(_ceil_div "$bench_topics" "$count")"
            topic_slots=$((per_broker_subs + 32))
            remote_slots_per_node=$((per_broker_subs + 32))
            peer_max="$P2P_PEER_MAX_BENCH"
            if [ "$STRICT_ESP32" -eq 1 ]; then
                [ "$peer_max" -gt 5 ] && peer_max=5
            else
                [ "$peer_max" -lt "$STATIC_SEED_FANOUT" ] && peer_max="$STATIC_SEED_FANOUT"
                [ "$peer_max" -lt 10 ] && peer_max=10
            fi
            router_count="$ROUTER_COUNT"
            [ "$router_count" -gt "$count" ] && router_count="$count"

            bin="$OUT/broker_${scenario}_${count}"
            _build_broker "$bin" "$topic_slots" "$remote_slots_per_node" \
                          "$peer_max" "$router_count" || exit 1
            _start_brokers "$count" "$bin" "${scenario}_${count}"

            sleep "$STARTUP_SEC"
            hosts_csv=""
            for ((i = 0; i < count; i++)); do
                ip="${IP_PREFIX}.$((10 + i))"
                if [ -z "$hosts_csv" ]; then
                    hosts_csv="$ip"
                else
                    hosts_csv="$hosts_csv,$ip"
                fi
            done
            if ! missing="$(_wait_for_mqtt_hosts "$hosts_csv")"; then
                _collect_logs "${scenario}_${count}"
                echo "[fail] broker_count=$count MQTT hosts not ready: $missing" >&2
                exit 1
            fi

            _schedule_failure "${scenario}_${count}" "$count" \
                "$FAIL_NODE_INDEX" "$FAIL_ACTION" "$FAIL_AFTER_SEC" \
                "$FAIL_RECOVER_AFTER_SEC"

            line="$(_run_python_load "$hosts_csv" "$bench_topics" "$bench_messages" \
                                      "$TARGET_P95_MS" "$SYNC_SETTLE_SEC")"
            _collect_logs "${scenario}_${count}"
            if [[ "$line" == RESULT,* ]]; then
                csv="${line#RESULT,}"
                echo "mqtt_min_broker_docker,$csv"
                echo "mqtt_min_broker_docker,$csv" >> "$RESULTS_FILE"
            else
                echo "$line"
                echo "[fail] unexpected benchmark output for broker_count=$count" >&2
                _collect_logs "${scenario}_${count}"
                exit 1
            fi
        done
    done
done

_cleanup

python3 - "$RESULTS_FILE" <<'PY'
import csv
import sys

rows = []
with open(sys.argv[1], newline="") as f:
    for row in csv.DictReader(f):
        row["broker_count"] = int(row["broker_count"])
        row["msg_per_sec"] = float(row["msg_per_sec"])
        row["p95_ms"] = float(row["p95_ms"])
        row["p99_ms"] = float(row["p99_ms"])
        row["lost"] = int(row["lost"])
        rows.append(row)

print("# docker throughput summary")
print("# brokers,msg_per_sec,p95_ms,p99_ms,lost,trend_vs_previous")
prev = None
for row in rows:
    trend = "baseline"
    if prev is not None:
        if row["msg_per_sec"] > prev["msg_per_sec"] * 1.05:
            trend = "increased"
        elif row["msg_per_sec"] < prev["msg_per_sec"] * 0.95:
            trend = "decreased"
        else:
            trend = "flat"
    print(
        f"# {row['broker_count']},{row['msg_per_sec']:.2f},"
        f"{row['p95_ms']:.3f},{row['p99_ms']:.3f},{row['lost']},{trend}"
    )
    prev = row
PY
