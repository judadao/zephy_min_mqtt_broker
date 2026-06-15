# mqtt_min_broker

A minimal MQTT v3.1.1 broker written in C. Runs on both **Linux** (for development/testing) and **Zephyr RTOS / ESP32** (production). No external dependencies beyond libc.

## Features

- MQTT v3.1.1: QoS 0 and QoS 1
- Topic wildcard matching (`+` single-level, `#` multi-level)
- Retained message store
- Persistent sessions (`clean_session = 0`)
- Keepalive timeout enforcement with QoS-1 inflight retry (DUP)
- Optional username/password auth (compile-time)
- Optional HTTP status dashboard + REST API (Linux, port 8080)
- Up to 8 concurrent clients; zero heap allocation

Not implemented: QoS 2, TLS, WebSocket.

---

## Quick Start — Linux

Build and test locally with no Zephyr toolchain required.

```bash
# build broker + CLI tool
make -f Makefile.linux

# start broker (listens on :1883)
./mqtt_broker

# subscribe (another terminal — Ctrl-C to stop)
./mqtt_cli sub -t "test/#"

# publish
./mqtt_cli pub -t test/hello -m "world"
./mqtt_cli pub -t test/temp  -m "23.5" -q 1   # QoS 1
./mqtt_cli pub -t test/keep  -m "hi"   -r      # retained

# run automated test suite (12 tests)
./scripts/test_broker.sh
```

### Build variants

```bash
# with HTTP dashboard on :8080
make -f Makefile.linux DASHBOARD=1

# with username/password auth
make -f Makefile.linux AUTH_USER=admin AUTH_PASS=secret

# both
make -f Makefile.linux DASHBOARD=1 AUTH_USER=admin AUTH_PASS=secret
```

---

## mqtt_cli Reference

```
mqtt_cli pub  [-h HOST] [-p PORT] [-i ID] [-u USER] [-P PASS]
              -t TOPIC -m MSG [-q 0|1] [-r]

mqtt_cli sub  [-h HOST] [-p PORT] [-i ID] [-u USER] [-P PASS]
              -t TOPIC [-q 0|1]

mqtt_cli status [-h HOST] [-p PORT]
```

| Option | Default | Description |
|--------|---------|-------------|
| `-h HOST` | `127.0.0.1` | Broker host |
| `-p PORT` | `1883` (MQTT) / `8080` (status) | Port |
| `-i ID` | `mqtt_cli_<pid>` | Client ID |
| `-u USER` | — | Username |
| `-P PASS` | — | Password |
| `-t TOPIC` | — | Topic |
| `-m MSG` | — | Message payload |
| `-q 0\|1` | `0` | QoS level |
| `-r` | off | Set retained flag |

`sub` prints one line per received message: `topic payload`. `status` hits the HTTP dashboard's `/api/status` endpoint and prints the JSON response.

---

## HTTP Dashboard

Enable with `DASHBOARD=1` at build time. Serves on port 8080:

| Endpoint | Description |
|----------|-------------|
| `GET /` | HTML page — connected clients, subscriptions, retained messages, publish form |
| `GET /api/status` | JSON snapshot of all broker state |
| `POST /api/publish` | Publish a message: `{"topic":"…","payload":"…","qos":0}` |

---

## Zephyr / ESP32 Build (Docker)

No Zephyr toolchain or SDK needed on the host.

```bash
# first run builds the Docker image (~15 min, only once)
./docker-build.sh

# subsequent runs (source is mounted, image is reused)
./docker-build.sh

# target a different board
BOARD=esp32s3 ./docker-build.sh

# force image rebuild (e.g. after Dockerfile change)
REBUILD_ENV=1 ./docker-build.sh
```

Firmware lands in `./firmware/` (`zephyr.bin`, `zephyr.elf`).

Flash from host after build:

```bash
west flash
west espressif monitor   # serial console
```

---

## Zephyr Module Usage

Embed this repo as a module in any Zephyr project.

**`west.yml`:**

```yaml
manifest:
  projects:
    - name: mqtt_min_broker
      url: https://github.com/judadao/zephy_min_mqtt_broker
      revision: main
      path: modules/mqtt_min_broker
```

Run `west update` once.

**`prj.conf`** (minimum):

```
CONFIG_MQTT_MIN_BROKER=y
CONFIG_NETWORKING=y
CONFIG_NET_TCP=y
CONFIG_NET_SOCKETS=y
CONFIG_NET_IPV4=y
```

**`main.c`:**

```c
#include "broker.h"

int main(void)
{
    /* bring up WiFi / network first */
    broker_init();
    broker_run(); /* does not return */
    return 0;
}
```

No changes to `CMakeLists.txt` needed.

---

## Configuration

### Kconfig options

| Option | Default | Description |
|--------|---------|-------------|
| `CONFIG_MQTT_AUTH_ENABLED` | n | Require username + password on CONNECT |
| `CONFIG_MQTT_AUTH_USERNAME` | `"admin"` | Required username |
| `CONFIG_MQTT_AUTH_PASSWORD` | `""` | Required password |
| `CONFIG_MQTT_WIFI_SSID` | `""` | WiFi SSID (standalone mode) |
| `CONFIG_MQTT_WIFI_PASSWORD` | `""` | WiFi password (standalone mode) |
| `CONFIG_MQTT_WIFI_DHCP` | y | Use DHCP (or STATIC) |
| `CONFIG_MQTT_HTTP_DASHBOARD` | n | HTTP dashboard (Linux only) |

Copy `prj.conf.template` → `prj.conf` and fill in credentials. `prj.conf` is gitignored.

### Code-level constants (`include/broker.h`, `include/client.h`, `include/packet.h`)

| Constant | Default | Description |
|----------|---------|-------------|
| `MQTT_BROKER_PORT` | `1883` | TCP listen port |
| `MQTT_MAX_CLIENTS` | `8` | Max concurrent connections |
| `CLIENT_STACK_SIZE` | `2048` | Per-client thread stack (bytes, Zephyr) |
| `CLIENT_INFLIGHT_MAX` | `4` | QoS-1 inflight slots per client |
| `MQTT_TOPIC_MAX` | `128` | Max topic length |
| `MQTT_PAYLOAD_MAX` | `512` | Max publish payload |

---

## Architecture

```
src/main.c      entry: WiFi init (Zephyr) → broker_init → broker_run
src/broker.c    TCP listen socket; accept loop; calls client_alloc() per fd
src/client.c    one thread per client; blocking recv loop; MQTT state machine
src/packet.c    MQTT v3.1.1 encode / decode (no dynamic alloc)
src/topic.c     subscription table; wildcard (+/#); retained store; fan-out
src/session.c   persistent session table
src/http.c      HTTP dashboard + REST API (Linux only)
src/wifi.c      WiFi init via net_mgmt (Zephyr only)
tools/mqtt_cli.c  CLI client (Linux)
platform/posix/ POSIX abstraction (pthread, BSD sockets)
platform/zephyr/ Zephyr abstraction (k_mutex, k_thread, zsock_*)
```

PUBLISH data flow:

```
client thread → recv_packet() → handle_publish()
  → topic_publish()
      → topic_match() for each subscription
          → client_send() to each match
```

Concurrency: one thread per client; shared state guarded by per-module mutexes (`pool_lock`, `topic_lock`, `session_lock`).

---

## Test Suite

```bash
# stop mosquitto if running on :1883
sudo systemctl stop mosquitto

./scripts/test_broker.sh
```

Covers: QoS 0/1, wildcard `+` and `#`, retained delivery and clear, multi-subscriber fan-out. Exits 0 when all 12 tests pass.
